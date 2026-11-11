/*
 * module.c - vpnturbo_kernel: module_start/stop, the timer thread and
 * the state file.
 *
 * module_start wires the pieces together and returns quickly; nothing here
 * touches net_ps (the interface attaches on the first vtConnect). The timer
 * thread drives the protocol timers, expires a user pause,
 * un-pauses an abandoned suspend, and persists the monotonic wall-clock
 * floor. The module stays resident once the interface is attached, because
 * net_ps holds pointers into it.
 */
#include "tn_kern.h"
#include "dns.h"       /* DNS override */
#include "overlay.h"   /* padlock + HUD */
#include "strand.h"    /* stranded-connection reset */

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>

#include <string.h>
#include "vt_paths.h"

struct tn_state g_st;

/* From the core (TN_SHIM_VITA): the monotonic wall-clock floor. */
uint64_t tnshim_vita_wall_floor_get(void);
void     tnshim_vita_wall_floor_set(uint64_t ns);

/* From suspend.c. */
int  suspend_register(void);
void suspend_tick(void);

#define STATE_DIR  VT_DATA_DIR
#define STATE_PATH STATE_DIR "/state"

static SceUID g_timer_thread = -1;
static volatile int g_timer_run;

/* ---- state file (last TAI64N floor, so timestamps never go back) --------- */

static void
state_load(void)
{
	uint64_t ns = 0;
	SceUID fd = ksceIoOpen(STATE_PATH, SCE_O_RDONLY, 0);

	if (fd >= 0) {
		if (ksceIoRead(fd, &ns, sizeof(ns)) == (int)sizeof(ns))
			tnshim_vita_wall_floor_set(ns);
		ksceIoClose(fd);
	}
	klog("state: wall floor seeded 0x%08X%08X",
	    (unsigned)(ns >> 32), (unsigned)ns);
}

static void
state_save(void)
{
	static uint64_t saved;
	uint64_t ns = tnshim_vita_wall_floor_get();
	SceUID fd;

	/* Written only when the floor moved (it was rewritten every
	 * 10 s for ever, tunnel or not; the floor only moves with handshakes). */
	if (ns == 0 || ns == saved)
		return;
	saved = ns;
	fd = ksceIoOpen(STATE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	ksceIoWrite(fd, &ns, sizeof(ns));
	ksceIoClose(fd);
}

/* ---- status bridge ------------------------------------------------------- */

void
tn_bridge_state(void)
{
	struct tnp_stats st;

	tnp_get_stats(&st);
	if (g_st.paused) {
		g_st.state = VT_STATE_PAUSED;
		return;
	}
	if (!g_st.want) {
		g_st.state = VT_STATE_IDLE;
		return;
	}
	/* Resume lockdown (control.c/suspend.c/routes.c): LOCKED until a
	 * fresh handshake, whatever the core's own state. */
	tn_lock_update();
	if (g_st.locked) {
		g_st.state = VT_STATE_LOCKED;
		return;
	}
	switch (st.state) {
	case TNP_HANDSHAKING: g_st.state = VT_STATE_HANDSHAKING; break;
	case TNP_UP:          g_st.state = VT_STATE_UP; break;
	case TNP_STALLED:     g_st.state = VT_STATE_STALLED; break;
	case TNP_ERROR:       g_st.state = VT_STATE_ERROR; break;
	default:              g_st.state = VT_STATE_HANDSHAKING; break;
	}
	if (g_st.state == VT_STATE_STALLED && g_st.last_error == VT_ERR_NONE)
		g_st.last_error = VT_ERR_NO_ANSWER;
}

/* ---- watchdog (see tn_kern.h) -------------------------------------------- */

static const char *const k_wd_name[WD_N] = { "lent rx", "lent tx", "timer", "iface tx" };

static struct {
	volatile SceUInt64 start;        /* 0 = not inside a step */
	volatile SceUInt64 last_end;
	const char *volatile phase;
	volatile SceUInt64 told;         /* last "stuck" report for this step */
} g_wd[WD_N];

void
wd_begin(int id)
{
	g_wd[id].phase = "step";
	g_wd[id].start = ksceKernelGetSystemTimeWide();
}

void
wd_phase(int id, const char *phase)
{
	g_wd[id].phase = phase;
}

void
wd_end(int id)
{
	SceUInt64 now = ksceKernelGetSystemTimeWide(), start = g_wd[id].start;

	if (start && now - start > WD_SLOW_US)
		klog("watchdog: %s step took %u ms (last call: %s)", k_wd_name[id],
		    (unsigned)((now - start) / 1000), g_wd[id].phase);
	g_wd[id].start = 0;
	g_wd[id].told = 0;
	g_wd[id].last_end = now;
}

SceUInt64
wd_last_end(int id)
{
	return g_wd[id].last_end;
}

void
wd_check(int self)
{
	static SceUInt64 rx_idle_told;
	SceUInt64 now = ksceKernelGetSystemTimeWide();
	int i;

	for (i = 0; i < WD_N; i++) {
		SceUInt64 start = g_wd[i].start;
		if (i == self || !start || now < start || now - start <= WD_SLOW_US)
			continue;
		/* First report at 2 s, then every 10 s while it lasts. */
		if (g_wd[i].told && now - g_wd[i].told < 10ULL * 1000 * 1000)
			continue;
		g_wd[i].told = now;
		klog("watchdog: %s step stuck for %u ms in %s", k_wd_name[i],
		    (unsigned)((now - start) / 1000), g_wd[i].phase);
	}
	/* Heartbeat: every 5 s while the tunnel is wanted, so a freeze
	 * that stops the log without a watchdog line is still dated to 5 s,
	 * with where each lent thread was ("-" between steps). */
	if (self == WD_TIMER && g_st.want) {
		static SceUInt64 next_hb;
		if (now >= next_hb) {
			next_hb = now + 5ULL * 1000 * 1000;
			static struct tn_wakes hw;
			struct tn_wakes cw = g_wakes;
			uint32_t kicks, frames;
			iface_hb_counts(&kicks, &frames);
			/* Steps of each lent thread and frames SceShell drew in
			 * the last 5 s (a livelock shows as thousands, a stopped UI
			 * as fb 0). */
			klog("hb: state %d wait %d rx %s tx %s | if kicks %u frames %u | 5 s: rx %u tx %u fb %u",
			    (int)g_st.state, g_st.net_wait, g_wd[WD_RX].start ? g_wd[WD_RX].phase : "-",
			    g_wd[WD_TX].start ? g_wd[WD_TX].phase : "-", (unsigned)kicks, (unsigned)frames,
			    (unsigned)(cw.rx - hw.rx), (unsigned)(cw.tx - hw.tx), (unsigned)(cw.fb - hw.fb));
			hw = cw;
		}
	}
	/* The timer (self == WD_TIMER) also watches the lent RX thread
	 * coming back at all: it steps every 2-20 ms when healthy. */
	if (self == WD_TIMER && g_st.want && !TN_QUIET() && g_wd[WD_RX].last_end &&
	    !g_wd[WD_RX].start && now - g_wd[WD_RX].last_end > 5ULL * 1000 * 1000 &&
	    now - rx_idle_told > 30ULL * 1000 * 1000) {
		rx_idle_told = now;
		klog("watchdog: no lent rx step for %u ms (SceShell's thread is not calling)",
		    (unsigned)((now - g_wd[WD_RX].last_end) / 1000));
	}
}

/*
 * Lock-wait watchdog for our leaf mutexes: a try
 * first; then waits of 1 s at a time, one line per second waited, naming
 * the lock, the waiter and the last thread to take it. The control mutex
 * (control.c) and the protocol lock (proto.c) have their own, naming the
 * call that holds them. If the timed wait itself fails (not a timeout) it
 * polls; it never gives the lock up.
 */
#define WAIT_TIMEOUT_ ((int)0x80028005)

void
tn_mutex_lock(SceUID m, const char *name, volatile SceUID *holder)
{
	SceUInt64 t0;
	int r, told = 0;

	if (m < 0)
		return;
	if (ksceKernelTryLockMutex(m, 1) >= 0)
		goto got;
	t0 = ksceKernelGetSystemTimeWide();
	for (;;) {
		SceUInt to = 1000 * 1000;
		r = ksceKernelLockMutex(m, 1, &to);
		if (r >= 0)
			break;
		if (r != WAIT_TIMEOUT_) {
			while (ksceKernelTryLockMutex(m, 1) < 0)
				ksceKernelDelayThread(1000);
			break;
		}
		told++;
		klog("watchdog: thread 0x%08X waited %u ms for the %s lock (last taken by thread 0x%08X)",
		    (unsigned)ksceKernelGetThreadId(), (unsigned)((ksceKernelGetSystemTimeWide() - t0) / 1000),
		    name, holder ? (unsigned)*holder : 0u);
	}
	if (told)
		klog("watchdog: thread 0x%08X got the %s lock after %u ms", (unsigned)ksceKernelGetThreadId(),
		    name, (unsigned)((ksceKernelGetSystemTimeWide() - t0) / 1000));
got:
	if (holder)
		*holder = ksceKernelGetThreadId();
}

/* ---- idle cost and latency histograms ------------------------- */

struct tn_wakes g_wakes;
static SceUID g_timer_evf = -1;
#define TIMER_WAKE 1u

void
tn_wake(void)
{
	transport_wake();
	if (g_timer_evf >= 0)
		ksceKernelSetEventFlag(g_timer_evf, TIMER_WAKE);
}

void
lh_log(const char *what, struct lhist *cur, struct lhist *old)
{
	static const uint32_t b[LH_N - 1] = { 25, 50, 100, 200, 500, 1000, 2000, 5000,
	                                      10000, 20000, 50000 };
	uint32_t d[LH_N], tot = 0, acc = 0, mx;
	unsigned i, p50 = LH_N - 1, p95 = LH_N - 1;
	int got50 = 0;

	for (i = 0; i < LH_N; i++) {
		uint32_t v = cur->n[i];
		d[i] = v - old->n[i];
		old->n[i] = v;
		tot += d[i];
	}
	mx = __atomic_exchange_n(&cur->max, 0, __ATOMIC_RELAXED);
	if (!tot)
		return;
	for (i = 0; i < LH_N; i++) {
		acc += d[i];
		if (!got50 && acc * 2 >= tot) {
			p50 = i;
			got50 = 1;
		}
		if ((uint64_t)acc * 100 >= (uint64_t)tot * 95) {
			p95 = i;
			break;
		}
	}
	klog("perf %s: n %u p50 %s%u p95 %s%u max %u us", what, (unsigned)tot,
	    p50 == LH_N - 1 ? ">" : "<=", (unsigned)b[p50 == LH_N - 1 ? LH_N - 2 : p50],
	    p95 == LH_N - 1 ? ">" : "<=", (unsigned)b[p95 == LH_N - 1 ? LH_N - 2 : p95],
	    (unsigned)mx);
}

/*
 * Idle: nothing for the timer to do at a 250 ms pace (no tunnel wanted, no
 * pause to expire, no Lockdown, no suspend or post-wake wait, no restore
 * check due, no deferred attach). It then ticks once a second; any control
 * call or other change (tn_wake) ends the wait at once, so a connect gets
 * the 250 ms pace (and the state bridge) immediately.
 */
static int
timer_idle(void)
{
	return !g_st.want && !g_st.paused && !g_st.pause_until_s && !g_st.lockdown &&
	    !g_st.suspended && !g_st.net_wait && !restore_pending();
}

/*
 * The "idle:" line: wakeups per second of everything of ours while the
 * tunnel is off, measured over the first minute of an idle stretch, then
 * every 30 minutes (one line; nothing else is written while idle).
 */
static void
idle_report(int idle, SceUInt64 now)
{
	static SceUInt64 since, next;
	static struct tn_wakes o;
	struct tn_wakes c;
	uint32_t s;

	if (!idle) {
		since = 0;
		return;
	}
	if (!since) {
		since = now;
		next = now + 60ULL * 1000 * 1000;
		o = g_wakes;
		return;
	}
	if (now < next)
		return;
	c = g_wakes;
	s = (uint32_t)((now - since) / 1000000ULL);
	if (!s)
		s = 1;
	klog("idle: %u s off: wakeups/s rx %u.%u tx %u.%u timer %u.%u iface %u.%u; dns %u fb %u per s",
	    (unsigned)s,
	    (unsigned)((c.rx - o.rx) / s), (unsigned)((c.rx - o.rx) * 10 / s % 10),
	    (unsigned)((c.tx - o.tx) / s), (unsigned)((c.tx - o.tx) * 10 / s % 10),
	    (unsigned)((c.timer - o.timer) / s), (unsigned)((c.timer - o.timer) * 10 / s % 10),
	    (unsigned)((c.iftx - o.iftx) / s), (unsigned)((c.iftx - o.iftx) * 10 / s % 10),
	    (unsigned)((c.dns - o.dns) / s), (unsigned)((c.fb - o.fb) / s));
	since = now;
	o = c;
	next = now + 30ULL * 60 * 1000 * 1000;
}

/* ---- timer thread -------------------------------------------------------- */

static int
timer_thread(SceSize args, void *argp)
{
	SceUInt64 next_slow = 0;

	(void)args; (void)argp;
	for (;;) {
		int idle = timer_idle();
		SceUInt64 now;
		unsigned int bits = 0;
		SceUInt32 to = idle ? 1000 * 1000 : 250 * 1000;

		if (g_timer_evf >= 0)
			ksceKernelWaitEventFlag(g_timer_evf, TIMER_WAKE,
			    SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &to);
		else
			ksceKernelDelayThread(to);
		if (!g_timer_run)
			break;
		g_wakes.timer++;

		wd_begin(WD_TIMER);
		suspend_tick();

		/* Expire a user pause, retry a deferred attach: under a
		 * try of the control mutex (control.c). */
		control_tick();

		/* Not while suspending: nothing can be sent (the TX step stays
		 * out of net_ps), and a sleep must not count as unanswered
		 * initiations (it would turn the state to stalled). */
		if (g_st.configured && g_st.want && !TN_QUIET()) {
			/* A try first: a busy protocol lock skips this
			 * tick; after four busy ticks in a row (1 s) the tick waits,
			 * watched (proto.c logs each second of waiting). */
			static unsigned busy;
			wd_phase(WD_TIMER, "tnp_timer_tick");
			if (tnp_timer_tick_try() >= 0) {
				busy = 0;
			} else if (++busy >= 4) {
				busy = 0;
				tnp_timer_tick();
			}
		}

		tn_bridge_state();
		dns_refresh("state change");   /* logs a DNS switch at once, not at the next read */
		options_retry_tick();          /* ur0: not readable at module start */
		options_restart_tick();        /* a faster-sending change */

		/* Persist the floor (only if it moved) and look for the
		 * restore check's debug_routes flag file about every 10 s, by
		 * time (the tick is 250 ms or, idle, 1 s). */
		now = ksceKernelGetSystemTimeWide();
		if (now >= next_slow) {
			next_slow = now + 10ULL * 1000 * 1000;
			wd_phase(WD_TIMER, "state file");
			state_save();
			restore_debug_tick();
		}
		idle_report(timer_idle() && !transport_socket_open(), now);
		wd_end(WD_TIMER);
		wd_check(WD_TIMER);
	}
	return ksceKernelExitDeleteThread(0);
}

/* ---- module -------------------------------------------------------------- */

/* A former name's data folder becomes the current one when the current one
 * does not exist yet (vt_paths.h; nothing to do while the list of former
 * names is empty). Runs before the log is opened, so it logs nothing. */
static void
data_dir_migrate(void)
{
	static const char *const old[] = { VT_LEGACY_DATA_DIRS 0 };
	SceIoStat st;

	if (!old[0] || ksceIoGetstat(VT_DATA_DIR, &st) >= 0)
		return;
	for (int i = 0; old[i]; i++)
		if (ksceIoGetstat(old[i], &st) >= 0) {
			ksceIoRename(old[i], VT_DATA_DIR);
			return;
		}
}

void _start() __attribute__((weak, alias("module_start")));

int
module_start(SceSize args, void *argp)
{
	(void)args; (void)argp;

	memset(&g_st, 0, sizeof(g_st));
	g_st.allow_local = 1;          /* local network reachable by default */
	g_st.mtu = DEFAULT_MTU;
	g_st.state = VT_STATE_IDLE;
	g_st.last_error = VT_ERR_NONE;

	data_dir_migrate();   /* before anything is written there */
	klog_init();
	control_init();    /* the control mutex (control.c), before any syscall */
	state_load();
	options_load();    /* kill switch, allow local, Lockdown (control.c) */

	if (tnp_init() != 0) {
		klog("tnp_init failed");
		return SCE_KERNEL_START_SUCCESS;   /* stay loaded; status will show idle */
	}
	tnp_set_sinks(transport_tx_enqueue, iface_rx_deliver, transport_roam, NULL);
	transport_init();
	latency_init();
	dns_init();        /* DNS override hook; logs and carries on if absent */
	overlay_init();    /* padlock + HUD framebuffer hook; same */

	if (iface_start_threads() < 0)
		klog("iface threads failed");

	g_timer_run = 1;
	g_timer_evf = ksceKernelCreateEventFlag("vpnturbo_tmw", 0, 0, NULL);
	g_timer_thread = ksceKernelCreateThread("vpnturbo_timer", timer_thread,
	    0x40, 0x3000, 0, 0, NULL);
	if (g_timer_thread >= 0)
		ksceKernelStartThread(g_timer_thread, 0, NULL);

	suspend_register();
	klog("---- vpnturbo_kernel %s started (API %u)", VT_VERSION_STR, (unsigned)VT_API_VERSION);
	return SCE_KERNEL_START_SUCCESS;
}

int
module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	/* net_ps holds pointers into us once attached; never unload then. */
	if (g_st.attached)
		return SCE_KERNEL_STOP_CANCEL;
	/* The stranded-connection hook is never released: the stack may be
	 * inside it (strand_hook.h). */
	if (strand_installed())
		return SCE_KERNEL_STOP_CANCEL;
	g_timer_run = 0;
	tn_wake();
	overlay_fini();
	dns_fini();
	transport_stop();
	return SCE_KERNEL_STOP_SUCCESS;
}
