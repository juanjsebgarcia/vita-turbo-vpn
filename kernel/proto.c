/*
 * proto.c - the protocol layer (see tn_proto.h). OpenBSD's wg_noise /
 * wg_cookie and the crypto do the work; this drives them: one peer, the
 * index table, the OpenBSD-style timers and the state machine.
 * It touches nothing Vita-specific and compiles for the host and
 * the kernel alike.
 */
#ifdef TN_CRYPTO_BENCH
#include <tn_crypto.h>
#endif
#include "tn_proto.h"

#include <tn_core.h>		/* wg_noise.h, wg_cookie.h, wire formats, timer constants */

#include <string.h>

#define SEC_NS		1000000000ULL
#define IDX_SLOTS	8

/* ---- the single peer ----------------------------------------------------- */

static struct {
	int			 configured;
	struct noise_local	 l;
	struct noise_remote	 r;
	struct cookie_maker	 cm;	/* keyed with the peer's public key */
	struct cookie_checker	 cc;	/* keyed with our own public key */
	struct pool		 pool;	/* rate-limiter entries (one checker) */

	uint32_t		 idx[IDX_SLOTS];	/* live local indices, 0 = free */

	uint32_t		 tunnel_address;	/* host order */
	struct tnp_prefix	 allowed[32];
	unsigned		 allowed_count;
	uint16_t		 keepalive_s;

	/* sinks */
	tnp_send_fn		 send;
	tnp_deliver_fn		 deliver;
	tnp_roam_fn		 roam;
	void			*ctx;

	/* current source address of the peer, host order (roaming) */
	uint32_t		 ep_ip;
	uint16_t		 ep_port;

	/* state machine / timers */
	int			 state;
	int			 want;			/* the tunnel is wanted */
	int			 initiator_session;	/* current session we began */
	unsigned		 hs_attempts;		/* initiations since last success */
	uint64_t		 hs_last_sent_ns;
	uint64_t		 last_handshake_ns;	/* last completed handshake, 0 = never */
	uint64_t		 last_recv_ns;		/* last authenticated inbound */
	uint64_t		 last_sent_data_ns;	/* last data we encrypted */
	uint64_t		 first_unanswered_ns;	/* first data sent since anything was received */
	int			 owe_keepalive;		/* received data, owe a passive keepalive */
	uint64_t		 persistent_last_ns;

	/* handshake round trip: the initiation being timed (its sender index,
	 * as on the wire) and when it left; a resend replaces both */
	uint32_t		 hs_meas_idx;
	uint64_t		 hs_meas_ns;		/* 0 = nothing being timed */
	uint32_t		 hs_rtt_ms;		/* last measured, 0xFFFFFFFF = none */
	uint32_t		 handshakes;		/* completed since tnp_init; never reset */
	unsigned		 trace;			/* log the next N timer actions (wake) */

	/* stats */
	uint64_t		 rx_bytes, tx_bytes;
	uint32_t		 rx_packets, tx_packets;
} g;

/*
 * One scratch buffer per direction; a datagram is at most MTU + headers.
 *
 * g_rx_scratch is used only by tnp_input, which only the lent RX thread
 * calls. g_tx_scratch is used by send_data, which three threads reach: the
 * interface TX thread (data, latency pings), the timer thread (keepalives)
 * and the lent RX thread (the keepalive that confirms a handshake). Every
 * caller holds the protocol lock (P_LOCK, below), and send_data also runs
 * under the core's module-wide "mutex" lock
 * (TNSHIM_LOCK_MTX). Why that lock and why it cannot deadlock:
 *  - noise_remote_encrypt already takes it (recursive), so the encryption
 *    was serialised on it anyway; the memcpy and the send sink are all the
 *    lock adds.
 *  - The core's order is rwlock before mutex; nothing inside send_data takes
 *    the rwlock, so the order holds.
 *  - The send sink (transport_tx_enqueue) only copies into the transport
 *    ring under the ring mutex, a leaf, and sets an event flag. No net_ps
 *    call happens under it, so net_ps's giant lock is never taken inside it,
 *    and no net_ps callback (desc[0x10]/[0x11]/[0x12], run under the giant
 *    lock) ever takes this lock.
 */
#define SCRATCH_MAX	2048
static uint8_t g_tx_scratch[SCRATCH_MAX];
static uint8_t g_rx_scratch[SCRATCH_MAX];

/*
 * THE PROTOCOL LOCK. Every public entry point that reads
 * or changes the peer (tnp_set_config, tnp_clear, tnp_stop, tnp_input,
 * tnp_timer_tick, tnp_start_handshake, tnp_rehandshake, tnp_encrypt_send,
 * tnp_allowed, tnp_same_private) runs under the core's module-wide "rwlock"
 * lock (TNSHIM_LOCK_RW, a recursive kernel mutex on the Vita: the shim maps
 * every struct rwlock to it). Before, tnp_set_config / tnp_clear / tnp_stop
 * (syscalls) could free the cookie checker's pool or zero the session while
 * the lent RX thread was inside tnp_input, the interface TX thread inside
 * send_data or the timer thread inside tnp_timer_tick.
 *
 * Why that lock: the core already takes it (noise's identity and handshake
 * rwlocks) and its order is rwlock before mutex, so taking it first at
 * every entry keeps that order; the data-path crypto was already serialised
 * on the mutex lock, so little parallelism is lost. Nothing done under it
 * calls net_ps or blocks: the send sink only copies into the transport ring
 * (a leaf mutex) and the roam sink sets fields. The deliver sink (desc[4] /
 * desc[7] / desc[9], net_ps entry points) is called AFTER the lock is left
 * (tnp_input). It is never taken from a net_ps callback.
 *
 * tnp_get_stats takes no lock (vtGetStatus, the DNS hook and the display
 * hook must never wait): the 64-bit counters and timestamps it reads are
 * written and read with single-copy-atomic 64-bit accesses (ld64/st64:
 * LDREXD/STREXD on ARMv7), the rest are aligned 32-bit words.
 */
/*
 * The gate in front of it. The core's lock has no
 * try and no timeout (core/shim), so the protocol lock is now a recursive
 * mutex of this file taken FIRST at every entry, then the core's rwlock lock
 * as before (uncontended from then on: every core call is made under the
 * gate, so the core's own lock order is unchanged). The gate gives:
 *   - a try (tnp_input_try, tnp_rehandshake_try, tnp_timer_tick_try): the
 *     lent RX step and the timer never wait for it; a busy gate means "next
 *     step" (the RX step keeps the datagram and feeds it again 2-20 ms later);
 *   - a watched wait for everyone else: after each second of waiting one
 *     line, "watchdog: ... waited N ms for the protocol lock (held by <entry>,
 *     thread T, for M ms)". The holder is the tnp_* entry that took it;
 *   - tnp_lock_holder() for the platform's own lines.
 * Nothing under it blocks (see above), so a wait is normally a few ms (one
 * curve25519 initiation is the longest hold, ~5-10 ms on the Vita).
 */
#define PL_LOG_US	1000000U	/* one watchdog line per second waited */

static volatile uintptr_t pl_owner;	/* thread holding the gate, 0 = none */
static unsigned pl_depth;		/* written by the holder only */
static const char *volatile pl_who;	/* the entry that took it */
static volatile uint64_t pl_since_ns;	/* when (now_ns) */
static int pl_ready;
static uint64_t now_ns(void);

#ifdef TN_SHIM_VITA
/* SceThreadmgrForDriver; declared here so this file keeps away from the
 * psp2kern headers (the shim remaps BSD names they also use). */
int ksceKernelCreateMutex(const char *name, unsigned int attr, int init_count, void *opt);
int ksceKernelDeleteMutex(int mutexid);
int ksceKernelLockMutex(int mutexid, int lock_count, unsigned int *timeout);
int ksceKernelTryLockMutex(int mutexid, int lock_count);
int ksceKernelUnlockMutex(int mutexid, int unlock_count);
#define PL_ATTR_RECURSIVE	0x02
#define PL_WAIT_TIMEOUT		((int)0x80028005)
static int pl_mtx = -1;

static int
pl_init(void)
{
	if (pl_mtx < 0)
		pl_mtx = ksceKernelCreateMutex("vpnturbo_proto", PL_ATTR_RECURSIVE, 0, NULL);
	return pl_mtx < 0 ? pl_mtx : 0;
}
static void pl_fini(void) { if (pl_mtx >= 0) ksceKernelDeleteMutex(pl_mtx); pl_mtx = -1; }
static int  pl_raw_try(void) { return ksceKernelTryLockMutex(pl_mtx, 1) >= 0; }
/* 1 = taken, 0 = a second went by, -1 = the wait itself failed */
static int
pl_raw_wait(void)
{
	unsigned int to = PL_LOG_US;
	int r = ksceKernelLockMutex(pl_mtx, 1, &to);
	return r >= 0 ? 1 : r == PL_WAIT_TIMEOUT ? 0 : -1;
}
static void pl_raw_unlock(void) { ksceKernelUnlockMutex(pl_mtx, 1); }
#else
#include <pthread.h>
static pthread_mutex_t pl_mtx;

static int
pl_init(void)
{
	pthread_mutexattr_t a;

	if (pl_ready)
		return 0;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&pl_mtx, &a) != 0)
		return -1;
	pthread_mutexattr_destroy(&a);
	return 0;
}
static void pl_fini(void) { }
static int  pl_raw_try(void) { return pthread_mutex_trylock(&pl_mtx) == 0; }
static int
pl_raw_wait(void)
{
	struct timespec ts;
	int r;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += PL_LOG_US / 1000000U;
	r = pthread_mutex_timedlock(&pl_mtx, &ts);
	return r == 0 ? 1 : r == ETIMEDOUT ? 0 : -1;
}
static void pl_raw_unlock(void) { pthread_mutex_unlock(&pl_mtx); }
#endif

static void
pl_took(const char *who)
{
	if (pl_depth++ == 0) {
		pl_who = who;
		pl_since_ns = now_ns();
		__atomic_store_n(&pl_owner, tnshim_be_self(), __ATOMIC_RELEASE);
	}
	tnshim_lock_enter(TNSHIM_LOCK_RW);
}

/* 1 = held now; 0 = busy (another thread holds it), nothing taken. */
static int
p_trylock(const char *who)
{
	if (!pl_ready)
		return 0;
	if (!pl_raw_try())
		return 0;
	pl_took(who);
	return 1;
}

static void
p_lock(const char *who)
{
	uint64_t t0;
	unsigned secs = 0;
	int r;

	if (!pl_ready)
		return;                 /* not initialised: tnp_init failed, no core */
	if (pl_raw_try()) {
		pl_took(who);
		return;
	}
	t0 = now_ns();
	while ((r = pl_raw_wait()) == 0) {
		const char *h = pl_who;
		uint64_t since = pl_since_ns, now = now_ns();
		secs++;
		tnp_log("watchdog: %s (thread 0x%lX) waited %u ms for the protocol lock (held by %s, thread 0x%lX, for %u ms)",
		    who, (unsigned long)tnshim_be_self(), (unsigned)((now - t0) / 1000000ULL),
		    h ? h : "?", (unsigned long)__atomic_load_n(&pl_owner, __ATOMIC_ACQUIRE),
		    since && now > since ? (unsigned)((now - since) / 1000000ULL) : 0u);
	}
	if (r < 0) {
		/* The wait itself failed (not a timeout): poll, never give up
		 * the call (the caller needs the lock to touch the peer). */
		static unsigned told;
		if (told++ < 3)
			tnp_log("watchdog: protocol lock wait failed in %s: polling", who);
		while (!pl_raw_try())
			tnshim_sleep_nsec(1000000ULL);
	}
	if (secs)
		tnp_log("watchdog: %s got the protocol lock after %u ms", who,
		    (unsigned)((now_ns() - t0) / 1000000ULL));
	pl_took(who);
}

static void
p_unlock(void)
{
	if (!pl_ready)
		return;
	tnshim_lock_exit(TNSHIM_LOCK_RW);
	if (--pl_depth == 0) {
		__atomic_store_n(&pl_owner, 0, __ATOMIC_RELEASE);
		pl_who = NULL;
		pl_since_ns = 0;
	}
	pl_raw_unlock();
}

const char *
tnp_lock_holder(uint32_t *held_ms)
{
	const char *h = pl_who;
	uint64_t since = pl_since_ns, now = now_ns();

	if (held_ms)
		*held_ms = h && since && now > since ? (uint32_t)((now - since) / 1000000ULL) : 0;
	return __atomic_load_n(&pl_owner, __ATOMIC_ACQUIRE) ? (h ? h : "?") : NULL;
}

#define P_LOCK()	p_lock(__func__)
#define P_TRYLOCK()	p_trylock(__func__)
#define P_UNLOCK()	p_unlock()

static inline uint64_t ld64(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void st64(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

/* ---- helpers ------------------------------------------------------------- */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	getnanouptime(&ts);
	return (uint64_t)ts.tv_sec * SEC_NS + (uint64_t)ts.tv_nsec;
}

static void
set_state(int s)
{
	static const char *const n[] = {
		"idle", "handshaking", "up", "stalled", "error"
	};

	if (g.state == s)
		return;
	g.state = s;
	tnp_log("state -> %s", (unsigned)s < 5 ? n[s] : "?");
}

static void
make_sin(struct sockaddr_in *sin, uint32_t ip, uint16_t port)
{
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = htobe16(port);
	sin->sin_addr.s_addr = htobe32(ip);
}

static uint32_t
idx_set(void *arg, struct noise_remote *r)
{
	uint32_t v;
	int i, dup;

	(void)arg; (void)r;
	do {
		v = arc4random();
		dup = (v == 0);
		for (i = 0; i < IDX_SLOTS; i++)
			dup |= (g.idx[i] == v);
	} while (dup);
	for (i = 0; i < IDX_SLOTS; i++)
		if (g.idx[i] == 0) {
			g.idx[i] = v;
			return v;
		}
	/* Table full should not happen with one peer and three keypairs. */
	return v;
}

static void
idx_drop(void *arg, uint32_t v)
{
	int i;

	(void)arg;
	for (i = 0; i < IDX_SLOTS; i++)
		if (g.idx[i] == v)
			g.idx[i] = 0;
}

static int
idx_known(uint32_t v)
{
	int i;

	for (i = 0; i < IDX_SLOTS; i++)
		if (v != 0 && g.idx[i] == v)
			return 1;
	return 0;
}

static struct noise_remote *
remote_get(void *arg, uint8_t pub[NOISE_PUBLIC_KEY_LEN])
{
	(void)arg;
	return memcmp(g.r.r_public, pub, NOISE_PUBLIC_KEY_LEN) == 0 ? &g.r : NULL;
}

/* ---- allowed-ips --------------------------------------------------------- */

static int
allowed_locked(uint32_t ip)
{
	unsigned i;

	for (i = 0; i < g.allowed_count; i++) {
		uint8_t p = g.allowed[i].prefix;
		uint32_t mask = p == 0 ? 0 : (p >= 32 ? 0xffffffffu : ~((1u << (32 - p)) - 1));
		if ((ip & mask) == (g.allowed[i].net & mask))
			return 1;
	}
	return 0;
}

int
tnp_allowed(uint32_t ip)
{
	int r;

	P_LOCK();
	r = allowed_locked(ip);
	P_UNLOCK();
	return r;
}

/* ---- sending handshake messages ----------------------------------------- */

/*
 * ROAMING. The peer's address is
 * taken ONLY from a handshake response that completed our own latest
 * initiation (on_response): a response cannot be replayed into another
 * handshake (it must match the ephemeral key of an initiation we just sent,
 * and noise accepts one response per initiation). Data, keepalives, cookie
 * replies and initiations from another address are still processed (data
 * is decrypted and delivered) but never change where we send: a captured
 * data packet replayed from elsewhere is rejected by the replay counter
 * anyway, but even a first copy of one raced from another address no longer
 * moves the tunnel or its cleartext bypass route. Before, every authentic
 * data packet roamed, and the roam rewrote the endpoint the bypass /32 is
 * built from (transport.c, routes.c).
 *
 * The sink is told the source of every completed response (one per
 * handshake, every two minutes or so); the platform decides whether that
 * is a change (transport_roam). The home hairpin case (the server answering
 * from the public address the profile names) is the same address: no roam.
 */
static void
roam_to(uint32_t ip, uint16_t port)
{
	if (ip == 0)
		return;
	g.ep_ip = ip;
	g.ep_port = port;
	if (g.roam)
		g.roam(g.ctx, ip, port);
}

static int
send_initiation(void)
{
	struct tn_pkt_initiation pkt;
	int ret;

	memset(&pkt, 0, sizeof(pkt));
	ret = noise_create_initiation(&g.r, &pkt.s_idx, pkt.ue, pkt.es, pkt.ets);
	if (ret != 0) {
		tnp_log("initiation: core error %d", ret);
		return -1;
	}
	pkt.t = TN_PKT_INITIATION;
	cookie_maker_mac(&g.cm, &pkt.m, &pkt, sizeof(pkt) - sizeof(pkt.m));
	g.hs_last_sent_ns = now_ns();
	g.hs_meas_idx = pkt.s_idx;
	g.hs_meas_ns = g.hs_last_sent_ns;
	g.send(g.ctx, &pkt, sizeof(pkt));
	st64(&g.tx_bytes, g.tx_bytes + sizeof(pkt));
	return 0;
}

/*
 * The initiation rate limit (the protocol's): at most one initiation per
 * REKEY_TIMEOUT, in every state. Each path that wants a handshake because
 * of traffic (a packet with no usable keypair, ESTALE, the responder-side
 * rekey) and the explicit starts (connect, resume) go through
 * initiate_limited; the retransmit timer keeps its own REKEY_TIMEOUT +
 * jitter spacing on the same hs_last_sent_ns. Earlier the guard
 * applied only while HANDSHAKING, so with the state UP and the keypair
 * expired every outbound packet sent a fresh initiation, each replacing
 * the last one at the peer, and the handshake never completed.
 *
 * g_init_busy is a flag, not a lock (three threads can get here: the
 * interface TX thread, the timer, the lent RX thread); a second caller at
 * the same instant simply skips, the first one sends.
 */
static volatile int g_init_busy;

static int
initiation_recent(uint64_t now)
{
	return g.hs_last_sent_ns != 0 &&
	    now - g.hs_last_sent_ns < (uint64_t)REKEY_TIMEOUT * SEC_NS;
}

/* 1 if an initiation went out now, 0 if one went out too recently. */
static int
initiate_limited(void)
{
	uint64_t now;
	int sent = 0;

	if (__atomic_exchange_n(&g_init_busy, 1, __ATOMIC_ACQUIRE))
		return 0;
	now = now_ns();
	if (!initiation_recent(now)) {
		if (send_initiation() != 0)
			g.hs_last_sent_ns = now;	/* a core error is not retried per packet */
		sent = 1;
	}
	__atomic_store_n(&g_init_busy, 0, __ATOMIC_RELEASE);
	return sent;
}

/*
 * Encrypt found no usable keypair (none yet, or the current one past
 * REJECT_AFTER_TIME / its message limits). While UP that means the
 * session is gone: go to HANDSHAKING, so the retransmit timer runs and,
 * unanswered, STALLED follows as for any handshake. Already handshaking or
 * stalled: nothing, the timer resends (a packet must not restart the
 * attempt count, or STALLED would never come while traffic flows). The
 * packet itself is dropped by the caller, as before (the protocol tolerates
 * loss). Called without the core's mutex held (lock order: see above).
 */
static void
session_unusable(void)
{
	if (!g.configured || !g.want)
		return;
	if (g.state == TNP_HANDSHAKING || g.state == TNP_STALLED)
		return;
	g.hs_attempts = 0;
	set_state(TNP_HANDSHAKING);
	tnp_log("no usable session: %s", initiate_limited() ?
	    "handshake started" : "handshake pending (initiation sent < 5 s ago)");
}

static void
send_response(void)
{
	struct tn_pkt_response pkt;
	int ret;

	memset(&pkt, 0, sizeof(pkt));
	ret = noise_create_response(&g.r, &pkt.s_idx, &pkt.r_idx, pkt.ue, pkt.en);
	if (ret != 0) {
		tnp_log("response: core error %d", ret);
		return;
	}
	if (noise_remote_begin_session(&g.r) != 0) {
		tnp_log("response: begin_session failed");
		return;
	}
	pkt.t = TN_PKT_RESPONSE;
	cookie_maker_mac(&g.cm, &pkt.m, &pkt, sizeof(pkt) - sizeof(pkt.m));
	g.send(g.ctx, &pkt, sizeof(pkt));
	st64(&g.tx_bytes, g.tx_bytes + sizeof(pkt));
	g.initiator_session = 0;
	st64(&g.last_handshake_ns, now_ns());
	set_state(TNP_UP);
}

/* Encrypt a (possibly empty) inner payload and send it as a data message.
 * ip==NULL,len==0 is a keepalive. Returns 0 on success, <0 if no session. */
static int
send_data(const void *ip, uint32_t len)
{
	struct tn_pkt_data *hdr = (struct tn_pkt_data *)g_tx_scratch;
	uint8_t *body = g_tx_scratch + sizeof(*hdr);
	uint32_t padded = (uint32_t)TN_PKT_WITH_PADDING(len);
	uint64_t nonce;
	uint32_t r_idx;
	int ret;

	if (sizeof(*hdr) + padded + NOISE_AUTHTAG_LEN > SCRATCH_MAX)
		return -1;

	tnshim_lock_enter(TNSHIM_LOCK_MTX);	/* g_tx_scratch: see above */
	if (len)
		memcpy(body, ip, len);
	if (padded > len)
		memset(body + len, 0, padded - len);

	ret = noise_remote_encrypt(&g.r, &r_idx, &nonce, body, padded);
	if (ret == EINVAL) {
		tnshim_lock_exit(TNSHIM_LOCK_MTX);
		session_unusable();		/* at most one initiation (above) */
		return -1;			/* no usable keypair */
	}
	/* ESTALE means "rekey soon"; the packet is still encrypted and valid
	 * and is sent below; the rekey is asked for after the lock is left. */

	hdr->t = TN_PKT_DATA;
	hdr->r_idx = r_idx;
	htolem64(hdr->nonce, nonce);
	g.send(g.ctx, g_tx_scratch, sizeof(*hdr) + padded + NOISE_AUTHTAG_LEN);

	st64(&g.tx_bytes, g.tx_bytes + len);
	if (len) {
		g.tx_packets++;
		g.last_sent_data_ns = now_ns();
		if (!g.first_unanswered_ns || g.last_recv_ns >= g.first_unanswered_ns)
			g.first_unanswered_ns = g.last_sent_data_ns;
	}
	g.owe_keepalive = 0;
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
	if (ret == ESTALE && g.want)
		initiate_limited();		/* the protocol's "keep key fresh" on send */
	return 0;
}

static void
send_keepalive(void)
{
	send_data(NULL, 0);
}

/* ---- public control ------------------------------------------------------ */

void
tnp_set_sinks(tnp_send_fn send, tnp_deliver_fn deliver, tnp_roam_fn roam, void *ctx)
{
	g.send = send;
	g.deliver = deliver;
	g.roam = roam;
	g.ctx = ctx;
}

int
tnp_init(void)
{
	int r = tnshim_init();

	if (r != 0)
		return r;
	if ((r = pl_init()) != 0) {
		tnshim_fini();
		return r;
	}
	pl_ready = 1;
	memset(&g, 0, sizeof(g));
	g.state = TNP_IDLE;
	g.last_handshake_ns = 0;
	g.hs_rtt_ms = 0xFFFFFFFFu;
#ifdef TN_CRYPTO_BENCH
	{
		/* Device check of the data-path crypto (core/README.md): every path
		 * against the portable C, then MB/s for 1420-byte packets. */
		static struct tn_crypto_bench_row rows[8];
		int i, n, f = tn_crypto_selftest();
		tnp_log("crypto %s selftest %s (%d)", tn_crypto_impl(), f ? "FAILED" : "ok", f);
		n = f ? 0 : tn_crypto_bench(rows, 8, 2000, 1420);
		for (i = 0; i < n; i++)
			tnp_log("bench %s ops=%u len=%u seal_us=%u open_us=%u seal_kBps=%u open_kBps=%u ok=%d",
			    rows[i].name, rows[i].ops, rows[i].bytes,
			    (unsigned)(rows[i].seal_ns / 1000), (unsigned)(rows[i].open_ns / 1000),
			    rows[i].kbps_seal, rows[i].kbps_open, rows[i].ok);
	}
#endif
	return 0;
}

void
tnp_fini(void)
{
	tnp_clear();
	pl_ready = 0;
	pl_fini();
	tnshim_fini();
}

/*
 * Everything that can be refused is checked BEFORE the running peer is
 * touched: a private key whose public key is zero, a
 * zero peer key, or a peer key whose shared secret with ours is zero (a
 * low-order point). Then -1 and the loaded peer, its session included, is
 * left exactly as it was. -2 only if a step after the clear failed (the
 * rate limiter's pool: not expected, the clear frees its slot); the core is
 * then unconfigured and the caller must say so.
 */
static int
all_zero(const uint8_t *p, unsigned n)
{
	uint8_t acc = 0;

	while (n--)
		acc |= *p++;
	return acc == 0;
}

static int
config_valid(const struct tnp_config *cfg)
{
	uint8_t priv[32], pub[32], ss[32];
	int ok;

	if (all_zero(cfg->private_key, 32) || all_zero(cfg->peer_public, 32))
		return 0;
	memcpy(priv, cfg->private_key, 32);
	curve25519_clamp_secret(priv);
	ok = curve25519_generate_public(pub, priv) &&
	    curve25519(ss, priv, cfg->peer_public) && !all_zero(ss, 32);
	tnshim_explicit_bzero(priv, sizeof(priv));
	tnshim_explicit_bzero(ss, sizeof(ss));
	return ok;
}

static void clear_locked(void);
static int set_config_locked(const struct tnp_config *cfg);

int
tnp_set_config(const struct tnp_config *cfg)
{
	int r;

	if (!config_valid(cfg)) {
		tnp_log("config: refused (bad key); the loaded peer is kept");
		return -1;
	}
	P_LOCK();
	r = set_config_locked(cfg);
	P_UNLOCK();
	return r;
}

static int
set_config_locked(const struct tnp_config *cfg)
{
	struct noise_upcall up;
	uint8_t priv[32], psk[32];
	int has_psk = 0;
	unsigned i;

	clear_locked();

	up.u_arg = NULL;
	up.u_remote_get = remote_get;
	up.u_index_set = idx_set;
	up.u_index_drop = idx_drop;
	noise_local_init(&g.l, &up);

	memcpy(priv, cfg->private_key, 32);
	noise_local_lock_identity(&g.l);
	if (noise_local_set_private(&g.l, priv) != 0) {
		noise_local_unlock_identity(&g.l);
		tnshim_explicit_bzero(priv, sizeof(priv));
		noise_remote_clear(&g.r);
		tnp_log("config: bad private key");
		return -2;
	}
	noise_local_unlock_identity(&g.l);
	tnshim_explicit_bzero(priv, sizeof(priv));

	noise_remote_init(&g.r, (uint8_t *)cfg->peer_public, &g.l);
	noise_remote_precompute(&g.r);

	for (i = 0; i < 32; i++)
		has_psk |= cfg->preshared[i];
	if (has_psk) {
		memcpy(psk, cfg->preshared, 32);
		noise_remote_set_psk(&g.r, psk);
		tnshim_explicit_bzero(psk, sizeof(psk));
	}

	cookie_maker_init(&g.cm, (uint8_t *)cfg->peer_public);
	{
		uint8_t ourpub[32];
		pool_init(&g.pool, sizeof(struct ratelimit_entry), 0, IPL_NET, 0,
		    "tnp_ratelimit", NULL);
		if (cookie_checker_init(&g.cc, &g.pool) != 0) {
			pool_destroy(&g.pool);
			tnp_log("config: cookie checker init failed");
			return -2;
		}
		noise_local_keys(&g.l, ourpub, NULL);
		cookie_checker_update(&g.cc, ourpub);
		tnshim_explicit_bzero(ourpub, sizeof(ourpub));
	}

	g.tunnel_address = cfg->tunnel_address;
	g.allowed_count = cfg->allowed_count > 32 ? 32 : cfg->allowed_count;
	for (i = 0; i < g.allowed_count; i++)
		g.allowed[i] = cfg->allowed[i];
	g.keepalive_s = cfg->keepalive_s;

	g.configured = 1;
	set_state(TNP_IDLE);
	return 0;
}

void
tnp_clear(void)
{
	P_LOCK();
	clear_locked();
	P_UNLOCK();
}

static void
clear_locked(void)
{
	if (g.configured) {
		noise_remote_clear(&g.r);
		cookie_checker_deinit(&g.cc);
		pool_destroy(&g.pool);
	}
	g.configured = 0;
	g.want = 0;
	g.initiator_session = 0;
	g.hs_attempts = 0;
	g.hs_last_sent_ns = 0;
	st64(&g.last_handshake_ns, 0);
	g.last_recv_ns = 0;
	g.last_sent_data_ns = 0;
	g.first_unanswered_ns = 0;
	g.owe_keepalive = 0;
	g.persistent_last_ns = 0;
	g.hs_meas_idx = 0;
	g.hs_meas_ns = 0;
	g.hs_rtt_ms = 0xFFFFFFFFu;
	g.ep_ip = 0;
	g.ep_port = 0;
	memset(g.idx, 0, sizeof(g.idx));
	set_state(TNP_IDLE);
}

void
tnp_start_handshake(void)
{
	P_LOCK();
	if (!g.configured) {
		set_state(TNP_ERROR);
		P_UNLOCK();
		return;
	}
	g.want = 1;
	/* An explicit start (connect, resume, a pause ending, a new profile):
	 * the attempt count starts again and the state shows the handshake,
	 * but the initiation itself obeys the rate limit (initiate_limited):
	 * one sent in the last REKEY_TIMEOUT is left to the retransmit timer.
	 * A packet dropped for want of a session no longer comes here (it
	 * goes through session_unusable, from send_data). */
	g.hs_attempts = 0;
	if (g.state != TNP_UP)
		set_state(TNP_HANDSHAKING);
	initiate_limited();
	P_UNLOCK();
}

void
tnp_stop(void)
{
	P_LOCK();
	g.want = 0;
	if (g.configured)
		noise_remote_clear(&g.r);	/* session keys and handshake; the
						 * peer's static keys stay */
	g.initiator_session = 0;
	g.hs_attempts = 0;
	g.hs_last_sent_ns = 0;
	st64(&g.last_handshake_ns, 0);
	g.last_recv_ns = 0;
	g.last_sent_data_ns = 0;
	g.first_unanswered_ns = 0;
	g.owe_keepalive = 0;
	g.persistent_last_ns = 0;
	g.hs_meas_ns = 0;
	g.ep_ip = 0;
	g.ep_port = 0;
	set_state(TNP_IDLE);
	P_UNLOCK();
}

static void rehandshake_locked(void);

void
tnp_rehandshake(void)
{
	P_LOCK();
	rehandshake_locked();
	P_UNLOCK();
}

int
tnp_rehandshake_try(void)
{
	if (!P_TRYLOCK())
		return -1;
	rehandshake_locked();
	P_UNLOCK();
	return 0;
}

static void
rehandshake_locked(void)
{
	if (!g.configured) {
		set_state(TNP_ERROR);
	} else {
		g.want = 1;
		if (!(g.state == TNP_HANDSHAKING && g.hs_last_sent_ns &&
		    now_ns() - g.hs_last_sent_ns < SEC_NS)) {
			g.hs_attempts = 0;
			set_state(TNP_HANDSHAKING);
			send_initiation();
		}
	}
}

void
tnp_trace_timers(unsigned n)
{
	g.trace = n;
}

int
tnp_same_private(const uint8_t priv[32])
{
	uint8_t k[32];
	int same;

	P_LOCK();
	if (!g.configured) {
		P_UNLOCK();
		return 0;
	}
	memcpy(k, priv, 32);
	curve25519_clamp_secret(k);	/* l_private is stored clamped */
	noise_local_lock_identity(&g.l);
	same = g.l.l_has_identity && timingsafe_bcmp(k, g.l.l_private, 32) == 0;
	noise_local_unlock_identity(&g.l);
	tnshim_explicit_bzero(k, sizeof(k));
	P_UNLOCK();
	return same;
}

int
tnp_encrypt_send(const void *ip, uint32_t len)
{
	int r = -1;

	P_LOCK();
	if (g.configured)
		r = send_data(ip, len);
	P_UNLOCK();
	return r;
}

/* ---- inbound ------------------------------------------------------------- */

static void
on_response(const uint8_t *buf, uint32_t len, uint32_t from_ip, uint16_t from_port)
{
	struct tn_pkt_response pkt;
	struct sockaddr_in sin;

	if (len < sizeof(pkt))
		return;
	memcpy(&pkt, buf, sizeof(pkt));
	make_sin(&sin, from_ip, from_port);
	if (cookie_checker_validate_macs(&g.cc, &pkt.m, &pkt,
	    sizeof(pkt) - sizeof(pkt.m), 0, sintosa(&sin)) != 0)
		return;
	if (!idx_known(pkt.r_idx))
		return;
	if (noise_consume_response(&g.r, pkt.s_idx, pkt.r_idx, pkt.ue, pkt.en) != 0)
		return;
	if (noise_remote_begin_session(&g.r) != 0)
		return;

	g.initiator_session = 1;
	st64(&g.last_handshake_ns, now_ns());
	/* Round trip of this handshake: only for the initiation still being
	 * timed (the core accepts a response to its latest one only, and a
	 * resend restarts the clock, so a retry never inflates the figure). */
	if (g.hs_meas_ns && pkt.r_idx == g.hs_meas_idx &&
	    g.last_handshake_ns >= g.hs_meas_ns) {
		uint64_t us = (g.last_handshake_ns - g.hs_meas_ns) / 1000ULL;
		uint32_t ms = us >= 0xFFFFFFF0ULL * 1000ULL ? 0xFFFFFFF0u :
		    (uint32_t)((us + 500) / 1000);
		g.hs_rtt_ms = ms ? ms : 1;
		tnp_log("handshake round trip %u ms", (unsigned)g.hs_rtt_ms);
	}
	g.hs_meas_ns = 0;
	g.last_recv_ns = g.last_handshake_ns;
	g.hs_attempts = 0;
	g.handshakes++;
	roam_to(from_ip, from_port);	/* the only roam (see ROAMING) */
	set_state(TNP_UP);
	tnp_log("handshake complete (initiator)");
	/* Confirm the session so the peer slides next->current and learns our
	 * source endpoint. */
	send_keepalive();
}

static void
on_initiation(const uint8_t *buf, uint32_t len, uint32_t from_ip, uint16_t from_port)
{
	struct tn_pkt_initiation pkt;
	struct noise_remote *r = NULL;
	struct sockaddr_in sin;

	if (len < sizeof(pkt))
		return;
	memcpy(&pkt, buf, sizeof(pkt));
	make_sin(&sin, from_ip, from_port);
	/* Not under load as a client: busy = 0. */
	if (cookie_checker_validate_macs(&g.cc, &pkt.m, &pkt,
	    sizeof(pkt) - sizeof(pkt.m), 0, sintosa(&sin)) != 0)
		return;
	if (noise_consume_initiation(&g.l, &r, pkt.s_idx, pkt.ue, pkt.es, pkt.ets) != 0)
		return;
	if (r != &g.r)
		return;
	/* No roam from an initiation (ROAMING): the response goes to the
	 * address we send to now. A peer that really moved and initiates
	 * from its new address is followed once we complete a handshake of
	 * our own (rekey, or the retransmit timer after a stall). */
	(void)from_port;
	g.last_recv_ns = now_ns();
	tnp_log("handshake complete (responder)");
	send_response();
}

static void
on_cookie(const uint8_t *buf, uint32_t len)
{
	struct tn_pkt_cookie pkt;

	if (len < sizeof(pkt))
		return;
	memcpy(&pkt, buf, sizeof(pkt));
	if (!idx_known(pkt.r_idx))
		return;
	if (cookie_maker_consume_payload(&g.cm, pkt.nonce, pkt.ec) != 0)
		return;
	/* Resend the initiation; cookie_maker_mac now adds mac2. */
	send_initiation();
}

/* Returns the length of a plaintext to deliver (in g_rx_scratch), 0 for
 * none: the caller delivers it after leaving the protocol lock. */
static uint32_t
on_data(const uint8_t *buf, uint32_t len)
{
	const struct tn_pkt_data *hdr = (const struct tn_pkt_data *)buf;
	uint8_t *body;
	uint32_t bodylen, plainlen;
	uint64_t nonce;
	int ret;

	if (len < sizeof(*hdr) || len > SCRATCH_MAX)
		return 0;
	bodylen = len - sizeof(*hdr);		/* ciphertext + tag */
	if (bodylen < NOISE_AUTHTAG_LEN)
		return 0;
	body = g_rx_scratch;
	memcpy(body, buf + sizeof(*hdr), bodylen);
	nonce = lemtoh64(hdr->nonce);

	ret = noise_remote_decrypt(&g.r, hdr->r_idx, nonce, body, bodylen);
	if (ret == EINVAL)
		return 0;			/* bad index / not authentic */
	/* 0, ECONNRESET (slid the next keypair in), ESTALE (rekey soon): the
	 * plaintext is valid and deliverable. */
	if (ret == ECONNRESET) {
		/* The peer's first data on the session we answered: the
		 * responder handshake is confirmed and in use. */
		st64(&g.last_handshake_ns, now_ns());
		g.handshakes++;
		set_state(TNP_UP);
	}
	plainlen = bodylen - NOISE_AUTHTAG_LEN;

	g.last_recv_ns = now_ns();
	/* No roam from data (ROAMING): decrypted and delivered whatever its
	 * source, but where we send is left alone. */

	if (plainlen == 0) {
		/* keepalive */
		return 0;
	}

	/* Check the inner source against AllowedIPs (an early bug). IPv4 only. */
	if (plainlen >= 20 && (body[0] >> 4) == 4) {
		uint32_t src = (uint32_t)body[12] << 24 | (uint32_t)body[13] << 16 |
		    (uint32_t)body[14] << 8 | body[15];
		uint32_t total = (uint32_t)body[2] << 8 | body[3];
		if (!allowed_locked(src))
			return 0;
		if (total >= 20 && total < plainlen)
			plainlen = total;	/* trim padding */
	}

	st64(&g.rx_bytes, g.rx_bytes + plainlen);
	g.rx_packets++;
	g.owe_keepalive = 1;
	return plainlen;
}

/* One caller at a time (the lent RX thread): g_rx_scratch is delivered
 * after the lock is left. */
static uint32_t input_locked(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port);

void
tnp_input(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port)
{
	uint32_t deliver;

	if (len < 4)
		return;
	P_LOCK();
	deliver = input_locked(buf, len, from_ip, from_port);
	P_UNLOCK();
	/* net_ps is entered (desc[4]/[7]/[9]) with no lock of ours held. */
	if (deliver && g.deliver)
		g.deliver(g.ctx, g_rx_scratch, deliver);
}

/* The lent RX step's entry: never waits for the protocol lock. -1 = busy,
 * the datagram was NOT looked at (feed it again later); 0 = processed. */
int
tnp_input_try(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port)
{
	uint32_t deliver;

	if (len < 4)
		return 0;
	if (!P_TRYLOCK())
		return -1;
	deliver = input_locked(buf, len, from_ip, from_port);
	P_UNLOCK();
	if (deliver && g.deliver)
		g.deliver(g.ctx, g_rx_scratch, deliver);
	return 0;
}

static uint32_t
input_locked(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port)
{
	uint32_t type, deliver = 0;

	memcpy(&type, buf, 4);		/* little-endian on the wire */
	if (!g.configured)
		return 0;
	if (type == TN_PKT_DATA)
		deliver = on_data(buf, len);
	else if (type == TN_PKT_RESPONSE)
		on_response(buf, len, from_ip, from_port);
	else if (type == TN_PKT_INITIATION)
		on_initiation(buf, len, from_ip, from_port);
	else if (type == TN_PKT_COOKIE)
		on_cookie(buf, len);
	return deliver;
}

/* ---- timers --------------------------------------------------- */

static int timer_tick_locked(void);

int
tnp_timer_tick(void)
{
	int r;

	P_LOCK();
	r = timer_tick_locked();
	P_UNLOCK();
	return r;
}

int
tnp_timer_tick_try(void)
{
	int r;

	if (!P_TRYLOCK())
		return -1;
	r = timer_tick_locked();
	P_UNLOCK();
	return r;
}

static int
timer_tick_locked(void)
{
	uint64_t now, jitter_ns;

	if (!g.configured || !g.want)
		return g.state;

	now = now_ns();
	jitter_ns = (uint64_t)arc4random_uniform(REKEY_TIMEOUT_JITTER) * 1000000ULL;

	/* Retransmit the initiation while handshaking or stalled. Logged for
	 * the first attempts after a wake (trace), and every sixth otherwise. */
	if (g.state == TNP_HANDSHAKING || g.state == TNP_STALLED) {
		if (now - g.hs_last_sent_ns >= (uint64_t)REKEY_TIMEOUT * SEC_NS + jitter_ns) {
			if (g.state == TNP_HANDSHAKING &&
			    g.hs_attempts >= MAX_TIMER_HANDSHAKES) {
				set_state(TNP_STALLED);
				g.hs_attempts = 0;
			} else {
				g.hs_attempts++;
				if (g.trace || g.hs_attempts % 6 == 1) {
					tnp_log("timer: initiation resent (attempt %u)", g.hs_attempts);
					if (g.trace)
						g.trace--;
				}
				send_initiation();
			}
		}
	}

	/* Established initiator session: rekey after REKEY_AFTER_TIME. */
	if (g.state == TNP_UP && g.initiator_session && g.last_handshake_ns &&
	    now - g.last_handshake_ns >= (uint64_t)REKEY_AFTER_TIME * SEC_NS &&
	    now - g.hs_last_sent_ns >= (uint64_t)REKEY_TIMEOUT * SEC_NS) {
		g.hs_attempts = 0;
		tnp_log("timer: rekey (session %u s old)",
		    (unsigned)((now - g.last_handshake_ns) / SEC_NS));
		send_initiation();
	}

	/* Session the peer began (we answered its initiation). Noise flags
	 * ESTALE by age only for keypairs we initiated, so nothing above
	 * renews this one, and a peer that does not send never rekeys it
	 * (the protocol rekeys on the initiator's sends). Left alone it would be
	 * rejected at REJECT_AFTER_TIME and the first packet after that would
	 * start a handshake (session_unusable): a gap of one handshake. So
	 * start one ourselves at REKEY_AFTER_TIME_RECV (165 s), before the
	 * keypair lapses; rate-limited like every initiation. */
	if (g.state == TNP_UP && !g.initiator_session && g.last_handshake_ns &&
	    now - g.last_handshake_ns >= (uint64_t)REKEY_AFTER_TIME_RECV * SEC_NS &&
	    !initiation_recent(now)) {
		g.hs_attempts = 0;
		tnp_log("timer: rekey as responder (session %u s old)",
		    (unsigned)((now - g.last_handshake_ns) / SEC_NS));
		initiate_limited();
	}

	/* Data sent but nothing received: new handshake after NEW_HANDSHAKE_TIMEOUT,
	 * counted from the FIRST unanswered send (from the last one, steady
	 * traffic kept it from ever firing). The peer answers any data within
	 * KEEPALIVE_TIMEOUT (its passive keepalive), so this is the tunnel not
	 * working: leave UP for HANDSHAKING, so the state says so and the
	 * retransmits, then STALLED, apply (it stayed UP, and every UI said
	 * "Connected", until the keypair expired 180 s later). As
	 * session_unusable does. */
	if (g.state == TNP_UP && g.first_unanswered_ns &&
	    g.last_recv_ns < g.first_unanswered_ns &&
	    now - g.first_unanswered_ns >= (uint64_t)NEW_HANDSHAKE_TIMEOUT * SEC_NS &&
	    now - g.hs_last_sent_ns >= (uint64_t)REKEY_TIMEOUT * SEC_NS) {
		g.hs_attempts = 0;
		tnp_log("timer: no reply to data for %u s, new handshake",
		    (unsigned)((now - g.first_unanswered_ns) / SEC_NS));
		set_state(TNP_HANDSHAKING);
		send_initiation();
	}

	/* Passive keepalive: received data, owe a keepalive after KEEPALIVE_TIMEOUT. */
	if (g.state == TNP_UP && g.owe_keepalive &&
	    now - g.last_recv_ns >= (uint64_t)KEEPALIVE_TIMEOUT * SEC_NS) {
		if (g.trace) {
			g.trace--;
			tnp_log("timer: passive keepalive");
		}
		send_keepalive();
	}

	/* Persistent keepalive, if the profile asks. */
	if (g.state == TNP_UP && g.keepalive_s &&
	    now - g.persistent_last_ns >= (uint64_t)g.keepalive_s * SEC_NS) {
		g.persistent_last_ns = now;
		if (g.trace) {
			g.trace--;
			tnp_log("timer: persistent keepalive");
		}
		send_keepalive();
	}

	/* Zero key material after REJECT_AFTER_TIME * 3 with no handshake. */
	if (g.last_handshake_ns &&
	    now - g.last_handshake_ns >= 3ULL * REJECT_AFTER_TIME * SEC_NS) {
		noise_remote_clear(&g.r);
		st64(&g.last_handshake_ns, 0);
		g.initiator_session = 0;
		set_state(TNP_STALLED);
		tnp_log("session expired; keys zeroed");
	}

	return g.state;
}

void
tnp_get_stats(struct tnp_stats *s)
{
	uint64_t now = now_ns(), hs = ld64(&g.last_handshake_ns);

	/* No lock (see THE PROTOCOL LOCK): atomic 64-bit reads. */
	s->state = g.state;
	if (hs == 0 || hs > now)
		s->handshake_age_s = 0xFFFFFFFFu;
	else
		s->handshake_age_s = (uint32_t)((now - hs) / SEC_NS);
	s->rx_bytes = ld64(&g.rx_bytes);
	s->tx_bytes = ld64(&g.tx_bytes);
	s->rx_packets = g.rx_packets;
	s->tx_packets = g.tx_packets;
	s->handshake_rtt_ms = g.hs_rtt_ms;
	s->handshakes = g.handshakes;
}
