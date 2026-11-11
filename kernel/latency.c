/*
 * latency.c - the tunnel round trip shown by the app (VtStatus.latency_ms).
 *
 * Two sources, the better one wins:
 *
 *  - HANDSHAKE (free): proto.c times each handshake we initiate, from the
 *    initiation leaving to its response being processed. Available a few
 *    seconds after connect and refreshed at every rekey (about 2 minutes).
 *  - PING: while the app is open and reading the status (it called
 *    vtGetStatus in the last APP_WINDOW_US) and the tunnel is up and not
 *    paused, the module sends one ICMP echo request a second through the
 *    tunnel, from the tunnel address to g_st.ping_target (chosen in
 *    control.c's load_profile: the profile's first DNS server if it is
 *    inside AllowedIPs, else the .1 of the tunnel's /24). The request is a
 *    plain inner IPv4 packet handed to tnp_encrypt_send, the same path the
 *    interface's frames take; the reply is recognised on the deliver path
 *    (iface_rx_deliver, after decrypt and the AllowedIPs source check) by
 *    its ICMP id and sequence, and consumed: it never reaches net_ps. A
 *    request with no reply within PING_TIMEOUT_US counts as lost. If the
 *    target never answers (ICMP blocked) the module stops after
 *    GIVEUP_LOST pings for this connection and the handshake figure stays.
 *
 * Cost: one 44-byte inner packet (about 80 bytes on the wire) a second each
 * way, only while the app is open. Nothing runs while paused or suspended
 * (latency_tick is gated on g_st.paused, which suspend.c sets at the first
 * suspend event) and nothing here calls net_ps: the request goes to the
 * transport ring like any encrypted frame.
 *
 * Threads: latency_tick runs on the iface TX thread (the only thread that
 * encrypts interface frames, so the ping shares its scratch buffer use);
 * latency_rx_intercept on the lent RX thread; latency_get and
 * latency_app_seen on syscall threads. The state below is under one leaf
 * mutex, never held across tnp_encrypt_send or a net_ps call.
 */
#include "tn_kern.h"

#include <psp2kern/kernel/threadmgr.h>

#include <string.h>

#define PING_ID          0x7654u            /* ICMP identifier, fixed */
#define PING_PAYLOAD     16
#define PING_LEN         (20 + 8 + PING_PAYLOAD)
#define PING_SLOTS       4                  /* requests in flight */
#define PING_INTERVAL_US 950000ull          /* the TX thread wakes about every 1 s */
#define PING_TIMEOUT_US  2000000ull
#define APP_WINDOW_US    3000000ull
#define PING_FRESH_US    5000000ull         /* a reply this recent: source PING */
#define EWMA_RESEED_US   10000000ull        /* older than this: start the average again */
#define GIVEUP_LOST      5
#define LOG_EVERY_US     60000000ull

struct ping_slot {
	int        live;
	uint16_t   seq;
	SceUInt64  sent_us;
};

static SceUID g_lock = -1;
static volatile SceUInt64 g_app_seen_us;    /* last vtGetStatus from the app */

static struct {
	struct ping_slot slot[PING_SLOTS];
	uint16_t   seq;
	uint16_t   ip_id;
	SceUInt64  last_sent_us;
	SceUInt64  last_reply_us;      /* 0 = no reply yet */
	uint32_t   last_us;            /* last sample */
	uint32_t   srtt_us;            /* smoothed (1/8 EWMA, as TCP) */
	uint32_t   replies;            /* this connection */
	uint32_t   lost;               /* this connection */
	int        gave_up;            /* no reply after GIVEUP_LOST: stop for this connection */
	/* log */
	int        logged_src;
	SceUInt64  logged_us;
} L;

static volatile SceUID g_lock_holder = -1;
static void lock(void)   { tn_mutex_lock(g_lock, "latency", &g_lock_holder); }
static void unlock(void) { if (g_lock >= 0) ksceKernelUnlockMutex(g_lock, 1); }

static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* RFC 1071 one's-complement sum over big-endian 16-bit words. */
static uint16_t
csum(const uint8_t *p, uint32_t len)
{
	uint32_t sum = 0;

	while (len > 1) {
		sum += rd16(p);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint32_t)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

void
latency_init(void)
{
	g_lock = ksceKernelCreateMutex("vpnturbo_lat", 0, 0, NULL);
	L.logged_src = -1;
}

void
latency_reset(void)
{
	lock();
	memset(L.slot, 0, sizeof(L.slot));
	L.last_sent_us = 0;
	L.last_reply_us = 0;
	L.last_us = 0;
	L.srtt_us = 0;
	L.replies = 0;
	L.lost = 0;
	L.gave_up = 0;
	unlock();
}

void
latency_app_seen(void)
{
	g_app_seen_us = ksceKernelGetSystemTimeWide();
}

/* A handshake round trip is shown only while that handshake is recent: a
 * session that has not rekeyed for REJECT_AFTER_TIME (180 s) is not
 * known to work, and its old figure must not look current (else the same
 * handshake figure would repeat for minutes with the transport dead). */
#define HS_FRESH_S 180u

/* Current figure, under the lock. */
static void
current(SceUInt64 now, uint32_t hs_rtt_ms, uint32_t hs_age_s, uint32_t *ms, uint8_t *src)
{
	*ms = VT_NEVER;
	*src = VT_LAT_NONE;
	if (g_st.state != VT_STATE_UP)
		return;
	if (!L.gave_up && L.last_reply_us && now - L.last_reply_us < PING_FRESH_US) {
		uint32_t v = (L.srtt_us + 500) / 1000;
		*ms = v ? v : 1;
		*src = VT_LAT_PING;
	} else if (hs_rtt_ms != VT_NEVER && hs_age_s != VT_NEVER && hs_age_s < HS_FRESH_S) {
		*ms = hs_rtt_ms;
		*src = VT_LAT_HANDSHAKE;
	}
}

void
latency_get(uint32_t hs_rtt_ms, uint32_t *ms, uint8_t *source, uint32_t *loss)
{
	SceUInt64 now = ksceKernelGetSystemTimeWide();
	struct tnp_stats st;

	tnp_get_stats(&st);
	/* vtGetStatus (SceShell's UI thread among the callers) never waits:
	 * if the lock is busy this read shows no figure, the next one will. */
	if (g_lock >= 0 && ksceKernelTryLockMutex(g_lock, 1) < 0) {
		*ms = VT_NEVER;
		*source = VT_LAT_NONE;
		*loss = L.lost;
		return;
	}
	current(now, hs_rtt_ms, st.handshake_age_s, ms, source);
	*loss = L.lost;
	unlock();
}

/* Build the echo request for `seq` into p (PING_LEN bytes). */
static void
build_ping(uint8_t *p, uint16_t seq, uint16_t ip_id)
{
	uint8_t *icmp = p + 20;
	int i;

	memset(p, 0, PING_LEN);
	p[0] = 0x45;                        /* IPv4, 20-byte header */
	wr16(p + 2, PING_LEN);
	wr16(p + 4, ip_id);
	p[8] = 64;                          /* TTL */
	p[9] = 1;                           /* ICMP */
	wr32(p + 12, g_st.tunnel_address);
	wr32(p + 16, g_st.ping_target);
	wr16(p + 10, csum(p, 20));

	icmp[0] = 8;                        /* echo request */
	wr16(icmp + 4, PING_ID);
	wr16(icmp + 6, seq);
	for (i = 0; i < PING_PAYLOAD; i++)
		icmp[8 + i] = (uint8_t)(0x40 + i);
	wr16(icmp + 2, csum(icmp, 8 + PING_PAYLOAD));
}

void
latency_tick(void)
{
	static uint8_t pkt[PING_LEN];       /* TX thread only */
	SceUInt64 now = ksceKernelGetSystemTimeWide();
	struct tnp_stats st;
	uint32_t ms;
	uint8_t src;
	int want, i, slot = -1, log_now = 0, gave_up_now = 0;
	uint16_t seq = 0, ip_id = 0;
	uint32_t lost;

	want = g_st.configured && g_st.want && !TN_QUIET() && !g_st.locked &&
	    g_st.state == VT_STATE_UP && g_st.ping_target != 0 &&
	    g_app_seen_us && now - g_app_seen_us < APP_WINDOW_US &&
	    tnp_allowed(g_st.ping_target);

	tnp_get_stats(&st);
	lock();
	if (!want) {
		/* Not pinging: forget requests in flight without counting them as
		 * lost (a pause or a suspend drops them for reasons of its own). */
		memset(L.slot, 0, sizeof(L.slot));
	} else {
		/* Expire requests with no reply. */
		for (i = 0; i < PING_SLOTS; i++) {
			struct ping_slot *s = &L.slot[i];
			if (s->live && now - s->sent_us >= PING_TIMEOUT_US) {
				s->live = 0;
				L.lost++;
				if (L.replies == 0 && L.lost >= GIVEUP_LOST && !L.gave_up) {
					L.gave_up = 1;
					gave_up_now = 1;
				}
			}
		}
		if (!L.gave_up && now - L.last_sent_us >= PING_INTERVAL_US) {
			for (i = 0; i < PING_SLOTS; i++)
				if (!L.slot[i].live) {
					slot = i;
					break;
				}
			if (slot >= 0) {
				seq = ++L.seq;
				ip_id = ++L.ip_id;
				L.slot[slot].live = 1;
				L.slot[slot].seq = seq;
				L.slot[slot].sent_us = now;
				L.last_sent_us = now;
			}
		}
	}
	current(now, st.handshake_rtt_ms, st.handshake_age_s, &ms, &src);
	if ((int)src != L.logged_src ||
	    (src != VT_LAT_NONE && now - L.logged_us >= LOG_EVERY_US)) {
		L.logged_src = src;
		L.logged_us = now;
		log_now = 1;
	}
	lost = L.lost;
	unlock();

	if (gave_up_now)
		klog("latency: no ping replies, using the handshake figure for this connection");
	if (slot >= 0) {
		build_ping(pkt, seq, ip_id);
		if (tnp_encrypt_send(pkt, PING_LEN) < 0) {
			lock();                     /* no session after all: not sent */
			if (L.slot[slot].live && L.slot[slot].seq == seq)
				L.slot[slot].live = 0;
			unlock();
		}
	}
	if (log_now) {
		static const char *const n[] = { "none", "handshake", "ping" };
		if (src == VT_LAT_NONE)
			klog("latency: none");
		else
			klog("latency: %u ms (%s, handshake %u s ago), %u lost", (unsigned)ms, n[src],
			    (unsigned)st.handshake_age_s, (unsigned)lost);
	}
}

int
latency_rx_intercept(const uint8_t *ip, uint32_t len)
{
	const uint8_t *icmp;
	uint32_t ihl, sample;
	uint16_t seq;
	SceUInt64 now;
	int i;

	if (len < 28 || (ip[0] >> 4) != 4 || ip[9] != 1 || g_st.ping_target == 0)
		return 0;
	ihl = (uint32_t)(ip[0] & 0x0f) * 4;
	if (ihl < 20 || len < ihl + 8)
		return 0;
	icmp = ip + ihl;
	if (icmp[0] != 0 || icmp[1] != 0 || rd16(icmp + 4) != PING_ID ||
	    rd32(ip + 12) != g_st.ping_target || rd32(ip + 16) != g_st.tunnel_address)
		return 0;

	/* Ours: consume it whether or not it is still awaited (a late reply). */
	seq = rd16(icmp + 6);
	now = ksceKernelGetSystemTimeWide();
	lock();
	for (i = 0; i < PING_SLOTS; i++) {
		struct ping_slot *s = &L.slot[i];
		if (!s->live || s->seq != seq)
			continue;
		s->live = 0;
		sample = (uint32_t)(now - s->sent_us);
		L.last_us = sample;
		if (!L.last_reply_us || now - L.last_reply_us >= EWMA_RESEED_US)
			L.srtt_us = sample;
		else
			L.srtt_us = L.srtt_us - (L.srtt_us >> 3) + (sample >> 3);
		L.last_reply_us = now;
		L.replies++;
		break;
	}
	unlock();
	return 1;
}
