/*
 * transport.c - the UDP transport, run on the two threads SceShell lends the
 * module.
 *
 * ksceNetRecvfrom cannot deliver into kernel memory (0xC0022005), so the
 * socket is created and used inside the two syscalls the SceShell plugin
 * never returns from: vtTransportRx and vtTransportTx. Those run on SceShell's
 * own threads, so ksceNetSocket ties the socket to SceShell's process and
 * recvfrom lands in a buffer in SceShell's memory (the `buf` the plugin
 * passes). Each datagram is copied into kernel memory before it is handed to
 * the protocol core; the core encrypts in the kernel and the outbound
 * datagrams it produces are queued here and sent, on the same SceShell-owned
 * socket, from the TX loop (a socket may be used only by the process that
 * owns it, so the send must happen on a SceShell thread too).
 *
 * Only the SceShell plugin's two lent threads may get here (control.c,
 * lend_enter: title id "main", the first such process pinned, one caller
 * per direction at a time), so the static buffers below (g_rx_kernel, the
 * TX step's out[], and the core's g_rx_scratch) have one user each and the
 * sockets, PF_ROUTE ones included, are only ever SceShell's.
 *
 * The RX loop owns the socket; the TX loop waits for it. Both gate on
 * TN_QUIET() (a user pause, a suspend from its first event, or a wake whose
 * network is not reported ready yet) so neither calls into net_ps while it
 * is suspending or resuming: the RX step
 * returns at once (the plugin sleeps between steps) and the TX step only
 * sleeps. The RX step never blocks. No lock of ours is held across any
 * net_ps call in this file (the ring lock is released before each send).
 */
#include "tn_kern.h"
#include "rx_seq.h"

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/netps.h>

#include <string.h>

#define SOL_SOCKET_  0xffff
#define SO_RCVTIMEO_ 0x1006
#define SO_RCVBUF_   0x1002

/* BSD sockaddr_in as net_ps takes it (same as smoke). */
typedef struct {
	uint8_t  len;
	uint8_t  family;
	uint16_t port;    /* network order */
	uint32_t addr;    /* network order */
	uint16_t vport;
	uint8_t  zero[6];
} SockaddrIn;

static void
sin_set(SockaddrIn *s, uint32_t ip, uint16_t port)
{
	memset(s, 0, sizeof(*s));
	s->len = sizeof(*s);
	s->family = 2;
	s->port = (uint16_t)((port >> 8) | (port << 8));
	s->addr = ((ip & 0xff) << 24) | ((ip & 0xff00) << 8) |
	    ((ip >> 8) & 0xff00) | (ip >> 24);
}

/* ---- outbound ring ------------------------------------------------------- */

#define TX_RING     64
#define TX_DGRAM_MAX 2048
/* The plugin's receive buffer is VT_LEND_RX_BUF_SIZE: the datagram sizes
 * below must not need more. */
_Static_assert(TX_DGRAM_MAX <= VT_LEND_RX_BUF_SIZE, "lent receive buffer too small");

struct tx_slot { uint32_t len; uint32_t t_us; uint8_t buf[TX_DGRAM_MAX]; };
static struct tx_slot g_ring[TX_RING];
static volatile uint32_t g_ring_head, g_ring_tail;   /* head - tail = pending */
static SceUID g_ring_lock = -1;
static volatile SceUID g_ring_holder = -1;   /* last taker (lock-wait watchdog) */
static SceUID g_tx_evf = -1;
static SceUID g_rx_evf = -1;            /* the RX step's idle wait */
static volatile uint32_t g_tx_kick_t;   /* first enqueue since the TX thread last woke (us | 1) */

/*
 * The send token. Two threads may send from the ring: the lent TX
 * thread (woken by every enqueue) and the lent RX thread, which helps when
 * the oldest queued datagram has waited RX_HELP_US (the TX thread is not
 * getting the CPU: a device test measured 1.5 ms average, 16 ms max from seal to
 * sendto, most of it the RX step itself on the same core at the same
 * priority). Whoever holds the token pops and sends in ring order, so the
 * two can never reorder datagrams; the other one does not wait for it. Both
 * are SceShell threads of the process that owns the socket. A try-only
 * flag, never waited for; never held by anything but a lent step.
 */
static volatile int g_tx_tok;

static volatile int g_sock = -1;
static volatile int g_run;

/*
 * Data-path counters. Each field has one writer (the lent RX thread, the lent TX
 * thread, or the send sink under the ring lock); transport_perf_log, on the
 * interface TX thread, reads them unlocked every 30 s and prints the change
 * since its last call. All 32-bit (sums in microseconds wrap after 71
 * minutes; the differences are still right), so no read can be torn. The
 * *_max fields are taken and cleared by the reader with an atomic exchange
 * (a maximum racing the clear may be lost: diagnostics only).
 */
struct tperf {
	/* lent RX thread */
	uint32_t rx_steps, rx_steps_data, rx_dg, rx_cap, rx_max;
	uint32_t rx_busy_ok, rx_busy_kept;
	uint32_t gap[4], gap_max;          /* idle time before a step while traffic flows */
	uint32_t recv_us, recv_n;
	uint32_t open_us, open_n, open_max;   /* tnp_input_try + deliver, one datagram */
	uint32_t routes_us, routes_n, routes_max, routes_skip;
	uint32_t rcvbuf;                   /* bytes accepted at the last socket */
	/* lent TX thread */
	uint32_t tx_steps, tx_sent, tx_max;
	uint32_t q_us, q_max;              /* enqueued -> sendto, per datagram */
	uint32_t send_us, send_max;
	/* send sink (ring lock held) */
	uint32_t ring_hw, ring_drop;
	/* idle cost and latency counters */
	uint32_t tx_by_rx;                 /* sent by the RX step's help (RX thread) */
	uint32_t rx_spin_ok, rx_nap_ok;    /* lock busy: fed after the spin / after a nap */
	uint32_t rx_bytes_max;             /* most bytes taken in one step (receive backlog) */
	uint32_t rx_idle_waits;            /* idle waits (tunnel off) */
	/* timing counters */
	uint32_t recv_empty_us, recv_empty_n;   /* recvfrom with nothing waiting */
	uint32_t recv_data_us, recv_data_n;     /* recvfrom that returned a datagram */
	uint32_t proto_us, dlv_us;         /* "open" split: protocol part, hand-off */
	uint32_t txw_late, txw_late_rx;    /* TX wakes over 2 ms; of them, kicked mid RX step */
	uint32_t txw_served_rx;            /* kicks whose queue the RX step emptied first */
	uint32_t cpu_rx[4], cpu_tx[4], cpu_kick[4], cpu_late[4];
	uint32_t late_same_cpu;            /* late TX wakes on the core of the last RX step */
};
static struct tperf g_pf;
static struct rx_seq g_rxs;            /* lent RX thread */

/*
 * Latency histograms (tn_kern.h lh_*), one writer each:
 *   h_txq   seal (enqueue) -> sendto start, per datagram (TX or RX thread,
 *           under the send token);
 *   h_txw   first enqueue -> the TX thread awake (the scheduler's part);
 *   h_rxo   recvfrom returned -> decrypted and handed to the stack, per
 *           datagram (the added latency we can see on the way in);
 *   h_rxg   idle time before a step while traffic flows: how long the
 *           first datagram of a step may have sat in the socket;
 *   h_rxs   a step's own length, steps that took datagrams.
 */
static struct lhist h_txq, h_txw, h_rxo, h_rxg, h_rxs;
/* Timing counters: h_txq by sender, the protocol part of "open" (lock, copy,
 * decrypt, checks), and recvfrom that returned a datagram. */
static struct lhist h_txq_tx, h_txq_rx, h_rxp, h_rcv;

/* The core this thread is on (MPIDR affinity level 0). */
static inline unsigned
cpu_id(void)
{
	uint32_t m;
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));
	return m & 3u;
}
static volatile int g_rx_in_step;          /* the lent RX step is running */
static volatile unsigned g_rx_cpu = 4;     /* core of the last RX step */
static volatile int g_kick_rx;             /* g_rx_in_step at the first kick */

static inline uint32_t now_us32(void) { return (uint32_t)ksceKernelGetSystemTimeWide(); }
static inline void pf_max(uint32_t *m, uint32_t v) { if (v > *m) *m = v; }

/*
 * Socket lifetime vs the TX step. The RX step owns the
 * socket and closes it (tunnel off, rebuild); the TX step sends on it from
 * another thread. Before, the TX step set a volatile busy flag and the RX
 * step waited at most 50 ms for it: with no barrier between the flag and
 * the socket id (two cores, store-load reordering) and a sendto that can
 * take longer than 50 ms, the id could be closed under a send and, once
 * net_ps reused it, a datagram sent on someone else's socket. Now the TX
 * step takes a reference (g_sock_users, sequentially consistent) BEFORE it
 * reads g_sock, and drops it after the sendto; the RX step clears g_sock
 * BEFORE it reads the count. Either the TX step sees -1 and does not send,
 * or the RX step sees the reference and does not close: a socket still in
 * use is parked (g_sock_closing) and closed by a later RX step once the
 * count is zero; no new socket is made meanwhile (the bind would collide
 * with a fixed ListenPort).
 */
static volatile int g_sock_users;
static volatile int g_sock_closing = -1;
static const char *g_closing_what;

/*
 * Socket rebuild. Whether a socket survives a sleep is not
 * known, and a tunnel that kept its old socket never recovered after a wake:
 * no handshake for 7 minutes, no error logged (the recv error log had a
 * lifetime cap of 5). So the socket is rebuilt on the first RX step after
 * every wake or abandoned suspend (transport_reopen), and after 20 hard
 * errors in a row on either side. Errors are logged per socket (first few,
 * then every 100th). The TX step holds a reference while it uses the socket
 * (above); a socket is never closed under it.
 */
static volatile int g_reopen;
static volatile const char *g_reopen_why;
static volatile unsigned g_rx_hard, g_tx_hard;   /* consecutive hard errors */
static volatile unsigned g_rx_errs, g_tx_errs;   /* this socket's error totals */
static volatile unsigned g_tx_ok;                /* datagrams sent on this socket */
static volatile unsigned g_rx_ok;                /* datagrams received on this socket */
static volatile int g_sock_id = -1;              /* for the log: the socket the counts are for */

volatile int g_socks_open;                       /* ours in SceShell's process (tn_kern.h) */

int transport_socket_open(void) { return g_sock >= 0 || g_sock_closing >= 0; }
unsigned transport_queued(void) { return (unsigned)(g_ring_head - g_ring_tail); }

void
transport_log_counts(const char *why)
{
	klog("transport: %s: socket %d sent %u (errors %u), received %u (errors %u), queued %u",
	    why, g_sock_id, g_tx_ok, g_tx_errs, g_rx_ok, g_rx_errs,
	    (unsigned)(g_ring_head - g_ring_tail));
}

void
transport_flush(void)
{
	uint32_t n;

	if (g_ring_lock < 0)
		return;
	tn_mutex_lock(g_ring_lock, "ring", &g_ring_holder);
	n = g_ring_head - g_ring_tail;
	g_ring_tail = g_ring_head;
	ksceKernelUnlockMutex(g_ring_lock, 1);
	if (n)
		klog("transport: %u stale datagram(s) dropped from the send queue", (unsigned)n);
}

void
transport_reopen(const char *why)
{
	g_reopen_why = why;
	g_reopen = 1;
	transport_wake();
}

#define TX_WAKE 1u

/*
 * tnp send sink: copy one datagram into the ring and wake the TX loop. Runs
 * on whatever thread called into the core (the iface TX thread, the timer
 * thread, or the lent RX thread). It never calls net_ps, so it is lock-order
 * safe: the ring lock is a leaf, never held across a net_ps call.
 */
int
transport_tx_enqueue(void *ctx, const void *buf, uint32_t len)
{
	(void)ctx;
	if (len > TX_DGRAM_MAX || g_ring_lock < 0)
		return -1;
	tn_mutex_lock(g_ring_lock, "ring", &g_ring_holder);
	if (g_ring_head - g_ring_tail >= TX_RING) {
		g_ring_tail++;                 /* ring full: drop the oldest */
		g_pf.ring_drop++;
	}
	{
		struct tx_slot *s = &g_ring[g_ring_head % TX_RING];
		s->len = len;
		s->t_us = now_us32();
		memcpy(s->buf, buf, len);
		__atomic_store_n(&g_ring_head, g_ring_head + 1, __ATOMIC_RELEASE);
		{
			uint32_t z = 0;
			if (__atomic_compare_exchange_n(&g_tx_kick_t, &z, s->t_us | 1u, 0,
			    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
				g_kick_rx = g_rx_in_step;
				g_pf.cpu_kick[cpu_id()]++;
			}
		}
	}
	pf_max(&g_pf.ring_hw, g_ring_head - g_ring_tail);
	ksceKernelUnlockMutex(g_ring_lock, 1);
	if (g_tx_evf >= 0)
		ksceKernelSetEventFlag(g_tx_evf, TX_WAKE);
	return 0;
}

#define RX_WAKE 1u

void
transport_wake(void)
{
	if (g_rx_evf >= 0)
		ksceKernelSetEventFlag(g_rx_evf, RX_WAKE);
	if (g_tx_evf >= 0)
		ksceKernelSetEventFlag(g_tx_evf, TX_WAKE);
}

/*
 * Where datagrams go (see ROAMING in proto.c). g_st.endpoint_ip
 * / endpoint_port are the configured (or resolved) endpoint and are never
 * moved by a roam: the endpoint /32 bypass route (routes.c) is pinned to
 * them. A roam (the source of a completed handshake response, when it is
 * not the configured endpoint) is kept separately; datagrams go there only
 * once routes.c has its own /32 via the Wi-Fi gateway in place
 * (transport_roam_routed), so they never enter a full tunnel's 0/1 +
 * 128/1. Each address is one 64-bit word (ip << 32 | port), read and
 * written atomically, so the TX step never sends to a torn ip:port.
 * Explicit changes (a new profile, vtSetEndpoint, connect, disconnect) go
 * back to the configured endpoint (transport_set_endpoint).
 */
static volatile uint64_t g_ep_pk, g_roam_pk;   /* ip << 32 | port; 0 = none */
static volatile uint32_t g_roam_routed;        /* the roam address whose /32 is in place */

static uint64_t pk(uint32_t ip, uint16_t port) { return (uint64_t)ip << 32 | port; }

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

void
transport_set_endpoint(uint32_t ip, uint16_t port)
{
	uint64_t old = __atomic_load_n(&g_roam_pk, __ATOMIC_RELAXED);

	g_st.endpoint_ip = ip;
	g_st.endpoint_port = port;
	__atomic_store_n(&g_ep_pk, pk(ip, port), __ATOMIC_SEQ_CST);
	__atomic_store_n(&g_roam_pk, 0, __ATOMIC_SEQ_CST);
	if (old)
		klog("transport: roam dropped: sending to the configured endpoint again");
}

uint32_t
transport_roam_ip(void)
{
	return (uint32_t)(__atomic_load_n(&g_roam_pk, __ATOMIC_SEQ_CST) >> 32);
}

void
transport_roam_routed(uint32_t ip)
{
	if (g_roam_routed != ip && ip)
		klog("transport: roam route in place: sending to the new address");
	g_roam_routed = ip;
}

void
transport_dest(uint32_t *ip, uint16_t *port)
{
	uint64_t r = __atomic_load_n(&g_roam_pk, __ATOMIC_SEQ_CST);
	uint64_t e = __atomic_load_n(&g_ep_pk, __ATOMIC_SEQ_CST);
	uint32_t rip = (uint32_t)(r >> 32);

	/* Same address, new port: the endpoint /32 covers it already. */
	if (r && (rip == (uint32_t)(e >> 32) || rip == g_roam_routed))
		e = r;
	*ip = (uint32_t)(e >> 32);
	*port = (uint16_t)e;
}

int
transport_is_own(uint32_t ip, uint16_t port)
{
	uint64_t r = __atomic_load_n(&g_roam_pk, __ATOMIC_SEQ_CST);

	if (ip == g_st.endpoint_ip && (port == 0 || port == g_st.endpoint_port))
		return 1;
	return r && ip == (uint32_t)(r >> 32) && (port == 0 || port == (uint16_t)r);
}

/*
 * The send gate. routes.c says, at the end of each
 * pass, whether a datagram to the endpoint can leave without entering the
 * tunnel: 0 while a route of ours covers the endpoint and its /32 is not in
 * place yet (connect under the Lockdown hold: 0/1 + 128/1 already point
 * into the tunnel interface). The TX step then leaves the queue as it is
 * (the ring keeps the newest 64) and sends once the gate opens; the first
 * initiation of such a connect therefore always leaves through the /32.
 */
static volatile int g_send_ok = 1;

void
transport_send_gate(int ok)
{
	__atomic_store_n(&g_send_ok, ok ? 1 : 0, __ATOMIC_RELEASE);
}

/* tnp roam sink: the source of a completed handshake response (under the
 * protocol lock: flags and a log line only). */
void
transport_roam(void *ctx, uint32_t ip, uint16_t port)
{
	uint64_t want = pk(ip, port), cur = __atomic_load_n(&g_roam_pk, __ATOMIC_SEQ_CST);

	(void)ctx;
	if (want == __atomic_load_n(&g_ep_pk, __ATOMIC_SEQ_CST)) {
		if (cur) {
			__atomic_store_n(&g_roam_pk, 0, __ATOMIC_SEQ_CST);
			klog("transport: peer answered from the configured endpoint again: roam dropped");
			routes_mark_dirty();
		}
		return;
	}
	if (want == cur)
		return;
	__atomic_store_n(&g_roam_pk, want, __ATOMIC_SEQ_CST);
	klog("transport: peer roamed: handshake answered from %u.%u.%u.%u:%u, not the configured "
	    "endpoint; sending there once its route is in place (the endpoint route stays)",
	    IP4(ip), (unsigned)port);
	routes_mark_dirty();
}

/* ---- sending (TX step, and the RX step's help) ------------------------- */

static int  tok_try(void) { return __atomic_exchange_n(&g_tx_tok, 1, __ATOMIC_ACQUIRE) == 0; }

/* Release the token; whatever is still queued goes to the TX thread. */
static void
tok_release(void)
{
	__atomic_store_n(&g_tx_tok, 0, __ATOMIC_RELEASE);
	if (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) != g_ring_tail && g_tx_evf >= 0)
		ksceKernelSetEventFlag(g_tx_evf, TX_WAKE);
}

/*
 * Pop and send up to `max` datagrams on socket s, in ring order. Caller
 * holds the send token. The ring lock is held only to copy one datagram out
 * (never across the sendto). by_rx: the RX thread's help (counted apart).
 * Stops (keeping the rest queued) when the send gate is shut, and drops
 * what it popped if the socket is being rebuilt or the console is going to
 * sleep, as before. Returns the datagrams sent (errors included).
 */
static int
tx_send_some(int s, int max, int by_rx, const char *phase_wd)
{
	static uint8_t out[TX_DGRAM_MAX];   /* the token's holder only */
	int wd = by_rx ? WD_RX : WD_TX, sent = 0;

	while (sent < max) {
		uint32_t len, qt, t0, dt;
		SockaddrIn to;
		int r;

		if (!__atomic_load_n(&g_send_ok, __ATOMIC_ACQUIRE))
			break;                  /* the send gate (TX step logs it) */
		tn_mutex_lock(g_ring_lock, "ring", &g_ring_holder);
		if (g_ring_head == g_ring_tail) {
			ksceKernelUnlockMutex(g_ring_lock, 1);
			break;
		}
		len = g_ring[g_ring_tail % TX_RING].len;
		qt = g_ring[g_ring_tail % TX_RING].t_us;
		memcpy(out, g_ring[g_ring_tail % TX_RING].buf, len);
		g_ring_tail++;
		ksceKernelUnlockMutex(g_ring_lock, 1);

		{
			uint32_t dip;
			uint16_t dport;
			transport_dest(&dip, &dport);
			sin_set(&to, dip, dport);
		}
		/* Reference first, then the id (see "Socket lifetime"). */
		__atomic_add_fetch(&g_sock_users, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&g_sock, __ATOMIC_SEQ_CST) != s || g_reopen || TN_QUIET()) {
			__atomic_sub_fetch(&g_sock_users, 1, __ATOMIC_SEQ_CST);
			break;                  /* being rebuilt, or going to sleep: drop */
		}
		wd_phase(wd, "udp sendto");
		t0 = now_us32();
		r = ksceNetSendto(s, out, len, 0, (const SceNetSockaddr *)&to, sizeof(to));
		__atomic_sub_fetch(&g_sock_users, 1, __ATOMIC_SEQ_CST);
		dt = now_us32() - t0;
		g_pf.send_us += dt;
		pf_max(&g_pf.send_max, dt);
		g_pf.q_us += t0 - qt;
		pf_max(&g_pf.q_max, t0 - qt);
		lh_add(&h_txq, t0 - qt);
		lh_add(by_rx ? &h_txq_rx : &h_txq_tx, t0 - qt);
		if (r < 0) {
			unsigned e = ++g_tx_errs;
			if (e <= 5 || e % 100 == 0)
				klog("transport: send 0x%08X (%u on this socket)", (unsigned)r, e);
			if (++g_tx_hard >= 20)
				transport_reopen("send errors");
		} else {
			g_tx_hard = 0;
			if (g_tx_ok++ == 0)
				klog("transport: first send on socket %d (type %u, %u bytes)",
				    s, (unsigned)out[0], (unsigned)len);
		}
		sent++;
	}
	if (sent) {
		g_pf.tx_sent += (uint32_t)sent;
		if (by_rx)
			g_pf.tx_by_rx += (uint32_t)sent;
	}
	if (phase_wd)
		wd_phase(wd, phase_wd);
	return sent;
}

/*
 * The RX step's help: if the oldest queued datagram has waited
 * RX_HELP_US the TX thread is not running (most often: this RX step holds
 * the core it would run on), so this thread sends a few itself, under the
 * token. Cheap when there is nothing to do: two loads.
 */
#define RX_HELP_US   250
#define RX_HELP_MAX  4

static void
rx_help_send(void)
{
	uint32_t tail = g_ring_tail;
	int s = g_sock;

	if (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) == tail || s < 0 ||
	    !__atomic_load_n(&g_send_ok, __ATOMIC_ACQUIRE))
		return;
	if (now_us32() - g_ring[tail % TX_RING].t_us < RX_HELP_US)
		return;                 /* the TX thread may well be on its way */
	if (!tok_try())
		return;                 /* it is sending now */
	tx_send_some(s, RX_HELP_MAX, 1, "udp recv");
	/* Emptied the queue: the kick is served, so the TX thread's wake (when
	 * it comes, maybe its 20 ms timeout) is not this datagram's latency. */
	if (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) == g_ring_tail &&
	    __atomic_exchange_n(&g_tx_kick_t, 0, __ATOMIC_RELAXED))
		g_pf.txw_served_rx++;
	tok_release();
}

/* ---- RX loop (vtTransportRx body) --------------------------------------- */

static uint8_t g_rx_kernel[TX_DGRAM_MAX];   /* kernel copy of a received datagram */

/*
 * A datagram the protocol lock was busy for: the
 * RX step never waits for that lock (tnp_input_try); the datagram stays in
 * g_rx_kernel and is fed again first thing on the next step, before any
 * new receive (which would overwrite it). Dropped with the socket.
 */
static uint32_t g_pend_len, g_pend_ip, g_pend_t;   /* g_pend_t: its recvfrom (us) */
static uint16_t g_pend_port;
static unsigned g_pend_busy;             /* busy answers, for the log */
static SceUInt64 g_pend_since, g_pend_told;

/*
 * A busy protocol lock is nearly always the interface TX thread sealing one
 * packet (a TCP ACK of the data this step just delivered: tens of
 * microseconds). Ending the step there cost the rest of the step and the
 * plugin's sleep (2 ms or more) for every collision, so the try is repeated
 * a few times, RX_BUSY_US apart, at most RX_BUSY_TRIES times (~1 ms), before
 * the datagram is kept as before. Still never a wait on the lock: a
 * bounded series of tries. A datagram already kept for longer than
 * RX_BUSY_FRESH_US (the holder is a control call or a handshake) gets a
 * single try per step, as before.
 */
#define RX_BUSY_TRIES    4
#define RX_BUSY_US       50
#define RX_BUSY_FRESH_US (5 * 1000)
/*
 * A nap of 50 us is really 0.5-1.5 ms (the
 * scheduler's floor), so 20 of them could hold a datagram - and, at the
 * same priority on the same core, every queued send - for up to ~20 ms.
 * The holder is nearly always the interface TX thread (0x3C, above us)
 * sealing one packet, on another core or preempting us: so the try is first
 * repeated without sleeping for up to RX_SPIN_US, then at most
 * RX_BUSY_TRIES naps, then the datagram is kept as before. The spin is
 * bounded and never a wait on the lock.
 */
#define RX_SPIN_US       200

/* 1 = fed (or dropped), 0 = still busy: stop this step. */
static int
feed(uint32_t len, uint32_t ip, uint16_t port, uint32_t t_recv)
{
	SceUInt64 now;
	uint32_t t0, tries = 0;
	int r, spun = 0;

	wd_phase(WD_RX, "tnp_input");
	(void)iface_rx_take_us();       /* "open" split: only this call's hand-off */
	t0 = now_us32();
	r = tnp_input_try(g_rx_kernel, len, ip, port);
	if (r != 0 && !(g_pend_len && ksceKernelGetSystemTimeWide() - g_pend_since > RX_BUSY_FRESH_US)) {
		uint32_t ts = now_us32();
		spun = 1;
		while (r != 0 && now_us32() - ts < RX_SPIN_US) {
			__asm__ volatile("yield" ::: "memory");
			t0 = now_us32();        /* "open" times the try that got in */
			r = tnp_input_try(g_rx_kernel, len, ip, port);
		}
		while (r != 0 && tries < RX_BUSY_TRIES) {
			if (tries++ == 0) {
				/* what this step delivered goes up before we nap */
				iface_rx_batch(0);
				iface_rx_batch(1);
				(void)iface_rx_take_us();
			}
			ksceKernelDelayThread(RX_BUSY_US);
			t0 = now_us32();
			r = tnp_input_try(g_rx_kernel, len, ip, port);
		}
	}
	if (r == 0) {
		uint32_t dt = now_us32() - t0, dl = iface_rx_take_us();
		g_pf.open_us += dt;
		g_pf.open_n++;
		g_pf.dlv_us += dl;
		g_pf.proto_us += dt > dl ? dt - dl : 0;
		lh_add(&h_rxp, dt > dl ? dt - dl : 0);
		pf_max(&g_pf.open_max, dt);
		lh_add(&h_rxo, t_recv ? now_us32() - t_recv : dt);
		if (tries)
			g_pf.rx_nap_ok++;
		else if (spun)
			g_pf.rx_spin_ok++;
		if (tries || spun)
			g_pf.rx_busy_ok++;
		if (g_pend_len && g_pend_told) {
			klog("watchdog: lent rx fed a datagram after waiting %u ms for the protocol lock",
			    (unsigned)((ksceKernelGetSystemTimeWide() - g_pend_since) / 1000));
		}
		g_pend_len = 0;
		g_pend_since = g_pend_told = 0;
		return 1;
	}
	g_pf.rx_busy_kept++;
	now = ksceKernelGetSystemTimeWide();
	if (!g_pend_len) {
		g_pend_len = len;
		g_pend_ip = ip;
		g_pend_port = port;
		g_pend_t = t_recv;
		g_pend_since = now;
		if (++g_pend_busy <= 5 || g_pend_busy % 1000 == 0)
			klog("transport: protocol lock busy (%u so far): datagram kept for the next step",
			    g_pend_busy);
	} else if (now - g_pend_since >= 1000000ULL && now - g_pend_told >= 10000000ULL) {
		uint32_t held = 0;
		const char *h = tnp_lock_holder(&held);
		g_pend_told = now;
		klog("watchdog: lent rx has waited %u ms for the protocol lock (held by %s for %u ms); not blocking",
		    (unsigned)((now - g_pend_since) / 1000), h ? h : "nobody now", (unsigned)held);
	}
	return 0;
}

/*
 * The receive buffer holds what arrives between two RX steps. 4.4BSD's
 * sbreserve refuses (ENOBUFS) a size over sb_max * MCLBYTES / (MSIZE +
 * MCLBYTES), about 227 KB with the usual 256 KB sb_max (assumed the
 * same here). The result used to go unchecked, so a refusal would
 * have left the default (about 41 KB, some 25 full-size datagrams). Tried
 * from the largest down; the size taken is logged and shown in the perf
 * line.
 */
static const int k_rcvbuf[] = { 256 * 1024, 220 * 1024, 192 * 1024, 128 * 1024, 64 * 1024 };

static int
make_socket(void)
{
	int s, tmo = 250 * 1000, rc = -1;
	unsigned i;
	SockaddrIn me;

	wd_phase(WD_RX, "udp socket");
	s = ksceNetSocket("vpnturbo_udp", 2, 2, 0);
	if (s < 0) {
		klog("transport: socket 0x%08X", (unsigned)s);
		g_st.last_error = VT_ERR_SOCKET;
		return s;
	}
	g_socks_open++;
	wd_phase(WD_RX, "udp setsockopt");
	ksceNetSetsockopt(s, SOL_SOCKET_, SO_RCVTIMEO_, &tmo, sizeof(tmo));
	g_pf.rcvbuf = 0;
	for (i = 0; i < sizeof(k_rcvbuf) / sizeof(k_rcvbuf[0]); i++) {
		int v = k_rcvbuf[i];
		rc = ksceNetSetsockopt(s, SOL_SOCKET_, SO_RCVBUF_, &v, sizeof(v));
		if (rc >= 0) {
			g_pf.rcvbuf = (uint32_t)v;
			break;
		}
	}
	if (g_pf.rcvbuf)
		klog("transport: socket %d receive buffer %u KB%s", s, (unsigned)(g_pf.rcvbuf / 1024),
		    i ? " (larger sizes refused)" : "");
	else
		klog("transport: socket %d receive buffer: every size refused (0x%08X), default kept",
		    s, (unsigned)rc);
	/* Bind to any address and the chosen (or random) source port; the
	 * endpoint /32 route keeps egress on wlan0. */
	sin_set(&me, 0, g_st.listen_port);
	wd_phase(WD_RX, "udp bind");
	{
		int b = ksceNetBind(s, (const SceNetSockaddr *)&me, sizeof(me));
		if (b < 0)
			klog("transport: bind 0x%08X", (unsigned)b);
	}
	return s;
}

/*
 * The lent SceShell threads call these in a loop and each call returns
 * quickly: a SceShell thread that
 * stayed inside the module, blocked in recvfrom for good, was followed by the
 * console restarting about 30 s later, and the socket's receive timeout was
 * not honoured in that context. So the RX step never blocks (MSG_DONTWAIT)
 * and the TX step waits at most TX_STEP_US; the plugin sleeps between idle
 * RX steps.
 */
#define MSG_DONTWAIT_ 0x80
/*
 * One RX step drains the socket until it is empty (EAGAIN), bounded by
 * RX_STEP_MAX datagrams and RX_STEP_US of receiving (checked every 16), so
 * a flood never keeps SceShell's thread in the module for long. Was 32 per
 * step; at 2 ms between steps a burst of a TCP window (a few dozen
 * datagrams) then waited a whole extra step.
 */
#define RX_STEP_MAX   256
#define RX_STEP_US    (4 * 1000)
/* The budget is checked every RX_STEP_CHECK datagrams (was 16:
 * with ~125 us a datagram that let a step run 2 ms past its budget). The device test's
 * "max 32, cap 2.2k" were steps that hit the 4 ms at 16 or 32 datagrams,
 * the time mostly the ~1 ms naps of the lock retries (now a spin). */
#define RX_STEP_CHECK 4
/* Tunnel off ("Idle cost"): the steps wait up to this long on an
 * event flag instead of returning to a 20 ms poll. A bounded kernel wait
 * that holds nothing of net_ps (unlike a blocking recvfrom) and
 * ends at once on any change (tn_wake); then the step returns, as always. */
#define RX_IDLE_US    (500 * 1000)
#define TX_IDLE_US    (500 * 1000)
/* Route work at most this often while steps come faster than the plugin's
 * old 2 ms (it ran every step; "routes_dirty" still runs it at once). */
#define ROUTES_EVERY_US (2 * 1000)
#define TX_STEP_US    (20 * 1000)

static int rx_step(void *user_buf, uint32_t size);
static int rx_drain(void *user_buf, uint32_t size);

static int g_rx_may_idle;      /* set by this step: nothing to do until woken */

/* No socket of ours open or parked (the idle wait never delays a close). */
static int no_socket(void) { return g_sock < 0 && g_sock_closing < 0; }

int
transport_rx_loop(void *user_buf, uint32_t size)
{
	int r;

	/* The plugin's buffer must hold the largest datagram and the route
	 * dump (VT_LEND_RX_BUF_SIZE); a smaller one would cut datagrams
	 * silently. Everything below reads and writes at most TX_DGRAM_MAX of
	 * it, which is the same number. */
	if (size < VT_LEND_RX_BUF_SIZE || !user_buf)
		return -(int)VT_ERR_BUFFER;
	if (size > TX_DGRAM_MAX)
		size = TX_DGRAM_MAX;
	g_run = 1;
	g_wakes.rx++;
	g_rx_may_idle = 0;

	wd_begin(WD_RX);
	if (TN_NO_NETPS()) {
		/* Suspended, or waiting for the network after a wake: nothing
		 * until the release (iface_rearm -> tn_wake) or a reopen. */
		r = 0;
		g_rx_may_idle = 1;
	} else if (g_st.paused) {
		/* Paused: no socket work, but the route table is still brought
		 * in line here (a pause whose removals found the table busy
		 * left them to this step; the restore check runs here too). */
		net_ready_work_done("tunnel paused");
		wd_phase(WD_RX, "routes");
		routes_apply(user_buf, size);
		r = 0;
		g_rx_may_idle = routes_quiet() && no_socket();
	} else {
		r = rx_step(user_buf, size);
	}
	wd_end(WD_RX);
	/* Idle (tunnel off, nothing pending): wait for a change instead of
	 * returning to the plugin's 20 ms poll. Outside wd_begin/wd_end: it
	 * is not work. Nothing is held; the next step re-checks everything,
	 * a suspend that began meanwhile included, before any net_ps call. */
	if (r == 0 && g_rx_may_idle && g_run && g_rx_evf >= 0) {
		unsigned int bits = 0;
		SceUInt32 to = RX_IDLE_US;
		g_pf.rx_idle_waits++;
		ksceKernelWaitEventFlag(g_rx_evf, RX_WAKE, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
		    &bits, &to);
	}
	return r;
}

static void
sock_close_now(int s, const char *what)
{
	wd_phase(WD_RX, "udp close");
	ksceNetClose(s);
	g_socks_open--;
	klog("transport: socket closed (%s; %u sent, %u send errors, %u received, %u recv errors)",
	    what, g_tx_ok, g_tx_errs, g_rx_ok, g_rx_errs);
}

/* A parked socket (see "Socket lifetime"): closed once no TX step uses it.
 * Returns 1 while one is still parked. */
static int
sock_reap(void)
{
	int s = g_sock_closing;

	if (s < 0)
		return 0;
	if (__atomic_load_n(&g_sock_users, __ATOMIC_SEQ_CST) != 0)
		return 1;
	g_sock_closing = -1;
	sock_close_now(s, g_closing_what ? g_closing_what : "deferred");
	return 0;
}

/* Close the socket: unpublish it, then close it unless a TX step holds a
 * reference (waits up to 20 ms for that, never longer; then it is parked). */
static void
sock_close(const char *what)
{
	int old = g_sock, i;

	g_pend_len = 0;                 /* a datagram kept for the lock goes with it */
	g_pend_since = g_pend_told = 0;
	__atomic_store_n(&g_sock, -1, __ATOMIC_SEQ_CST);
	if (old < 0)
		return;
	for (i = 0; i < 20 && __atomic_load_n(&g_sock_users, __ATOMIC_SEQ_CST) != 0; i++)
		ksceKernelDelayThread(1000);
	if (__atomic_load_n(&g_sock_users, __ATOMIC_SEQ_CST) != 0) {
		g_closing_what = what;
		g_sock_closing = old;
		klog("transport: socket %d still in a send: closed on a later step (%s)", old, what);
		return;
	}
	sock_close_now(old, what);
}

static int
rx_step(void *user_buf, uint32_t size)
{
	/* Tunnel not wanted: no socket of ours stays open, as
	 * if the module were not there. It is made again on the next step
	 * after a connect; an initiation queued meanwhile waits in the ring. */
	sock_reap();
	if (!g_st.want) {
		if (g_sock >= 0)
			sock_close("tunnel off");
		g_reopen = 0;
		net_ready_work_done("tunnel not running");
		wd_phase(WD_RX, "routes");
		routes_apply(user_buf, size);
		g_rx_may_idle = routes_quiet() && no_socket() && !g_st.want;
		return 0;
	}
	if (g_reopen && g_sock >= 0)
		sock_close(g_reopen_why ? (const char *)g_reopen_why : "rebuild");

	/* Routes are reconciled here, on every step at most ROUTES_EVERY_US
	 * apart (2 ms: the old step cadence; at once when routes_dirty() or
	 * while there is no socket): this is a SceShell user context, the only
	 * place a PF_ROUTE reply (gateway, subnet, the change monitor) can be
	 * read back. Cheap when nothing changed. Before
	 * the socket is (re)made: the restore check's before-connect snapshot
	 * is taken in here and must see the console as it was, and a socket
	 * that cannot be made must not hold up the route work. */
	{
		static SceUInt64 last;
		SceUInt64 t = ksceKernelGetSystemTimeWide();
		if (g_sock < 0 || routes_dirty() || t - last >= ROUTES_EVERY_US) {
			uint32_t dt;
			last = t;
			wd_phase(WD_RX, "routes");
			routes_apply(user_buf, size);
			dt = (uint32_t)(ksceKernelGetSystemTimeWide() - t);
			g_pf.routes_us += dt;
			g_pf.routes_n++;
			pf_max(&g_pf.routes_max, dt);
		} else {
			g_pf.routes_skip++;
		}
	}
	if (TN_NO_NETPS())
		return 0;       /* the Wi-Fi went away during the route work */

	/* The socket is created on an RX step, so SceShell owns it; not while
	 * the previous one is still parked in a send. */
	if (g_sock < 0 && sock_reap())
		return 0;
	if (g_sock < 0) {
		int s = make_socket();
		if (s < 0)
			return s;
		g_reopen = 0;
		g_rx_hard = g_tx_hard = g_rx_errs = g_tx_errs = g_tx_ok = g_rx_ok = 0;
		g_sock_id = s;
		g_rxs.idx = 0;          /* the counts run on; a new run starts */
		__atomic_store_n(&g_sock, s, __ATOMIC_SEQ_CST);
		klog("transport: rx up, socket %d", s);
		if (g_tx_evf >= 0)
			ksceKernelSetEventFlag(g_tx_evf, TX_WAKE);   /* queued initiation: send now */
	}

	return rx_drain(user_buf, size);
}

/*
 * The receive half of a step: the kept datagram first, then the socket until
 * it is empty or the step's budget is spent. Decrypted packets go up in one
 * RX-ready batch (iface_rx_batch). Returns the datagrams taken; 1 if none
 * but one is kept for a lock that was busy only a moment ago (the plugin
 * then polls again soon instead of backing off); 0 if idle.
 */
static int
rx_drain(void *user_buf, uint32_t size)
{
	static SceUInt64 last_end, last_data;
	SceUInt64 t_start = ksceKernelGetSystemTimeWide();
	int got = 0, cap = 0;
	uint32_t bytes = 0;

	/* Idle time before this step while traffic flows (a datagram in the
	 * last 100 ms): how long the plugin's sleep really was. */
	if (last_end && last_data && t_start - last_data < 100000ULL) {
		uint32_t g = (uint32_t)(t_start - last_end);
		g_pf.gap[g < 500 ? 0 : g < 1500 ? 1 : g < 3000 ? 2 : 3]++;
		pf_max(&g_pf.gap_max, g);
		lh_add(&h_rxg, g);
	}
	g_pf.rx_steps++;
	g_rx_cpu = cpu_id();
	g_pf.cpu_rx[g_rx_cpu]++;
	g_rx_in_step = 1;
	iface_rx_batch(1);
	rx_help_send();

	/* A datagram the protocol lock was busy for, first (see feed). */
	if (g_pend_len) {
		if (!feed(g_pend_len, g_pend_ip, g_pend_port, g_pend_t))
			goto out;
		got++;
	}

	wd_phase(WD_RX, "udp recv");
	while (got < RX_STEP_MAX) {
		SockaddrIn from;
		unsigned int fromlen = sizeof(from);
		uint32_t t0;
		int n;

		if (got && (got % RX_STEP_CHECK) == 0 &&
		    ksceKernelGetSystemTimeWide() - t_start >= RX_STEP_US) {
			cap = 1;        /* the budget is spent: the rest next step */
			break;
		}
		memset(&from, 0, sizeof(from));
		t0 = now_us32();
		n = ksceNetRecvfrom(g_sock, user_buf, size, MSG_DONTWAIT_,
		    (SceNetSockaddr *)&from, &fromlen);
		{
			uint32_t t1 = now_us32();
			g_pf.recv_us += t1 - t0;
			g_pf.recv_n++;
			if (n > 0) {
				g_pf.recv_data_us += t1 - t0;
				g_pf.recv_data_n++;
				lh_add(&h_rcv, t1 - t0);
			} else {
				g_pf.recv_empty_us += t1 - t0;
				g_pf.recv_empty_n++;
			}
			t0 = t1;        /* from here: the datagram's time with us */
		}
		if (n <= 0) {
			/* -35 (EAGAIN): nothing waiting. */
			if (n != -35 && n != 0) {
				unsigned e = ++g_rx_errs;
				if (e <= 5 || e % 100 == 0)
					klog("transport: recv 0x%08X (%u on this socket)", (unsigned)n, e);
				if (++g_rx_hard >= 20)
					transport_reopen("recv errors");
			}
			break;
		}
		g_rx_hard = 0;
		got++;
		bytes += (uint32_t)n;
		if ((uint32_t)n > sizeof(g_rx_kernel))
			continue;
		/* Only the kernel copy is parsed from here on (tnp_input). A
		 * failed copy drops the datagram: g_rx_kernel may hold the
		 * previous one, which must not be fed in again. */
		if (ksceKernelMemcpyUserToKernel(g_rx_kernel, user_buf, (SceSize)n) < 0) {
			unsigned e = ++g_rx_errs;
			if (e <= 5 || e % 100 == 0)
				klog("transport: copy of a received datagram failed: dropped (%u on this socket)", e);
			continue;
		}
		rx_seq_note(&g_rxs, g_rx_kernel, (uint32_t)n);
		{
			uint32_t ip = ((from.addr & 0xff) << 24) | ((from.addr & 0xff00) << 8) |
			    ((from.addr >> 8) & 0xff00) | (from.addr >> 24);
			uint16_t port = (uint16_t)((from.port >> 8) | (from.port << 8));
			if (g_rx_ok++ == 0)
				klog("transport: first datagram on socket %d (type %u, %d bytes, %s the endpoint)",
				    g_sock, (unsigned)g_rx_kernel[0], n,
				    transport_is_own(ip, 0) ? "from" : "NOT from");
			if (!feed((uint32_t)n, ip, port, t0))
				break;          /* kept: fed again on the next step */
			wd_phase(WD_RX, "udp recv");
			rx_help_send();
		}
	}
	if (got >= RX_STEP_MAX)
		cap = 1;
out:
	iface_rx_batch(0);
	rx_help_send();
	g_rx_in_step = 0;
	last_end = ksceKernelGetSystemTimeWide();
	if (got) {
		last_data = last_end;
		lh_add(&h_rxs, (uint32_t)(last_end - t_start));
		pf_max(&g_pf.rx_bytes_max, bytes);
		g_pf.rx_steps_data++;
		g_pf.rx_dg += (uint32_t)got;
		pf_max(&g_pf.rx_max, (uint32_t)got);
		g_pf.rx_cap += (uint32_t)cap;
		return got;
	}
	return g_pend_len && last_end - g_pend_since <= RX_BUSY_FRESH_US ? 1 : 0;
}

/* ---- TX step (vtTransportTx body) ---------------------------------------- */

int
transport_tx_loop(void *user_buf, uint32_t size)
{
	static unsigned tx_errs;
	static int announced;
	unsigned int bits = 0;
	SceUInt32 timeout = TX_STEP_US;
	int s = g_sock, sent = 0;

	(void)user_buf; (void)size;
	g_run = 1;
	g_wakes.tx++;
	if (s < 0 || TN_QUIET() || g_reopen) {
		/* No socket yet (the RX step makes it and wakes us), quiet, or a
		 * rebuild. Tunnel off: an idle wait of up to TX_IDLE_US that any
		 * change ends (tn_wake); otherwise the old 20 ms. An event-flag
		 * wait holds nothing (only a net_ps call must not block). */
		timeout = (!g_st.want || g_st.paused || TN_NO_NETPS()) ? TX_IDLE_US : TX_STEP_US;
		if (g_tx_evf >= 0) {
			SceUInt64 t0 = ksceKernelGetSystemTimeWide();
			int w = ksceKernelWaitEventFlag(g_tx_evf, TX_WAKE,
			    SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
			/* On the device this wait came back at once for a
			 * whole pause, ~12,000 steps a second on SceShell's
			 * highest-priority thread, which starved SceShell (no
			 * frames drawn) until the console died. Never faster
			 * than the step pace here, whatever the wait does. */
			if (ksceKernelGetSystemTimeWide() - t0 < 1000) {
				static unsigned told;
				if (told++ < 5)
					klog("transport: quiet tx wait returned at once (0x%08X, bits 0x%X, "
					    "paused %d, sock %d, reopen %d)", (unsigned)w, bits,
					    g_st.paused, s, g_reopen);
				ksceKernelDelayThread(TX_STEP_US);
			}
		} else {
			ksceKernelDelayThread(TX_STEP_US);
		}
		return 0;
	}
	if (!announced) {
		announced = 1;
		klog("transport: tx up (evf 0x%08X)", (unsigned)g_tx_evf);
	}

	wd_begin(WD_TX);
	/* Something queued already (left by a shut gate or the RX step's
	 * help) and the token free: no wait. Otherwise wait for an enqueue
	 * or the token's release (tok_release wakes us if anything is left),
	 * at most 20 ms; never a spin while the RX step holds the token. */
	if (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) == g_ring_tail ||
	    __atomic_load_n(&g_tx_tok, __ATOMIC_ACQUIRE)) {
		wd_phase(WD_TX, "wait");
		int w = ksceKernelWaitEventFlag(g_tx_evf, TX_WAKE,
		    SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
		/* 0x80028005 is the wait timing out; any other error returns at once. */
		if (w < 0 && (unsigned)w != 0x80028005u) {
			if (tx_errs++ < 5)
				klog("transport: tx wait 0x%08X", (unsigned)w);
			ksceKernelDelayThread(TX_STEP_US);
		}
	}
	/* Wake latency: the first enqueue since we last woke -> now. */
	{
		uint32_t kt = __atomic_exchange_n(&g_tx_kick_t, 0, __ATOMIC_RELAXED);
		if (kt) {
			uint32_t dt = (now_us32() | 1u) - kt;
			unsigned c = cpu_id();
			lh_add(&h_txw, dt);
			g_pf.cpu_tx[c]++;
			if (dt > 2000) {
				g_pf.txw_late++;
				g_pf.txw_late_rx += (uint32_t)(g_kick_rx != 0);
				g_pf.cpu_late[c]++;
				g_pf.late_same_cpu += (uint32_t)(c == g_rx_cpu);
			}
		}
	}

	/* The send gate: nothing to the endpoint before its /32 (above). */
	{
		static SceUInt64 held_since;
		int ok = __atomic_load_n(&g_send_ok, __ATOMIC_ACQUIRE);
		if (!ok) {
			if (!held_since) {
				held_since = ksceKernelGetSystemTimeWide();
				klog("transport: %u datagram(s) held until the endpoint route is in place",
				    (unsigned)(g_ring_head - g_ring_tail));
			}
			wd_end(WD_TX);
			if (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) != g_ring_tail)
				ksceKernelDelayThread(TX_STEP_US);   /* polled at the old pace */
			return 0;
		}
		if (held_since) {
			klog("transport: endpoint route in place after %u ms: sending (%u queued)",
			    (unsigned)((ksceKernelGetSystemTimeWide() - held_since) / 1000),
			    (unsigned)(g_ring_head - g_ring_tail));
			held_since = 0;
		}
	}

	/* Drain the ring under the send token (in order; see g_tx_tok). The
	 * RX step's help holds it for at most RX_HELP_MAX sends; then
	 * tok_release wakes us again if anything is left. */
	while (__atomic_load_n(&g_ring_head, __ATOMIC_ACQUIRE) != g_ring_tail) {
		int n;
		if (!tok_try())
			break;
		n = tx_send_some(s, TX_RING, 0, NULL);
		__atomic_store_n(&g_tx_tok, 0, __ATOMIC_RELEASE);
		sent += n;
		if (n == 0 || __atomic_load_n(&g_sock, __ATOMIC_SEQ_CST) != s || g_reopen || TN_QUIET())
			break;
	}
	if (sent) {
		g_pf.tx_steps++;
		pf_max(&g_pf.tx_max, (uint32_t)sent);
	}
	wd_end(WD_TX);
	return sent;
}

/*
 * The perf lines (interface TX thread, with the 30 s liveness line while
 * UP). Differences since the last call; averages in microseconds.
 */
static uint32_t pf_take(uint32_t *m) { return __atomic_exchange_n(m, 0, __ATOMIC_RELAXED); }
static unsigned pf_avg(uint32_t sum, uint32_t n) { return n ? (unsigned)(sum / n) : 0u; }

void
transport_perf_log(void)
{
	static struct tperf o;
	static struct rx_seq so;
	static struct lhist o_txq, o_txw, o_rxo, o_rxg, o_rxs;
	struct tperf d;
	uint32_t *pd = (uint32_t *)&d, *pn = (uint32_t *)&g_pf, *po = (uint32_t *)&o;
	unsigned i;

	/* Every field is a uint32_t: the struct is walked as an array. */
	for (i = 0; i < sizeof(d) / sizeof(uint32_t); i++) {
		uint32_t v = pn[i];
		pd[i] = v - po[i];
		po[i] = v;
	}
	d.rx_max = pf_take(&g_pf.rx_max);
	d.gap_max = pf_take(&g_pf.gap_max);
	d.open_max = pf_take(&g_pf.open_max);
	d.routes_max = pf_take(&g_pf.routes_max);
	d.tx_max = pf_take(&g_pf.tx_max);
	d.q_max = pf_take(&g_pf.q_max);
	d.send_max = pf_take(&g_pf.send_max);
	d.ring_hw = pf_take(&g_pf.ring_hw);
	d.rx_bytes_max = pf_take(&g_pf.rx_bytes_max);
	o.rx_max = o.gap_max = o.open_max = o.routes_max = o.tx_max = o.q_max = 0;
	o.send_max = o.ring_hw = o.rx_bytes_max = 0;

	klog("perf rx: %u dg %u steps (%u data, max %u, cap %u)",
	    d.rx_dg, d.rx_steps, d.rx_steps_data, d.rx_max, d.rx_cap);
	klog("perf rx: lost %u late %u jumps %u runs %u; lock busy ok %u kept %u; rcvbuf %u KB",
	    g_rxs.lost - so.lost, g_rxs.late - so.late, g_rxs.jumps - so.jumps,
	    g_rxs.restarts - so.restarts, d.rx_busy_ok, d.rx_busy_kept,
	    (unsigned)(g_pf.rcvbuf / 1024));
	klog("perf rx: gap <.5ms %u <1.5 %u <3 %u more %u max %u us",
	    d.gap[0], d.gap[1], d.gap[2], d.gap[3], d.gap_max);
	klog("perf rx: recv %u us open %u us max %u; routes %u x %u us max %u, %u skipped",
	    pf_avg(d.recv_us, d.recv_n), pf_avg(d.open_us, d.open_n), d.open_max,
	    d.routes_n, pf_avg(d.routes_us, d.routes_n), d.routes_max, d.routes_skip);
	klog("perf tx: %u sent %u steps max %u; ring %u drop %u",
	    d.tx_sent, d.tx_steps, d.tx_max, d.ring_hw, d.ring_drop);
	klog("perf tx: queue %u us max %u; sendto %u us max %u",
	    pf_avg(d.q_us, d.tx_sent), d.q_max, pf_avg(d.send_us, d.tx_sent), d.send_max);
	/* Where the loss and the reordering are, and the spread. */
	klog("perf rx: gone %u fill %u dup %u old %u; depth 1:%u 2-3:%u 4-15:%u 16+:%u max %u",
	    g_rxs.gone - so.gone, g_rxs.filled - so.filled, g_rxs.dup - so.dup,
	    g_rxs.too_late - so.too_late, g_rxs.depth[0] - so.depth[0], g_rxs.depth[1] - so.depth[1],
	    g_rxs.depth[2] - so.depth[2], g_rxs.depth[3] - so.depth[3], (unsigned)g_rxs.depth_max);
	klog("perf rx: busy spin %u nap %u; backlog max %u/%u KB; tx by rx %u of %u",
	    d.rx_spin_ok, d.rx_nap_ok, (unsigned)(d.rx_bytes_max / 1024), (unsigned)(g_pf.rcvbuf / 1024),
	    d.tx_by_rx, d.tx_sent);
	g_rxs.depth_max = 0;
	lh_log("lat tx queue", &h_txq, &o_txq);
	lh_log("lat tx wake", &h_txw, &o_txw);
	lh_log("lat rx open", &h_rxo, &o_rxo);
	lh_log("lat rx gap", &h_rxg, &o_rxg);
	lh_log("lat rx step", &h_rxs, &o_rxs);
	/* Timing counters: where the receive and send time goes. */
	klog("perf rx: recvfrom data %u us x %u, empty %u us x %u",
	    pf_avg(d.recv_data_us, d.recv_data_n), d.recv_data_n,
	    pf_avg(d.recv_empty_us, d.recv_empty_n), d.recv_empty_n);
	klog("perf rx: open %u us = protocol %u + hand-off %u",
	    pf_avg(d.open_us, d.open_n), pf_avg(d.proto_us, d.open_n), pf_avg(d.dlv_us, d.open_n));
	klog("perf tx: wakes >2 ms %u (kicked mid rx step %u, on rx core %u); served by rx %u",
	    d.txw_late, d.txw_late_rx, d.late_same_cpu, d.txw_served_rx);
	klog("perf cpu: rx steps %u/%u/%u/%u, tx wakes %u/%u/%u/%u",
	    d.cpu_rx[0], d.cpu_rx[1], d.cpu_rx[2], d.cpu_rx[3],
	    d.cpu_tx[0], d.cpu_tx[1], d.cpu_tx[2], d.cpu_tx[3]);
	klog("perf cpu: late tx wakes %u/%u/%u/%u, kicks %u/%u/%u/%u",
	    d.cpu_late[0], d.cpu_late[1], d.cpu_late[2], d.cpu_late[3],
	    d.cpu_kick[0], d.cpu_kick[1], d.cpu_kick[2], d.cpu_kick[3]);
	{
		static struct lhist o_q_tx, o_q_rx, o_rxp, o_rcv;
		lh_log("lat txq by tx", &h_txq_tx, &o_q_tx);
		lh_log("lat txq by rx", &h_txq_rx, &o_q_rx);
		lh_log("lat rx protocol", &h_rxp, &o_rxp);
		lh_log("lat rx recvfrom", &h_rcv, &o_rcv);
	}
	so = g_rxs;
}

void
transport_stop(void)
{
	g_run = 0;
	transport_wake();
}

void
transport_set_paused(int paused)
{
	(void)paused;   /* the loops read g_st.paused directly */
}

/* Created at module start, in the kernel process, before any lending. */
void
transport_init(void)
{
	g_ring_lock = ksceKernelCreateMutex("vpnturbo_ring", 0, 0, NULL);
	g_tx_evf = ksceKernelCreateEventFlag("vpnturbo_txq", 0, 0, NULL);
	g_rx_evf = ksceKernelCreateEventFlag("vpnturbo_rxw", 0, 0, NULL);
	routes_init();
}
