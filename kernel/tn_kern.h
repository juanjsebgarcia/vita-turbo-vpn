/*
 * tn_kern.h - internal glue of vpnturbo_kernel, shared by its .c files.
 * Nothing here is part of the public ABI (that is ../include/vpnturbo.h). This
 * header does NOT include tn_core.h: only proto.c talks to the OpenBSD core,
 * so the rest of the module keeps the Vita's own net headers without the
 * BSD-kernel name clashes the shim introduces.
 */
#ifndef TN_KERN_H
#define TN_KERN_H

#include <psp2kern/types.h>
#include <stdint.h>

#define VPNTURBO_NO_PROTOTYPES
#include "../include/vpnturbo.h"
#include "tn_proto.h"

/* ---- descriptor contract (net_ps's driver descriptor) ------------------- */

enum {
	D_ALLOC_PKT = 4, D_FREE_PKT = 6, D_RX = 7, D_TX_DEQ = 8, D_EVENT = 9,
	D_SET_MTU = 10, D_INIT = 0x10, D_STOP = 0x11, D_START_TX = 0x12,
	D_MCAST = 0x13, D_IOCTL = 0x14, D_CAPS = 0x15, D_TYPE = 0x16,
	D_MAC = 0x17, D_MAX_MTU = 0x19, D_HEADROOM = 0x1a, D_SOFTC = 0x1b,
	D_WORDS = 0x20,
};

#define DEFAULT_MTU  1420       /* matches tn_core.h */
#define TYPE_OTHER   5          /* a type outside 1-4 gives other0 */
#define SOFT_LINK_UP 1
#define SOFT_RX_READY 4

typedef void *(*alloc_pkt_fn)(void *desc, uint32_t len, uint32_t align);
typedef void (*free_pkt_fn)(void *desc, void *pkt);
typedef int (*rx_fn)(void *desc, void *pkt);
typedef void *(*tx_deq_fn)(void *desc);
typedef int (*event_fn)(void *desc, int unused, uint32_t publish, uint32_t soft, void *data, uint32_t len);
typedef int (*set_mtu_fn)(void *desc, uint32_t mtu);

extern uint32_t g_desc[D_WORDS] __attribute__((aligned(16)));
#define SVC(type, i) ((type)g_desc[i])

extern const uint8_t k_our_mac[6];
extern const uint8_t k_peer_mac[6];

/* ---- shared state -------------------------------------------------------- */

/* The running configuration, taken from the last accepted VtProfile. Keys do
 * NOT live here (they are inside the protocol core); only routing-relevant
 * fields and display fields are kept. */
struct tn_state {
	int      configured;          /* a profile is loaded */
	int      want;                /* the tunnel is wanted (connect, not paused) */
	int      paused;              /* vtPause: tunnel and routes off, stay out of net_ps */
	uint32_t pause_until_s;       /* uptime seconds a vtPause ends by itself */

	uint32_t endpoint_ip;         /* host order, from the profile or vtSetEndpoint; never
	                               * moved by a roam (transport_set_endpoint) */
	uint16_t endpoint_port;
	uint16_t listen_port;         /* 0 = random */
	uint32_t tunnel_address;      /* host order */
	uint8_t  tunnel_prefix;
	uint16_t mtu;                 /* 0 -> DEFAULT_MTU */
	uint16_t flags;               /* VT_PROFILE_* */
	struct tnp_prefix allowed[VT_MAX_ALLOWED];
	unsigned allowed_count;
	int      full_tunnel;
	char     name[VT_NAME_MAX];
	uint32_t ping_target;         /* host order, latency pings (latency.c); 0 = none */

	/* options (kept in ur0:data/vpnturbo/options.txt, control.c) */
	uint8_t  kill_switch;         /* only while the tunnel is wanted */
	uint8_t  allow_local;
	volatile uint8_t lockdown;    /* Lockdown mode: only tunnel traffic, ever */
	volatile uint8_t fast_send;   /* faster sending: the plugin's TX thread off the system core */

	/* status bridged out of proto on every tick */
	int      state;               /* VT_STATE_* */
	int      last_error;          /* VT_ERR_* */
	uint32_t exit_ip;             /* for the HUD; set by the app, drawn later */

	/* interface */
	int      attached;
	volatile int iface_up;        /* IFF_UP set (desc[0x10]); the address is in place */
	char     ifname[16];          /* "other0" once attached */
	uint32_t wlan_address;        /* host order; learnt by routes.c (RTM_GET), 0 = unknown */
	uint32_t wlan_gateway;        /* host order; discovered for the endpoint route */

	/* protection: resume lockdown, kill-switch hold, allow
	 * local. Written by control.c, suspend.c and routes.c. */
	volatile int suspended;       /* first suspend event .. 0x400000: stay out of
	                               * net_ps; separate from a user pause */
	volatile int net_wait;        /* after a wake (or a deep called-off suspend)
	                               * until the network is reported ready
	                               * (vtNetReady): still no net_ps call at all */
	volatile int locked;          /* tunnel traffic held until a fresh handshake */
	uint32_t lock_hs;             /* tnp handshake count when the lock was armed */
	volatile int ever_up;         /* UP since this connect: with the kill switch off
	                               * the routes stay through a rehandshake */
	volatile uint32_t lock_drops; /* outbound packets dropped while held, this connection */
	volatile uint32_t iface_gen;  /* bumped at every desc[0x10] init (routes refresh) */
	uint8_t  wlan_prefix;         /* Wi-Fi subnet prefix, 0 = unknown */
	volatile uint8_t route_flags; /* VT_RT_* now in place (routes.c) */
};

/* Stay out of net_ps: a user pause, a suspend in progress, or a wake
 * whose network is not confirmed ready yet (net_wait). */
#define TN_QUIET() (g_st.paused || g_st.suspended || g_st.net_wait)
/* The same without the user pause: no net_ps call of any kind, removals
 * included (routes.c). */
#define TN_NO_NETPS() (g_st.suspended || g_st.net_wait)

/* The kill switch in effect: the setting, or Lockdown (which implies it). */
#define TN_KILL() (g_st.kill_switch || g_st.lockdown)
/* Lockdown with the tunnel not wanted: everything held in the tunnel
 * interface (VT_HOLD_LOCKDOWN_MODE). Needs a profile (its address). */
#define TN_LD_HOLD() (g_st.lockdown && g_st.configured && !g_st.want && !g_st.paused)

extern struct tn_state g_st;

/* ---- klog (klog.c) ------------------------------------------------------- */

void klog_init(void);
void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- interface (iface.c) ------------------------------------------------- */

int  iface_start_threads(void);   /* create the TX thread at module start */
int  iface_attach(void);          /* scePsNetDriverIfAttach + MTU + link up */
void iface_kick_tx(void);         /* wake the TX thread (resume) */
void iface_strand_wake(void);     /* wake it for the stranded-connection reset (strand.c) */
/* Re-arm output (iface_rearm.h): link-up + one dequeue pass on the TX
 * thread once net_ps may be called. Flags only; `why` a literal. A wake's
 * network-ready, every connect, a resume from pause, the self-heal. */
void iface_rearm(const char *why);
void iface_set_paused(int paused);
void iface_rx_deliver(void *ctx, const void *ip, uint32_t len);  /* tnp deliver sink */
uint32_t iface_rx_take_us(void);
void iface_addr_added(void);          /* the tunnel address appeared, the interface kept UP */
void net_lost(const char *why);       /* the Wi-Fi address went away: wait as after a wake */
int  options_restart_pending(void);   /* a faster-sending change: banner, then restart */
void options_restart_tick(void);      /* timer thread */   /* deliver time since the last call (lent RX thread) */
/* Lent RX thread, around the decrypt-and-deliver loop of one step: 1 = the
 * RX-ready signal (desc[9] bit 4) goes out for the first packet, every
 * RX_SIGNAL_EVERY after it and once at the end (0), not for every packet. */
void iface_rx_batch(int on);
/* Addresses the synthetic ARP answered since the last take (each one is a
 * host entry net_ps cloned on the tunnel interface). Copies up to `cap`,
 * clears the set; *lost = answered but not tracked (set full). */
int  iface_arp_take(uint32_t *out, int cap, unsigned *lost);
/* Counters for the restore check: frames seen, frames that reached the
 * interface while it was off (no tunnel wanted, no Lockdown), held. */
void iface_counts(uint32_t *frames, uint32_t *off, uint32_t *held);

/* ---- transport (transport.c) --------------------------------------------- */

int  transport_tx_enqueue(void *ctx, const void *buf, uint32_t len);  /* tnp send sink */
void transport_roam(void *ctx, uint32_t ip, uint16_t port);           /* tnp roam sink */
/* The configured endpoint (profile, vtSetEndpoint); drops any roam. Control
 * paths only (under the control mutex). */
void transport_set_endpoint(uint32_t ip, uint16_t port);
/* The address a completed handshake answered from when it is not the
 * configured endpoint (0 = none): routes.c gives it a /32 too. */
uint32_t transport_roam_ip(void);
/* routes.c: the roam address whose /32 is now in place (0 = none). */
void transport_roam_routed(uint32_t ip);
/* routes.c, end of each pass: 1 if a datagram to the endpoint can leave
 * without entering the tunnel (no route of ours covers it, or its /32 is
 * in place). 0 holds the send queue (the TX step sends nothing). */
void transport_send_gate(int ok);
/* Where the TX step sends now (atomic ip:port). */
void transport_dest(uint32_t *ip, uint16_t *port);
/* 1 if ip (and port, 0 = any) is the endpoint or the roam address. */
int  transport_is_own(uint32_t ip, uint16_t port);
int  transport_rx_loop(void *user_buf, uint32_t size);   /* vtTransportRx body */
int  transport_tx_loop(void *user_buf, uint32_t size);   /* vtTransportTx body */
void transport_init(void);        /* ring + event flag, at module start */
void transport_stop(void);
void transport_set_paused(int paused);
/* Rebuild the UDP socket on the next RX step (wake, abandoned suspend). */
void transport_reopen(const char *why);
/* Drop every datagram queued for sending (stale after a sleep). */
void transport_flush(void);
/* One log line with this socket's counters (lent RX thread, routes.c's
 * health line while the tunnel is not up). */
void transport_log_counts(const char *why);
/* The 30 s "perf rx/tx" lines (interface TX thread, with the liveness line). */
void transport_perf_log(void);
/* Wake the lent steps out of their idle waits (tunnel off: they wait up to
 * RX_IDLE_US / TX_IDLE_US on an event flag instead of polling). Sets event
 * flags only: safe from any thread, the giant lock included. */
void transport_wake(void);
/* Restore check: 1 if the UDP socket is open; datagrams queued to send. */
int  transport_socket_open(void);
unsigned transport_queued(void);
/* Our sockets in SceShell's process now open (UDP, route monitor, route). */
extern volatile int g_socks_open;

/* ---- routes (routes.c) --------------------------------------------------- */

/*
 * Routes are reconciled: routes.c keeps a table of what it has applied and
 * computes what should be in place from g_st (want, pause, kill switch,
 * lockdown, allow local, interface, proto state); the difference is
 * written. See routes.c for the rules.
 */
void routes_init(void);           /* lock, at module start (transport_init) */
/* Every lent RX step (a SceShell user context, where PF_ROUTE replies can be
 * read into its buffer ubuf): discovery, the change monitor, additions. */
int  routes_apply(void *ubuf, uint32_t ubuf_size);
/* From any thread context except under the giant lock: apply removals now
 * (disconnect, pause, kill switch off, allow local on). Additions follow on
 * the next RX step. A no-op while suspended. */
void routes_sync(void);
void routes_remove(void);         /* = routes_sync(), kept for older callers */
void routes_mark_dirty(void);     /* retry discovery/additions without waiting */
int  routes_dirty(void);
/* 1 if routes_apply has nothing to do now and will not until a flag set by
 * someone else (tunnel not active, table empty, no change, no monitor, no
 * sweep, no restore check due): the lent RX step may then wait idle. */
int  routes_quiet(void);
void routes_connect(void);        /* vtConnect: forget a stale gateway if nothing is held */
void routes_net_changed(const char *why);  /* rediscover gateway/subnet (resume) */
/* The network was reported ready after a wake (vtNetReady): netctl's address
 * and default gateway (host order, 0 = not known) as hints for the relearn,
 * then a network change. Flags only; call while net_wait is still set. */
void routes_net_ready(uint32_t addr, uint32_t gw);
/* The tunnel's own UDP reached the tunnel interface (the loop signature):
 * the endpoint /32 is missing or broken. Flags only: safe from any thread. */
void routes_ep_suspect(const char *why);
/* vtGetRouteFor: VT_IFACE_* the address goes to with the routes in place. */
int  routes_iface_for(uint32_t ip);
/* vtDisconnect / vtPause: removals now (routes_sync) and the after-snapshot
 * of the restore check (restore.c) in a few seconds. */
void routes_disconnect(int pause);
void iface_hb_counts(uint32_t *kicks, uint32_t *frames);

/* ---- restore check (restore.c; runs inside routes_apply, table owned) ---- */

struct rt_look {                  /* one RTM_GET answer, host order */
	int      ok;                  /* 0 = no route / no reply */
	int32_t  flags;
	uint32_t dst, mask, gw, ifa;
	uint8_t  has_mask, gw_kind, has_ifa;   /* gw_kind: 0 none, 1 IPv4, 2 link */
};
/* RTM_GET (with RTA_IFP|IFA) on socket s; with_mask = netmask given. */
int  routes_lookup(int s, uint32_t dst, int with_mask, uint32_t mask, void *ubuf,
                   uint32_t ubuf_size, struct rt_look *out);
/* Our table now: number of entries and a bitmask of kinds (1 << K_*). */
void routes_table(int *n, unsigned *kinds);
int  routes_monitor_open(void);
/* Request a snapshot: 1 = before connect (now), 2 = after disconnect,
 * 4 = after a pause (both at `due` us uptime), 3 = on demand (the
 * debug_routes file). */
void restore_request(int kind, SceUInt64 due);
/* Called by routes_apply with the table owned: phase 0 at the start of the
 * step (the before-snapshot only), phase 1 at the end; busy = a sweep is
 * still running (the after-snapshot waits). *s is the step's route socket,
 * opened with open_sock when needed. */
void restore_step(int phase, int busy, int (*open_sock)(void), int *s,
                  void *ubuf, uint32_t ubuf_size);
int  restore_pending(void);
/* Timer thread, every ~10 s: the debug_routes flag file. */
void restore_debug_tick(void);
int  restore_debug_on(void);   /* debug_routes exists */

/* ---- latency (latency.c) ------------------------------------------------- */

void latency_init(void);          /* lock, at module start */
void latency_reset(void);         /* a new connection: counts and give-up cleared */
void latency_app_seen(void);      /* the app read the status: ping for ~3 s */
void latency_tick(void);          /* iface TX thread, about once a second or more */
/* Deliver-path hook (lent RX thread): 1 if the decrypted inner packet is the
 * reply to one of our pings and was consumed; 0 to deliver it as usual. */
int  latency_rx_intercept(const uint8_t *ip, uint32_t len);
/* Status figures; hs_rtt_ms is from tnp_get_stats. */
void latency_get(uint32_t hs_rtt_ms, uint32_t *ms, uint8_t *source, uint32_t *loss);

/* ---- timers (module.c) --------------------------------------------------- */

void tn_bridge_state(void);       /* copy proto state/stats into g_st */
/* Something changed (a control call ended, routes marked dirty, a re-arm, a
 * wake): end the idle waits of the timer thread and the lent steps now.
 * Event flags only: safe from any thread, the giant lock included. */
void tn_wake(void);

/*
 * Idle cost: every
 * wakeup of ours, counted where it happens (one writer each, read by the
 * timer thread for the "idle:" line). dns/fb are hook calls (resolver
 * reads, frames), counted even when they return at once.
 */
struct tn_wakes {
	volatile uint32_t rx, tx, timer, iftx, dns, fb;
};
extern struct tn_wakes g_wakes;

/*
 * Latency histograms for the perf lines: fixed buckets in
 * microseconds (upper bounds LH_BOUNDS, the last one open). One writer per
 * histogram; the perf reader keeps its own copy and prints the change since
 * its last line as p50 / p95 (the bucket bound the percentile falls in) and
 * the maximum (taken with an exchange). Plain 32-bit counters: never torn.
 */
#define LH_N 12
struct lhist { uint32_t n[LH_N]; uint32_t max; };
static inline void
lh_add(struct lhist *h, uint32_t us)
{
	/* 25 50 100 200 500 1000 2000 5000 10000 20000 50000 more */
	static const uint32_t b[LH_N - 1] = { 25, 50, 100, 200, 500, 1000, 2000, 5000,
	                                      10000, 20000, 50000 };
	unsigned i = 0;
	while (i < LH_N - 1 && us > b[i])
		i++;
	h->n[i]++;
	if (us > h->max)
		h->max = us;
}
/* One line "perf <what>: n N p50 X p95 Y max Z us" for the change since
 * *old (updated); nothing if no sample. module.c. */
void lh_log(const char *what, struct lhist *cur, struct lhist *old);

/*
 * Watchdog (module.c). Each loop that calls into net_ps or the core marks
 * the start and end of one step, and names the call it is in (a string
 * literal) before each one that could block. The timer thread checks the
 * others every 250 ms and the interface TX thread checks the timer: a step
 * still running after WD_SLOW_US is logged with the call it is in ("stuck"),
 * and so is a step that ends late ("took"); the lent RX thread not stepping
 * at all while the tunnel is wanted is logged too. Lock-free, never blocks.
 */
enum { WD_RX = 0, WD_TX, WD_TIMER, WD_IFTX, WD_N };
#define WD_SLOW_US (2ULL * 1000 * 1000)
void wd_begin(int id);
void wd_phase(int id, const char *phase);   /* a literal: kept by pointer */
void wd_end(int id);
void wd_check(int self);          /* check every loop but `self` */
/* Uptime (us) of the last completed lent RX step, 0 = none yet. */
SceUInt64 wd_last_end(int id);
/* Lock one of our leaf mutexes, watched: a line per second waited, naming
 * `name` and the last thread that took it (*holder, updated here; NULL =
 * not tracked). Unlock with ksceKernelUnlockMutex as usual. */
void tn_mutex_lock(SceUID m, const char *name, volatile SceUID *holder);

/* ---- network readiness after a wake (suspend.c) ----------------------------- */

/* vtNetReady body: VT_NET_* (see vpnturbo.h). */
int  net_ready_report(uint32_t addr, uint32_t gw);
/* The wake work after "network ready" has reached its end (one log line):
 * from the RX step once the socket is rebuilt (tunnel not wanted) or from
 * routes.c once routes are checked and the handshake started. */
void net_ready_work_done(const char *what);
/* control.c: the control mutex (module start) and the timer thread's
 * control work (pause expiry, a deferred attach), under a try of it. */
void control_init(void);
void control_tick(void);

/* ---- control (control.c) ------------------------------------------------- */
/* The syscalls themselves live in control.c and are exported by the yml. */

/* Lockdown. arm: hold tunnel traffic until a handshake newer than now
 * completes (suspend at 0x1000, a Wi-Fi change). update: release it when that
 * happened, or when the tunnel is no longer wanted or is paused. Neither
 * calls net_ps; both are safe from any thread, including the sysevent. */
void tn_lock_arm(const char *why);
void tn_lock_update(void);
/* VT_HOLD_* for the status: why traffic is being held, 0 = not held. */
int  tn_hold_reason(void);
/* 1 if ip (host order) is inside the loaded profile's AllowedIPs, from the
 * module's copy: lock-free (the lent RX step must not wait for the
 * protocol lock). */
int  tn_allowed_cfg(uint32_t ip);
/* Options file (kill switch, allow local, Lockdown): read at module start
 * (and retried from the timer thread while ur0: is not readable yet);
 * written when vtSetOptions changes them. */
void options_load(void);
void options_retry_tick(void);

#endif /* TN_KERN_H */
