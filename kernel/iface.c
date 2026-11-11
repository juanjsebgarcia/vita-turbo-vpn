/*
 * iface.c - the other0 virtual interface.
 *
 * Attaches an interface of a type outside 1-4 (net_ps names it other0),
 * reports link-up, sets the MTU with desc[10], answers ARP with a synthetic
 * reply from a fixed fake peer MAC (rule: never our own MAC), and runs the
 * TX thread. The TX thread is woken only by the desc[0x12] kick (never a
 * timer), drains desc[8], and for each Ethernet frame strips the 14-byte
 * header, checks the inner destination against AllowedIPs, and hands the
 * inner IPv4 packet to the protocol core to encrypt and send. Decrypted inner
 * packets come back through iface_rx_deliver, which wraps them in a synthetic
 * Ethernet header and indicates them with desc[7] + desc[9] bit 4.
 *
 * RE-ARM (iface_rearm.h): after a wake's network
 * ready, on every connect and on a resume from pause, the TX thread sends
 * desc[9] link-up again (IFF_RUNNING on, IFF_OACTIVE off) and does one
 * dequeue pass; a kick that arrives while quiet is kept, not dropped. A
 * liveness line every 30 s while UP shows kicks, passes and the ifnet
 * flags, and re-arms if output is blocked although other0 is UP.
 *
 * HOLD (resume lockdown, kill switch): while traffic is held, routes.c keeps
 * the tunnel routes in place so everything arrives here, and this thread
 * drops it (counted in g_st.lock_drops) instead of encrypting it: while
 * LOCKED, after a disconnect with the kill switch on, and (counted only)
 * while there is no session with the kill switch on. A packet to the
 * tunnel's own endpoint UDP port is never encapsulated (it can arrive here
 * only in the moment the endpoint /32 is being re-learnt after a Wi-Fi
 * change; tunnelling it would loop). Seeing one is the loop signature: it
 * is logged (first, then at most every 10 s) and routes.c refreshes the
 * endpoint route at once (routes_ep_suspect, flags only). Limited
 * broadcast or DHCP reaching this interface (the Wi-Fi's own DHCP caught by
 * the tunnel routes) is logged the same way: it means the Wi-Fi cannot
 * renew its address while the routes are in place.
 *
 * DISCONNECTED: with no tunnel wanted and Lockdown off
 * nothing should reach this interface (no route of ours; the plugin
 * removes the address within a second). Whatever still does is dropped
 * and counted as "off", with its addresses logged, so the restore check
 * can tell. With Lockdown on it is held, as above.
 *
 * ARP AND CLONED HOST ENTRIES. On this BSD stack every non-host interface
 * route clones a host entry per destination, resolved by ARP; ours are
 * answered here, so each destination reached through 0/1 + 128/1 leaves a
 * host route on this interface with the fake peer MAC. The addresses
 * answered are tracked (iface_arp_take) so routes.c can delete what is
 * left of them when the tunnel routes go (a 4.4BSD stack does not always
 * delete clones with their parent; one left behind would keep sending that
 * destination into a dead interface after Disconnect).
 *
 * NETWORK DIAGNOSTICS (for troubleshooting): a frame reaching this
 * interface whose destination is on the Wi-Fi subnet, any TCP to or from
 * ports 1337/1338 (a homebrew FTP and command server), and any frame whose
 * source is not the tunnel address are counted and logged (first five, then
 * every 10 s).
 *
 * Lock-order rule: the kick runs under net_ps's giant lock, so it only
 * sets an event flag. The TX thread never holds any of our locks across a
 * net_ps call; it does not call ksceNetSendto at all (the lent TX thread,
 * in SceShell's process, does). So the giant lock is never re-entered.
 */
#include "tn_kern.h"
#include "iface_rearm.h"
#include "tcp_rst.h"
#include "strand.h"

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem/address_space.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>		/* KERNEL_PID */

#include <string.h>

/* taihenModuleUtils (no SDK header). */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

#define NETPS_DRIVER_LIB 0xB2A5C920u
#define NID_IF_ATTACH    0x1ABF937Du

typedef int (*attach_fn)(void *desc);

uint32_t g_desc[D_WORDS] __attribute__((aligned(16)));
const uint8_t k_our_mac[6]  = { 0x02, 0x57, 0x47, 0x54, 0x00, 0x01 };
const uint8_t k_peer_mac[6] = { 0x02, 0x57, 0x47, 0x54, 0x00, 0x02 };

static SceUID g_evf = -1;
static SceUID g_tx_thread = -1;
static volatile int g_run;      /* TX thread may call desc[8] */

#define EV_KICK   1u
#define EV_QUIT   2u
#define EV_STRAND 4u            /* strand_arm: install the hook, start polling */

/* Re-arm (iface_rearm.h): requests from any thread, served by the TX thread. */
static struct txgate g_gate;
static const char *volatile g_rearm_why = "start";
static volatile uint32_t c_kicks;       /* desc[0x12] calls (net_ps, giant lock) */
static uint32_t c_loops, c_passes, c_empty, c_rearms;   /* TX thread only */
static uint32_t g_kick_base;            /* c_kicks at the last re-arm */
/* Perf: uptime (us, low 32 bits, | 1) of the
 * first kick since the last dequeue pass, set by the kick and taken by the
 * pass, so the 30 s line shows how long a frame waits for this thread. */
static volatile uint32_t g_kick_t;
static uint32_t p_wake_us, p_wake_n, p_wake_max, p_pass_max;   /* TX thread only */
static uint32_t p_seal_us, p_seal_n, p_seal_max;               /* TX thread only */
/* Kick -> dequeue pass, and the seal, as histograms (lh_*). */
static struct lhist h_ifw, h_seal;                             /* TX thread only */
/* Timing counters: the hand-off of a decrypted packet, split into
 * indicate (packet alloc, copy, desc[7]) and the RX-ready signal (desc[9],
 * which wakes net_ps's soft thread); lent RX thread only. g_dlv_us sums
 * this call's deliver time for transport.c (iface_rx_take_us). */
static struct lhist h_ind, h_sig;
static uint32_t g_dlv_us;

uint32_t
iface_rx_take_us(void)
{
	uint32_t v = g_dlv_us;
	g_dlv_us = 0;
	return v;
}
static volatile uint32_t p_sig;         /* RX-ready signals for decrypted packets */
static const char *g_kick_watch;        /* set: log the first kick after it */
static SceUInt64 g_rearm_at;

/* ---- packet layout -------------------------------------------------------- */

static inline uint8_t *pkt_seg(void *pkt) { return *(uint8_t **)((uint8_t *)pkt + 0x20); }
static inline uint8_t *pkt_data(void *pkt) { return *(uint8_t **)(pkt_seg(pkt) + 8); }
static inline uint32_t pkt_len(void *pkt) { return *(uint32_t *)((uint8_t *)pkt + 8); }

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* Hand a frame up to net_ps; ownership passes at desc[7]. Thread context. */
static int
indicate(const uint8_t *frame, uint32_t len)
{
	void *pkt = SVC(alloc_pkt_fn, D_ALLOC_PKT)(g_desc, len, 4);
	uint8_t *seg, *d;

	if (!pkt)
		return 0;
	seg = pkt_seg(pkt);
	d = *(uint8_t **)(seg + 8);
	memcpy(d, frame, len);
	*(uint8_t **)(seg + 0xc) = d + len;
	*(uint32_t *)((uint8_t *)pkt + 8) = len;
	SVC(rx_fn, D_RX)(g_desc, pkt);
	return 1;
}

static uint8_t g_rx_frame[2048];   /* deliver sink staging (lent RX thread) */
static uint8_t g_arp_reply[64];    /* TX thread only */
static uint8_t g_rst_frame[14 + TCP_RST_LEN];   /* TX thread only */

/* Counters for the log (TX thread and the lent RX thread). */
static volatile uint32_t c_frames, c_arp, c_arp_rep, c_ip, c_not_allowed,
	c_no_session, c_rx_deliver, c_rx_fail, c_held, c_loop, c_dhcp, c_off,
	c_lan, c_ftp, c_src, c_rst, c_rst_strand;

/* ---- ARP-answered addresses (see the file comment) ------------------------ */

#define ARP_TRACK 128
static uint32_t g_arp_ips[ARP_TRACK];
static int g_arp_n;
static unsigned g_arp_lost;
static SceUID g_arp_lock = -1;     /* a leaf: never held across a net_ps call */
static volatile SceUID g_arp_holder = -1;

static void
arp_note(uint32_t ip)
{
	int i;

	if (g_arp_lock < 0 || ip == 0)
		return;
	tn_mutex_lock(g_arp_lock, "ARP", &g_arp_holder);
	for (i = 0; i < g_arp_n && g_arp_ips[i] != ip; i++)
		;
	if (i == g_arp_n) {
		if (g_arp_n < ARP_TRACK)
			g_arp_ips[g_arp_n++] = ip;
		else
			g_arp_lost++;
	}
	ksceKernelUnlockMutex(g_arp_lock, 1);
}

int
iface_arp_take(uint32_t *out, int cap, unsigned *lost)
{
	int n;

	*lost = 0;
	if (g_arp_lock < 0)
		return 0;
	tn_mutex_lock(g_arp_lock, "ARP", &g_arp_holder);
	n = g_arp_n < cap ? g_arp_n : cap;
	memcpy(out, g_arp_ips, (unsigned)n * sizeof(out[0]));
	/* What did not fit stays for the next take. */
	memmove(g_arp_ips, g_arp_ips + n, (unsigned)(g_arp_n - n) * sizeof(g_arp_ips[0]));
	g_arp_n -= n;
	*lost = g_arp_lost;
	g_arp_lost = 0;
	ksceKernelUnlockMutex(g_arp_lock, 1);
	return n;
}

void
iface_counts(uint32_t *frames, uint32_t *off, uint32_t *held)
{
	*frames = c_frames;
	*off = c_off;
	*held = c_held;
}

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

/* First five of a kind, then at most every 10 s (per kind). */
static int
tell(volatile uint32_t *count, SceUInt64 *told)
{
	SceUInt64 t = ksceKernelGetSystemTimeWide();
	uint32_t c = ++*count;

	if (c <= 5 || t - *told > 10000000ull) {
		*told = t;
		return 1;
	}
	return 0;
}

/* The network diagnostics (file comment) for one outbound IPv4 frame. */
static void
frame_diag(const uint8_t *ip, uint32_t len, const char *fate)
{
	static SceUInt64 t_lan, t_ftp, t_src;
	uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
	uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
	uint32_t wl = g_st.wlan_address, ta = g_st.tunnel_address;
	unsigned pfx = g_st.wlan_prefix;
	uint16_t sp = 0, dp = 0;
	int l4 = (ip[9] == 6 || ip[9] == 17) && ihl >= 20 && len >= ihl + 4 &&
	    (rd16(ip + 6) & 0x1fff) == 0;

	if (l4) {
		sp = rd16(ip + ihl);
		dp = rd16(ip + ihl + 2);
	}
	if (wl && pfx >= 8 && pfx <= 30 && ((dst ^ wl) >> (32 - pfx)) == 0 &&
	    dst != 0xffffffffu && tell(&c_lan, &t_lan))
		klog("iface: LAN host %u.%u.%u.%u in tunnel if: proto %u %u->%u src %s, %s (#%u)",
		    IP4(dst), (unsigned)ip[9], sp, dp, src == ta ? "tunnel" : src == wl ? "WI-FI" : "other",
		    fate, c_lan + 0u);
	if (ip[9] == 6 && l4 && (sp == 1337 || sp == 1338 || dp == 1337 || dp == 1338) &&
	    tell(&c_ftp, &t_ftp))
		klog("iface: TCP %u->%u %u.%u.%u.%u in tunnel if: flags 0x%02X len %u, %s (#%u)",
		    sp, dp, IP4(dst), len >= ihl + 14 ? (unsigned)ip[ihl + 13] : 0u, (unsigned)len,
		    fate, c_ftp + 0u);
	if (ta && src != ta && src != 0 && tell(&c_src, &t_src))
		klog("iface: src %s %u.%u.%u.%u -> %u.%u.%u.%u proto %u in tunnel if, %s (#%u)",
		    src == wl ? "WI-FI" : "other", IP4(src), IP4(dst), (unsigned)ip[9],
		    fate, c_src + 0u);
}

/* Limited broadcast, or UDP to or from the DHCP ports. */
static int
dhcp_or_bcast(const uint8_t *ip, uint32_t len)
{
	uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;
	uint16_t sp, dp;

	if (rd32(ip + 16) == 0xffffffffu)
		return 1;
	if (ip[9] != 17 || ihl < 20 || len < ihl + 4 || (rd16(ip + 6) & 0x1fff) != 0)
		return 0;
	sp = rd16(ip + ihl);
	dp = rd16(ip + ihl + 2);
	return (sp == 68 && dp == 67) || (sp == 67 && dp == 68);
}

/* 1 if the inner packet is UDP to the tunnel's own endpoint and port. */
static int
own_transport(const uint8_t *ip, uint32_t len)
{
	uint32_t ihl = (uint32_t)(ip[0] & 0x0f) * 4;

	/* The configured endpoint, or the address a handshake roamed to. */
	if (ip[9] != 17 || !transport_is_own(rd32(ip + 16), 0))
		return 0;
	if ((rd16(ip + 6) & 0x1fff) != 0)
		return 1;               /* a later fragment to the endpoint: drop too */
	return ihl >= 20 && len >= ihl + 4 && transport_is_own(rd32(ip + 16), rd16(ip + ihl + 2));
}

/* ARP: answer every request except our own announcement, from the fake peer
 * MAC, addressed to our IP and MAC. */
static void
handle_arp(const uint8_t *f, uint32_t len)
{
	const uint8_t *a = f + 14;
	uint8_t *r = g_arp_reply, *ra;
	uint32_t spa, tpa;

	if (len < 14 + 28 || rd16(a + 6) != 1)
		return;
	spa = rd32(a + 14);
	tpa = rd32(a + 24);
	if (spa == tpa)
		return;                 /* gratuitous: do not answer */
	arp_note(tpa);                  /* a host entry net_ps clones on this interface */
	ra = r + 14;
	memcpy(r, k_our_mac, 6);
	memcpy(r + 6, k_peer_mac, 6);
	wr16(r + 12, 0x0806);
	memcpy(ra, a, 6);               /* htype, ptype, hlen, plen */
	wr16(ra + 6, 2);                /* reply */
	memcpy(ra + 8, k_peer_mac, 6);  /* sha = fake peer */
	wr32(ra + 14, tpa);             /* spa = the address asked about */
	memcpy(ra + 18, a + 8, 6);      /* tha = the asker */
	wr32(ra + 24, spa);             /* tpa */
	/* Hand the reply up and wake net_ps's soft thread: without the RX-ready
	 * bit the reply sat in the queue until some other packet arrived, so
	 * address resolution through the tunnel stalls. */
	if (indicate(r, 14 + 28)) {
		c_arp_rep++;
		SVC(event_fn, D_EVENT)(g_desc, 0, 0, SOFT_RX_READY, NULL, 0);
	}
}

/* ---- deliver sink: decrypted inner IPv4 -> net_ps ----------------------- */

/*
 * RX-ready batching. net_ps's soft thread drains the whole RX queue per
 * bit-4 event (the Wi-Fi driver signals once per RX
 * batch), so one signal per packet only woke it again for packets it had
 * already taken. Within an RX step the first packet is signalled at once
 * (the stack starts on it, and the ACK it sends, while we decrypt the
 * rest), then every RX_SIGNAL_EVERY-th, and whatever is left at the end of
 * the step (iface_rx_batch(0)): nothing indicated waits past its step.
 * Outside a batch (no other caller today) every packet is signalled.
 */
#define RX_SIGNAL_EVERY 8
static int g_rx_batch, g_rx_first;     /* lent RX thread only */
static uint32_t g_rx_unsig;            /* indicated since the last signal */

static void
rx_signal(void)
{
	uint32_t t0 = (uint32_t)ksceKernelGetSystemTimeWide(), dt;

	g_rx_unsig = 0;
	p_sig++;
	SVC(event_fn, D_EVENT)(g_desc, 0, 0, SOFT_RX_READY, NULL, 0);
	dt = (uint32_t)ksceKernelGetSystemTimeWide() - t0;
	lh_add(&h_sig, dt);
	g_dlv_us += dt;
}

void
iface_rx_batch(int on)
{
	g_rx_batch = g_rx_first = on;
	if (!on && g_rx_unsig)
		rx_signal();
}

void
iface_rx_deliver(void *ctx, const void *ip, uint32_t len)
{
	uint8_t *f = g_rx_frame;

	(void)ctx;
	if (latency_rx_intercept(ip, len))
		return;                 /* a reply to the module's own ping (latency.c) */
	if (!g_st.attached || len == 0 || len + 14 > sizeof(g_rx_frame)) {
		c_rx_fail++;            /* counted: the log must show every drop */
		return;
	}
	memcpy(f, k_our_mac, 6);
	memcpy(f + 6, k_peer_mac, 6);
	wr16(f + 12, 0x0800);
	memcpy(f + 14, ip, len);
	uint32_t t0 = (uint32_t)ksceKernelGetSystemTimeWide();
	int ok = indicate(f, len + 14);
	uint32_t dt = (uint32_t)ksceKernelGetSystemTimeWide() - t0;
	lh_add(&h_ind, dt);
	g_dlv_us += dt;
	if (ok) {
		c_rx_deliver++;
		g_rx_unsig++;
		if (!g_rx_batch || g_rx_first || g_rx_unsig >= RX_SIGNAL_EVERY) {
			g_rx_first = 0;
			rx_signal();
		}
	} else {
		c_rx_fail++;
	}
}

/* ---- TX thread ----------------------------------------------------------- */

/* tnp_encrypt_send, timed for the perf line (the protocol lock wait, the
 * seal and the copy into the send ring). */
static int
seal(const uint8_t *ip, uint32_t len)
{
	uint32_t t0 = (uint32_t)ksceKernelGetSystemTimeWide(), dt;
	int r = tnp_encrypt_send(ip, len);

	dt = (uint32_t)ksceKernelGetSystemTimeWide() - t0;
	lh_add(&h_seal, dt);
	p_seal_us += dt;
	p_seal_n++;
	if (dt > p_seal_max)
		p_seal_max = dt;
	return r;
}

/* Hand up the reset that answers the IPv4 TCP packet `ip` (tcp_rst.c), as
 * from the far end. 1 if it was indicated. TX thread. */
static int
rst_inject(const uint8_t *ip, uint32_t len)
{
	if (!tcp_rst_for(ip, len, g_rst_frame + 14))
		return 0;
	memcpy(g_rst_frame, k_our_mac, 6);
	memcpy(g_rst_frame + 6, k_peer_mac, 6);
	wr16(g_rst_frame + 12, 0x0800);
	if (!indicate(g_rst_frame, sizeof(g_rst_frame)))
		return 0;
	/* Wake net_ps's soft thread, as for an ARP reply: without the
	 * RX-ready bit the reset waits for some other packet. */
	SVC(event_fn, D_EVENT)(g_desc, 0, 0, SOFT_RX_READY, NULL, 0);
	return 1;
}

/* Answer a stranded TCP segment with a reset (see tx_frame). TX thread. */
static void
rst_stranded(const uint8_t *ip, uint32_t len)
{
	static SceUInt64 told;

	if (!rst_inject(ip, len))
		return;
	if (tell(&c_rst, &told))   /* counts it */
		klog("iface: reset a connection opened before the tunnel: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (#%u)",
		    IP4(rd32(ip + 12)), (unsigned)rd16(ip + (ip[0] & 0x0F) * 4),
		    IP4(rd32(ip + 16)), (unsigned)rd16(ip + (ip[0] & 0x0F) * 4 + 2), c_rst + 0u);
}

/*
 * The other direction (strand.h): connections the tunnel carried, still
 * sending over the Wi-Fi with the tunnel address after it went away. The
 * hook in the stack's transmit routine caught their headers; each is answered
 * with a reset up the console's own stack so the application reconnects
 * directly. Polled at 250 ms only while the window is open or entries wait.
 * A pause is not a reason to stay away (the interface is attached and up; the
 * routes are removed through the stack at the same moment); a suspend or a
 * wait for the network is, and drops what was caught.
 */
static void
strand_service(void)
{
	static SceUInt64 told;
	uint8_t ip[STRAND_IP_MAX];
	uint32_t n;
	int quiet;

	if (!strand_pending())
		return;
	quiet = !g_run || !g_st.attached || TN_NO_NETPS();
	strand_poll(quiet);
	if (quiet)
		return;
	while ((n = strand_next(ip)) != 0) {
		wd_phase(WD_IFTX, "strand reset");
		if (!rst_inject(ip, n))
			continue;
		if (tell(&c_rst_strand, &told))
			klog("iface: reset a stranded connection (tunnel gone): %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (#%u)",
			    IP4(rd32(ip + 12)), (unsigned)rd16(ip + (ip[0] & 0x0F) * 4),
			    IP4(rd32(ip + 16)), (unsigned)rd16(ip + (ip[0] & 0x0F) * 4 + 2), c_rst_strand + 0u);
	}
}

/* One outbound frame net_ps handed over (desc[8]); freed by the caller. */
static void
tx_frame(const uint8_t *f, uint32_t len)
{
	c_frames++;
	if (len < 14)
		return;
	{
		uint16_t et = rd16(f + 12);
		if (et == 0x0806) {
			c_arp++;
			handle_arp(f, len);
		} else if (et == 0x0800 && len > 14 + 20) {
			const uint8_t *ip = f + 14;
			uint32_t inner = len - 14;
			uint32_t dst = rd32(ip + 16);
			c_ip++;
			if (dhcp_or_bcast(ip, inner)) {
				static SceUInt64 told;
				SceUInt64 t = ksceKernelGetSystemTimeWide();
				if (c_dhcp++ == 0 || t - told > 10000000ull) {
					told = t;
					klog("iface: DHCP or broadcast reached the tunnel interface (%u so far, %s)",
					    c_dhcp, g_st.locked || !g_st.want ? "dropped" : "tunnelled");
				}
			}
			uint32_t src = rd32(ip + 12), ta = g_st.tunnel_address;
			int foreign = ta && src != ta;
			frame_diag(ip, inner, !g_st.want ? (g_st.lockdown ? "held: Lockdown" : "dropped: tunnel off") :
			    g_st.locked ? "held: lockdown" : g_st.paused ? "dropped: paused" :
			    foreign ? "dropped: not the tunnel address" : "tunnelled");
			if (!g_st.want && !g_st.lockdown) {
				/* Off: nothing of ours
				 * should send traffic here. */
				static SceUInt64 told;
				if (tell(&c_off, &told))
					klog("iface: off: %u.%u.%u.%u -> %u.%u.%u.%u proto %u dropped, tunnel off (#%u)",
					    IP4(rd32(ip + 12)), IP4(dst), (unsigned)ip[9], c_off + 0u);
			} else if (!tnp_allowed(dst)) {
				c_not_allowed++;
			} else if (own_transport(ip, inner)) {
				static SceUInt64 told;
				SceUInt64 t = ksceKernelGetSystemTimeWide();
				if (c_loop++ == 0 || t - told > 10000000ull) {
					told = t;
					klog("iface: LOOP: the tunnel's own UDP to the endpoint reached the tunnel "
					    "interface (%u so far): endpoint route missing, repairing", c_loop);
					routes_ep_suspect("tunnel UDP looped into the tunnel");
				}
			} else if (g_st.paused) {
				c_off++;                /* paused: traffic goes direct, never here */
			} else if (g_st.locked || !g_st.want) {
				/* Held (resume lockdown, or Lockdown mode
				 * with the tunnel off). */
				c_held++;
				g_st.lock_drops++;
			} else if (foreign) {
				/* A TCP segment of a connection opened
				 * before the tunnel came up (it keeps the
				 * Wi-Fi address as its source): answered
				 * with a reset, as from the far end, back up
				 * the console's own stack, so the
				 * application opens a new connection (with
				 * the tunnel address) at once instead of
				 * waiting on one that can never get through
				 * (tcp_rst.c). Nothing is sent. */
				rst_stranded(ip, inner);
				/* Sourced from another address (the
				 * stack's replies to traffic that came in
				 * over Wi-Fi, e.g. port unreachables to a
				 * server still sending to a closed tunnel
				 * port): the server drops anything not from
				 * the tunnel address, so it is dropped
				 * here, unsent. Sealed, it would count as
				 * unanswered data and set off the no-reply
				 * rule (proto.c) every 15 s while idle. */
			} else if (seal(ip, inner) < 0) {
				c_no_session++;
				if (TN_KILL())
					g_st.lock_drops++;   /* held, not leaked */
				/* No usable session: the packet is
				 * dropped (the protocol tolerates loss) and
				 * the core has started a handshake if
				 * none is running, at most one
				 * initiation per 5 s (proto.c,
				 * session_unusable). Calling
				 * tnp_start_handshake here for every
				 * packet restarted the attempt count
				 * and, while UP, flooded initiations. */
			}
		}
		/* Anything else (other ethertypes, runts) is counted in
		 * c_frames only. */
	}
}

void iface_hb_counts(uint32_t *kicks, uint32_t *frames) { *kicks = c_kicks; *frames = c_frames; }

/* One desc[8] pass until NULL; returns the frames taken. */
static uint32_t
drain(void)
{
	uint32_t n = 0, kt;
	void *pkt;

	c_passes++;
	kt = __atomic_exchange_n(&g_kick_t, 0, __ATOMIC_ACQ_REL);
	if (kt) {
		uint32_t dt = ((uint32_t)ksceKernelGetSystemTimeWide() | 1u) - kt;
		p_wake_us += dt;
		p_wake_n++;
		lh_add(&h_ifw, dt);
		if (dt > p_wake_max)
			p_wake_max = dt;
	}
	wd_phase(WD_IFTX, "desc[8] dequeue");
	while ((pkt = SVC(tx_deq_fn, D_TX_DEQ)(g_desc)) != NULL) {
		wd_phase(WD_IFTX, "frame");
		tx_frame(pkt_data(pkt), pkt_len(pkt));
		n++;
		SVC(free_pkt_fn, D_FREE_PKT)(g_desc, pkt);
		wd_phase(WD_IFTX, "desc[8] dequeue");
	}
	if (n == 0)
		c_empty++;
	if (n > p_pass_max)
		p_pass_max = n;
	return n;
}

/*
 * other0's ifnet flags (iface_rearm.h), read only, never written.
 *
 * kread32 reads one aligned kernel word only if its address is mapped
 * (ksceKernelVAtoPA; a translation query, it cannot fault). The TX thread and
 * control.c run in the kernel process, so a mapped address is kernel memory.
 *
 * ifnet_probe reads the chain once (attach, then again at most once per
 * liveness tick while it has not passed), logs every word it read, and
 * caches the ifnet only if ifprobe_verdict accepts it. ifnet_flags reads the
 * cached ifnet's flags, and only while desc[0xd] still holds the container
 * the probe accepted. Nothing else enables the self-heal.
 */
static uint32_t g_if_cont, g_if_ifp;     /* accepted chain; 0 = none */
static volatile int g_if_probing;
static unsigned g_if_probes;

static int
kread32(uintptr_t a, uint32_t *out)
{
	uintptr_t pa;

	if (a == 0 || (a & 3))
		return 0;
	if (ksceKernelVAtoPA((const void *)a, &pa) < 0)
		return 0;
	*out = *(volatile const uint32_t *)a;
	return 1;
}

static void
ifnet_probe(const char *when)
{
	struct ifprobe p;
	const char *why;
	uint32_t w[4], x2c = 0, x34 = 0, x40 = 0;
	char nm[17];
	int i;

	if (__atomic_exchange_n(&g_if_probing, 1, __ATOMIC_ACQ_REL))
		return;
	memset(&p, 0, sizeof(p));
	p.desc = (uint32_t)(uintptr_t)g_desc;
	p.cont = g_desc[0xd];
	if (p.cont && !(p.cont & 3)) {
		p.c0_ok = kread32(p.cont, &p.c0);
		p.ifp_ok = p.c0_ok && kread32(p.cont + 0x30, &p.ifp);
	}
	if (p.ifp_ok) {
		p.i0_ok = kread32(p.ifp, &p.i0);
		p.name_ok = p.i0_ok;
		for (i = 0; i < 4 && p.name_ok; i++)
			p.name_ok = kread32(p.ifp + 0x14 + 4 * i, &w[i]);
		if (p.name_ok)
			memcpy(p.name, w, sizeof(p.name));
		p.flags_ok = p.i0_ok && kread32(p.ifp + 0x30, &p.flags);
		if (p.i0_ok) {
			(void)kread32(p.ifp + 0x2c, &x2c);   /* if_index */
			(void)kread32(p.ifp + 0x34, &x34);
			(void)kread32(p.ifp + 0x40, &x40);   /* MTU */
		}
	}
	for (i = 0; i < 16; i++) {
		char c = p.name_ok ? p.name[i] : 0;
		if (c == 0)
			break;
		nm[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	nm[i] = 0;
	why = ifprobe_verdict(&p);
	g_if_probes++;
	if (!why) {
		g_if_ifp = p.ifp;
		__atomic_store_n(&g_if_cont, p.cont, __ATOMIC_RELEASE);
	}
	/* Every probe that passes, and the first five that fail, are logged
	 * (four lines: a klog line holds 112 bytes; "?" = not mapped/read). */
	if (!why || g_if_probes <= 5) {
		klog("iface: ifnet probe (%s): desc 0x%08X cont 0x%08X%s ifnet 0x%08X%s",
		    when, (unsigned)p.desc, (unsigned)p.cont, p.c0_ok ? "" : "?",
		    (unsigned)p.ifp, p.ifp_ok ? "" : "?");
		klog("iface: ifnet probe: cont[0] 0x%08X ifnet[0] 0x%08X%s name \"%s\"%s",
		    (unsigned)p.c0, (unsigned)p.i0, p.i0_ok ? "" : "?", nm,
		    p.name_ok ? "" : "?");
		klog("iface: ifnet probe: +2c 0x%X flags(+30) 0x%X%s +34 0x%X mtu(+40) %u",
		    (unsigned)x2c, (unsigned)p.flags, p.flags_ok ? "" : "?",
		    (unsigned)x34, (unsigned)x40);
		if (why)
			klog("iface: ifnet probe: REJECTED, self-heal off: %s", why);
		else
			klog("iface: ifnet probe: OK, self-heal on");
	}
	__atomic_store_n(&g_if_probing, 0, __ATOMIC_RELEASE);
}

/* 1 and *fl = other0's ifnet flags if the probe accepted the chain and it
 * still holds; 0 otherwise (unknown: no self-heal). */
static int
ifnet_flags(uint32_t *fl)
{
	uint32_t cont = __atomic_load_n(&g_if_cont, __ATOMIC_ACQUIRE);

	*fl = 0;
	if (!g_st.attached || cont == 0 || g_desc[0xd] != cont)
		return 0;
	return kread32(g_if_ifp + 0x30, fl);
}

/* The "perf if" line, every 30 s with the liveness line:
 * differences since the last one. */
/*
 * The tunnel interface can get its address while it is already UP: with iface_mode keep the plugin swaps the address on a
 * profile with another tunnel address without taking the interface down, so
 * net_ps never calls desc[0x10] and iface_up, cleared by the new profile,
 * stayed 0 - no tunnel routes, the traffic left over the Wi-Fi while the app
 * said Protected. routes.c's change monitor calls this on RTM_NEWADDR of the
 * tunnel address: the same as drv_init's flags if the interface is UP (or
 * its flags cannot be read: the address was just added). Lent RX thread.
 */
void
iface_addr_added(void)
{
	uint32_t fl;
	int known;

	if (g_st.iface_up)
		return;
	known = ifnet_flags(&fl);
	if (known && !(fl & IFF_UP_))
		return;                 /* down: desc[0x10] follows when it comes up */
	klog("iface: tunnel address on an interface already up%s: tunnel routes go ahead",
	    known ? "" : " (flags unknown)");
	g_st.iface_gen++;
	g_st.iface_up = 1;
	routes_mark_dirty();
}

static void
iface_perf_log(void)
{
	static uint32_t o_kicks, o_passes, o_frames, o_rx, o_sig, o_wus, o_wn, o_sus, o_sn;
	uint32_t kicks = c_kicks, passes = c_passes, frames = c_frames, rx = c_rx_deliver, sig = p_sig;
	uint32_t wn = p_wake_n - o_wn, sn = p_seal_n - o_sn;

	klog("perf if: kicks %u passes %u frames %u max %u/pass; rx %u signals %u",
	    kicks - o_kicks, passes - o_passes, frames - o_frames, p_pass_max, rx - o_rx, sig - o_sig);
	klog("perf if: wake %u us max %u; seal %u us max %u",
	    wn ? (unsigned)((p_wake_us - o_wus) / wn) : 0u, p_wake_max,
	    sn ? (unsigned)((p_seal_us - o_sus) / sn) : 0u, p_seal_max);
	o_kicks = kicks; o_passes = passes; o_frames = frames; o_rx = rx; o_sig = sig;
	o_wus = p_wake_us; o_wn = p_wake_n; o_sus = p_seal_us; o_sn = p_seal_n;
	p_pass_max = p_wake_max = p_seal_max = 0;
	{
		static struct lhist o_ifw, o_seal;
		lh_log("lat if wake", &h_ifw, &o_ifw);
		lh_log("lat if seal", &h_seal, &o_seal);
		static struct lhist o_ind, o_sig;
		lh_log("lat rx indicate", &h_ind, &o_ind);
		lh_log("lat rx signal", &h_sig, &o_sig);
	}
	transport_perf_log();
}

/* Every 5 s while anything moves: where traffic goes. Every 30 s while the
 * tunnel is UP, a liveness line even if nothing moved (and the self-heal:
 * iface_rearm.h). */
static void
tx_log_tick(void)
{
	static SceUInt64 next, next_alive;
	static int was_up;
	static uint32_t last;
	SceUInt64 now = ksceKernelGetSystemTimeWide();
	uint32_t sum = c_frames + c_rx_deliver + c_rx_fail;

	if (now >= next && sum != last) {
		next = now + 5000000ull;
		last = sum;
		klog("iface: frames %u arp %u/%u ip %u not-allowed %u no-session %u held %u off %u (dhcp %u) loop %u | rx %u fail %u",
		    c_frames, c_arp_rep, c_arp, c_ip, c_not_allowed, c_no_session,
		    c_held, c_off, c_dhcp, c_loop, c_rx_deliver, c_rx_fail);
		if (c_lan || c_ftp || c_src || c_rst)
			klog("iface: diag: LAN-subnet %u, port 1337/1338 %u, non-tunnel source %u, resets %u",
			    c_lan, c_ftp, c_src, c_rst);
		if (c_rst_strand || strand_st.caught || strand_st.dropped || strand_st.unreadable) {
			uint32_t caught, dropped, unreadable, lost;

			strand_counts(&caught, &dropped, &unreadable, &lost);
			klog("iface: diag: stranded: reset %u, caught %u, ring full %u, unreadable %u, given up %u",
			    c_rst_strand + 0u, caught, dropped, unreadable, lost);
		}
	}
	if (g_st.state != VT_STATE_UP || TN_QUIET()) {
		/* Leaving UP: the perf lines for what is left of the window, so
		 * a session shorter than 30 s is measured too */
		if (was_up && !TN_QUIET())
			iface_perf_log();
		was_up = 0;
		next_alive = now + 30000000ull;   /* the first line 30 s into UP */
		return;
	}
	was_up = 1;
	if (now >= next_alive) {
		uint32_t fl;
		int known;

		next_alive = now + 30000000ull;
		if (!g_if_cont || g_desc[0xd] != g_if_cont)
			ifnet_probe("liveness");
		known = ifnet_flags(&fl);
		/* Two lines: a klog line holds 112 bytes. */
		klog("iface: alive: loops %u kicks %u passes %u (empty %u) re-arms %u",
		    c_loops, c_kicks + 0u, c_passes, c_empty, c_rearms);
		klog("iface: alive: frames %u rx %u fail %u | if flags %s0x%X%s",
		    c_frames, c_rx_deliver, c_rx_fail, known ? "" : "unknown ",
		    (unsigned)fl,
		    known && ifflags_blocked(fl) ? " BLOCKED: re-arming" :
		    known && g_st.iface_up && !(fl & IFF_UP_) ? " (not UP, init ran)" : "");
		if (known && ifflags_blocked(fl) && g_st.iface_up)
			iface_rearm("self-heal");
		iface_perf_log();
	}
}

static int
tx_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	for (;;) {
		unsigned int bits = 0;
		/* Wake to notice an abandoned suspend; every 250 ms only while the
		 * stranded-connection window is open (strand.h). */
		SceUInt32 timeout = strand_pending() ? 250 * 1000 : 1000 * 1000;
		unsigned act;

		ksceKernelWaitEventFlag(g_evf, EV_KICK | EV_QUIT | EV_STRAND,
		    SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
		if (bits & EV_QUIT)
			break;
		g_wakes.iftx++;
		wd_check(WD_IFTX);              /* the timer thread, and the others */
		wd_begin(WD_IFTX);
		c_loops++;
		tx_log_tick();
		strand_service();
		/* Latency ping (latency.c): at most one a second, only while the
		 * app is open and the tunnel up; gated on g_st.paused itself.
		 * It only encrypts into the transport ring, never calls net_ps. */
		latency_tick();
		/* Never desc[8] while suspending or paused; a kick seen
		 * then is kept for later (iface_rearm.h). */
		act = txgate_step(&g_gate, (bits & EV_KICK) != 0,
		    g_run && g_st.attached && !TN_QUIET());
		if (g_kick_watch && c_kicks != g_kick_base) {
			klog("iface: first start-TX kick after the re-arm (%s), %u ms later",
			    g_kick_watch, (unsigned)((ksceKernelGetSystemTimeWide() - g_rearm_at) / 1000));
			g_kick_watch = NULL;
		}
		if (act == TXA_NONE) {
			wd_end(WD_IFTX);
			continue;
		}
		tn_lock_update();
		if (act & TXA_LINKUP) {
			const char *why = g_rearm_why;
			uint32_t fl, n;
			int known = ifnet_flags(&fl);

			/* desc[9] link up: IFF_RUNNING on, IFF_OACTIVE off
			 * (thread context, nothing held; takes no lock). */
			wd_phase(WD_IFTX, "desc[9] link up");
			SVC(event_fn, D_EVENT)(g_desc, 0, 0, SOFT_LINK_UP, NULL, 0);
			c_rearms++;
			n = drain();
			g_rearm_at = ksceKernelGetSystemTimeWide();
			g_kick_base = c_kicks;
			g_kick_watch = why;
			klog("iface: re-arm (%s): link up sent, if flags were %s0x%X%s, %u waiting, kicks %u",
			    why, known ? "" : "unknown ", (unsigned)fl,
			    known && ifflags_blocked(fl) ? " BLOCKED" : "",
			    n, c_kicks + 0u);
		} else {
			drain();
		}
		wd_end(WD_IFTX);
	}
	return ksceKernelExitDeleteThread(0);
}

/* ---- driver callbacks (net_ps calls these with its giant lock held) ----- */

static int drv_init(void *desc, int arg)
{
	(void)desc; (void)arg;
	klog("iface: desc[0x10] init");
	SVC(event_fn, D_EVENT)(g_desc, 0, 0, SOFT_LINK_UP, NULL, 0);   /* no lock */
	/* The plugin brings the interface up right after giving it its address:
	 * now the tunnel routes can point at it (routes.c). A new generation
	 * makes routes.c re-add tunnel routes made before an interface bounce
	 * (they were bound to the address the interface had then). Flags
	 * only: this runs under net_ps's giant lock. */
	g_st.iface_gen++;
	g_st.iface_up = 1;
	routes_mark_dirty();
	return 0;
}

static int drv_stop(void *desc, int arg) { (void)desc; (void)arg; g_st.iface_up = 0; klog("iface: desc[0x11] stop"); return 0; }
static int drv_mcast(void *desc, const void *macs, int len) { (void)desc; (void)macs; (void)len; return 0; }
static int drv_ioctl(void *desc, uint32_t cmd, void *data, int len) { (void)desc; (void)data; (void)len; (void)cmd; return 0; }

/* Enqueue only: wake the TX thread, as wlanbt's kick does. */
static int drv_start_tx(void *desc, int arg)
{
	(void)desc; (void)arg;
	c_kicks++;              /* only writer: net_ps, under its giant lock */
	{
		uint32_t z = 0, t = (uint32_t)ksceKernelGetSystemTimeWide() | 1u;
		__atomic_compare_exchange_n(&g_kick_t, &z, t, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	}
	ksceKernelSetEventFlag(g_evf, EV_KICK);
	return 0;
}

/* ---- public -------------------------------------------------------------- */

void
iface_kick_tx(void)
{
	if (g_evf >= 0)
		ksceKernelSetEventFlag(g_evf, EV_KICK);
}

/* strand_arm: the TX thread installs the hook and starts its 250 ms poll. A
 * bit of its own, so it is not mistaken for a kick. */
void
iface_strand_wake(void)
{
	if (g_evf >= 0)
		ksceKernelSetEventFlag(g_evf, EV_STRAND);
}

/* Flags only (any thread, but not under the giant lock: it sets an event
 * flag, as iface_kick_tx does). The TX thread sends the link-up and does one
 * dequeue pass as soon as it may call net_ps (iface_rearm.h). */
void
iface_rearm(const char *why)
{
	g_rearm_why = why;
	__atomic_add_fetch(&g_gate.req, 1, __ATOMIC_RELEASE);
	iface_kick_tx();
	tn_wake();              /* the lent steps and the timer out of an idle wait */
}

void
iface_set_paused(int paused)
{
	(void)paused;   /* g_st.paused is read directly by the TX thread */
}

int
iface_start_threads(void)
{
	g_arp_lock = ksceKernelCreateMutex("vpnturbo_arp", 0, 0, NULL);
	g_evf = ksceKernelCreateEventFlag("vpnturbo_tx", 0, 0, NULL);
	if (g_evf < 0)
		return g_evf;
	g_tx_thread = ksceKernelCreateThread("vpnturbo_tx", tx_thread, 0x3C, 0x2000, 0, 0, NULL);
	if (g_tx_thread < 0)
		return g_tx_thread;
	return ksceKernelStartThread(g_tx_thread, 0, NULL);
}

/*
 * Attach once. Called with NO lock of ours held (control.c
 * attach_now, after the control mutex is released - the
 * attach is a net_ps call). The attach flag below makes the check-then-set
 * atomic, so two callers can never both pass "not attached" and the second
 * memset g_desc while net_ps already holds it. A second caller during an
 * attach gets -VT_ERR_BUSY and the work stays deferred for the timer.
 */
static volatile int g_attaching;

int
iface_attach(void)
{
	uintptr_t attach = 0;
	int r, unit;

	if (g_st.attached)
		return 0;
	if (__atomic_exchange_n(&g_attaching, 1, __ATOMIC_ACQ_REL))
		return -(int)VT_ERR_BUSY;
	if (g_st.attached) {
		__atomic_store_n(&g_attaching, 0, __ATOMIC_RELEASE);
		return 0;
	}
	r = module_get_export_func(KERNEL_PID, "SceNetPs", NETPS_DRIVER_LIB, NID_IF_ATTACH, &attach);
	if (r < 0) {
		klog("iface: resolve attach failed 0x%08X", (unsigned)r);
		__atomic_store_n(&g_attaching, 0, __ATOMIC_RELEASE);
		return r;
	}

	memset(g_desc, 0, sizeof(g_desc));
	g_desc[D_INIT]     = (uint32_t)(uintptr_t)drv_init;
	g_desc[D_STOP]     = (uint32_t)(uintptr_t)drv_stop;
	g_desc[D_START_TX] = (uint32_t)(uintptr_t)drv_start_tx;
	g_desc[D_MCAST]    = (uint32_t)(uintptr_t)drv_mcast;
	g_desc[D_IOCTL]    = (uint32_t)(uintptr_t)drv_ioctl;
	g_desc[D_CAPS]     = 0;            /* no checksum offload */
	g_desc[D_TYPE]     = TYPE_OTHER;
	memcpy(&g_desc[D_MAC], k_our_mac, 6);
	g_desc[D_MAX_MTU]  = 1500;
	g_desc[D_HEADROOM] = 0;
	g_desc[D_SOFTC]    = (uint32_t)(uintptr_t)&g_st;

	r = ((attach_fn)attach)(g_desc);
	klog("iface: attach -> 0x%08X", (unsigned)r);
	if (r < 0) {
		__atomic_store_n(&g_attaching, 0, __ATOMIC_RELEASE);
		return r;
	}
	g_st.attached = 1;
	g_run = 1;
	__atomic_store_n(&g_attaching, 0, __ATOMIC_RELEASE);

	/* Unit number: net_ps assigns the first free other* unit; we cannot read
	 * it back cheaply, so report other0 and let the client probe other0..7
	 * when it sets the address (as the app does). */
	for (unit = 0; unit < 8; unit++) { }
	strncpy(g_st.ifname, "other0", sizeof(g_st.ifname) - 1);

	/* Attach forces MTU 1500; set the tunnel MTU now (desc[0x19] is only the
	 * SIOCSIFMTU ceiling). */
	SVC(set_mtu_fn, D_SET_MTU)(g_desc, g_st.mtu ? g_st.mtu : DEFAULT_MTU);
	/* Read only, nothing held: find and prove other0's ifnet (flags 0x8802
	 * expected here, before the plugin brings it up). */
	ifnet_probe("attach");
	ksceKernelSetEventFlag(g_evf, EV_KICK);
	return 0;
}
