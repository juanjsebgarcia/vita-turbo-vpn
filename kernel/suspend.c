/*
 * suspend.c - the power system-event handler
 * and the resume lockdown trigger.
 *
 * Rule from stay out of net_ps from the FIRST suspend event of any kind
 * (resume == 0), because net_ps suspends itself at 0x200, before 0x1000; a
 * thread still touching net_ps during that window hung the console twice.
 * This sets g_st.suspended, which every net_ps user checks (TN_QUIET); it is
 * separate from a user pause (g_st.paused), so a pause that was running
 * before the sleep is still running after it.
 *
 * Leaving the quiet state:
 *   - 0x400000 (resume == 1): the wake. Everything below runs.
 *   - a suspend called off (no 0x1000 seen): if the system sends any
 *     resume-side event, 1 s after the last event; if it sends nothing,
 *     after 60 s without events, and only when every event seen was an early
 *     notice (below 0x200: net_ps has not suspended itself). A device test saw a real
 *     automatic standby come 43 s after its 0x100/0x102 notices, so the old
 *     30 s could end the quiet just before the real suspend. A cycle that
 *     reached 0x200 without 0x1000 waits for a resume-side event, and only
 *     as a last resort ends after 300 s (held tunnel routes would otherwise
 *     keep the console offline for good).
 *   Every event is logged (repeats folded) so the log shows exactly
 *   which sequence a cancelled suspend produces.
 *
 * AFTER A WAKE THE MODULE WAITS FOR THE NETWORK (wake work done at
 * 0x400000 - socket close and reopen, route monitor reopen, route messages,
 * the first handshake send - can leave both lent threads blocked inside
 * net_ps for good, and the console UI with them). 0x400000 only says the
 * system has resumed; the network driver and netctl go on resuming after
 * it. So at 0x400000 g_st.net_wait is set (before suspended is cleared, so
 * TN_QUIET never drops in between) and nothing touches net_ps - no socket
 * call, no route message, no send, no desc[8] - until the SceShell plugin
 * reports through vtNetReady that netctl is connected (an address obtained,
 * on whichever interface netctl manages). The report is taken no earlier
 * than NET_SETTLE_US after the wake. Then, in this order:
 *   - an interface attach that vtConnect deferred meanwhile is done;
 *   - the send ring is emptied (initiations queued before the sleep);
 *   - the UDP transport socket and the route monitor socket are marked for
 *     a rebuild, and routes.c re-learns the gateway, address and
 *     subnet (netctl's address and gateway are passed as hints; a different
 *     gateway is a network change) and checks every route (RTM_GET,
 *     add-if-missing; nothing is deleted unless it points the wrong way);
 *   - routes.c starts a fresh handshake in HANDSHAKING (resent by the timer
 *     every 5 s until answered) once the gateway is known;
 *   - net_wait is cleared last, so the lent RX step sees all of it at once.
 * Lines: "resume: waiting for the network", "network ready after N ms",
 * "wake work done (...)". With no report, a line every 10 s for 5 minutes,
 * then every minute; traffic stays held and the wait goes on (no timeout).
 * If no reporter has ever called vtNetReady (an older plugin), the wait ends
 * by itself NET_NO_REPORTER_US after the wake, as before but later.
 *
 * A suspend called off before net_ps suspended itself (no event >= 0x200)
 * changes nothing on the network side: routes are re-checked and a fresh
 * handshake started, with no wait. One called off after that point, and
 * the 300 s last resort, wait for the network like a wake.
 *
 * Lockdown mode with the tunnel off: nothing is armed; the hold routes stay
 * through the sleep like any route of ours, and after the network-ready
 * report routes.c re-learns the Wi-Fi side and re-checks them (the hold
 * counts as active there), so DHCP and ARP keep working on a new network.
 *
 * At 0x1000 (the suspend is going ahead), if the tunnel is wanted and
 * not paused by the user, the lockdown is armed (tn_lock_arm). Nothing is
 * removed during the sleep: the tunnel routes stay, so after the wake
 * everything still goes into the tunnel interface, which drops it while
 * LOCKED, and only the tunnel's own UDP (endpoint /32 via the gateway)
 * leaves over Wi-Fi. The lock is released by a fresh handshake completing,
 * or by Disconnect or Pause (control.c); there is no timeout.
 *
 * The handler itself never calls net_ps and never takes a lock: it only sets
 * flags (and logs into the lock-free ring). Everything after a wake waits
 * for vtNetReady; the restart after a suspend called off early (come_back:
 * a fresh initiation takes the core's locks) runs on the timer thread. A handler that waited on a lock held
 * by a thread the system had stopped would stall the whole suspend or
 * resume sequence.
 *
 * "Deep" (net_ps has suspended itself) means 0x200 itself, or any event
 * after it in the sequence (0x400 and above). 0x201-0x20F come BEFORE 0x200
 * (the sequence counts down) and are only the foreground app's notices: a
 * cancelled suspend can show 0x20F..0x201 and then nothing, and a test of
 * >= 0x200 would take that for "net_ps suspended", so the quiet would last
 * 300 s instead of 60 s.
 */
#include "tn_kern.h"

#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/threadmgr.h>

#define WAKE_EVENT      0x400000
#define COMMIT_EVENT    0x1000
#define NETPS_EVENT     0x200      /* net_ps suspends itself here */
#define CANCEL_SETTLE_US (1000ULL * 1000)
#define ABANDON_US       (60ULL * 1000 * 1000)
#define DEEP_GIVEUP_US   (300ULL * 1000 * 1000)
#define NET_SETTLE_US      (2ULL * 1000 * 1000)    /* earliest report taken after the wake */
#define NET_NO_REPORTER_US (20ULL * 1000 * 1000)   /* no vtNetReady caller ever: end the wait */

static volatile int g_committed;        /* saw 0x1000: the suspend is going ahead */
static volatile int g_deep;             /* saw an event >= 0x200 this cycle */
static volatile int g_resume_side;      /* saw a resume == 1 event this cycle */
static volatile SceUInt64 g_last_event;

/* Network readiness (see the file comment). */
static volatile int g_reporter;         /* vtNetReady has been called since boot */
static volatile SceUInt64 g_wake_at;    /* when net_wait was set */
static volatile SceUInt64 g_ready_at;   /* when it was released, for the "done" line */
static volatile SceUInt64 g_last_report;
static volatile int g_work_pending;     /* "wake work done" not logged yet */
static volatile int g_releasing;        /* serialises releases: a try-only flag, not
                                         * a mutex (the release may attach the
                                         * interface, a net_ps call) */

static int
is_deep(int eventid)
{
	return eventid == NETPS_EVENT || eventid >= 0x400;
}

/* Log each event, folding repeats (0x401 comes ~50 times, 10 ms apart). */
static void
log_event(int resume, int eventid)
{
	static int last_id = -1, last_res = -1;
	static unsigned repeats, lines;

	if (eventid == last_id && resume == last_res) {
		repeats++;
		return;
	}
	if (repeats)
		klog("sysevent: (x%u more)", repeats + 1);
	repeats = 0;
	last_id = eventid;
	last_res = resume;
	if (lines++ < 400)
		klog("sysevent: %s 0x%08X", resume ? "resume" : "suspend", (unsigned)eventid);
}

/* A suspend called off before net_ps suspended itself: the network was never
 * touched by the system, so no wait. Timer thread (the initiation takes the
 * core's locks); routes are re-checked on the next RX step. */
static void
come_back(const char *why)
{
	(void)why;
	if (g_st.want && !g_st.paused) {
		tnp_trace_timers(12);
		routes_mark_dirty();
		tnp_rehandshake();              /* fresh initiation, resent until answered */
	} else if (TN_LD_HOLD()) {
		routes_mark_dirty();            /* the Lockdown hold is re-checked */
	}
	iface_rearm("suspend called off");
}

/* Begin the wait for the network (flags only: the sysevent handler runs it).
 * net_wait is raised BEFORE suspended drops, so TN_QUIET() stays true. */
static void
net_wait_begin(const char *why)
{
	g_wake_at = ksceKernelGetSystemTimeWide();
	g_st.net_wait = 1;
	__sync_synchronize();
	g_st.suspended = 0;
	klog("%s: waiting for the network before any network call%s", why,
	    g_st.locked ? " (locked until the tunnel is back)" : "");
}

/*
 * The Wi-Fi address went away while the module was using the network (on
 * a device: twice, the route check and the re-handshake that
 * followed an RTM_DELADDR of the Wi-Fi address left both lent threads
 * blocked in net_ps - "udp sendto", "route socket close", "udp recv" - for
 * good, and the console froze with them). The same wait as after a wake:
 * no net_ps call until the SceShell plugin reports netctl connected again,
 * then the same release (socket rebuild, route check, fresh handshake).
 * Lent RX thread (routes.c's change monitor).
 */
void
net_lost(const char *why)
{
	if (g_st.net_wait || g_st.suspended)
		return;
	net_wait_begin(why);
}

static int
sysevent(int resume, int eventid, void *args, void *opt)
{
	(void)args; (void)opt;
	log_event(resume, eventid);
	g_last_event = ksceKernelGetSystemTimeWide();
	if (resume == 0) {
		if (!g_st.suspended) {
			g_st.suspended = 1;
			g_deep = 0;
			g_resume_side = 0;
			klog("suspend: quiet (first event 0x%08X)", (unsigned)eventid);
		}
		if (is_deep(eventid))
			g_deep = 1;
		if (eventid == COMMIT_EVENT && !g_committed) {
			g_committed = 1;
			if (g_st.want && !g_st.paused)
				tn_lock_arm("sleep");
		}
	} else if (eventid == WAKE_EVENT) {
		g_committed = 0;
		net_wait_begin("resume");       /* nothing more until vtNetReady */
	} else if (g_st.suspended) {
		g_resume_side = 1;              /* settles in suspend_tick */
	}
	return 0;
}

/* Release the wait: see the file comment for the order. Runs in the
 * vtNetReady syscall (the SceShell worker) or on the timer thread. */
static int
net_ready_release(const char *who, uint32_t addr, uint32_t gw)
{
	SceUInt64 now = ksceKernelGetSystemTimeWide();

	if (__atomic_exchange_n(&g_releasing, 1, __ATOMIC_ACQUIRE))
		return VT_NET_WAITING;          /* another release is running */
	if (!g_st.net_wait || g_st.suspended) {
		__atomic_store_n(&g_releasing, 0, __ATOMIC_RELEASE);
		return g_st.net_wait ? VT_NET_WAITING : VT_NET_IDLE;
	}
	klog("network ready after %u ms (%s; address %s, gateway %s)",
	    (unsigned)((now - g_wake_at) / 1000), who,
	    addr && addr != VT_NET_ADDR_UNKNOWN ? "known" : "not reported",
	    gw ? "known" : "not reported");
	/* A deferred interface attach follows with no lock held: after
	 * vtNetReady leaves the control mutex, or on the next timer tick. */
	transport_flush();                  /* datagrams queued before the sleep */
	transport_reopen("wake");
	if (g_st.want && !g_st.paused)
		tnp_trace_timers(12);
	routes_net_ready(addr == VT_NET_ADDR_UNKNOWN ? 0 : addr, gw);
	g_ready_at = now;
	g_work_pending = 1;
	__sync_synchronize();
	g_st.net_wait = 0;                  /* last: the RX step sees all of the above */
	/* Link-up again and one dequeue pass (iface_rearm.h): net_ps got no
	 * link-up from us since before the sleep, and with the interface
	 * kept UP across connects no desc[0x10] init gives one either. */
	iface_rearm("wake");
	__atomic_store_n(&g_releasing, 0, __ATOMIC_RELEASE);
	return VT_NET_RELEASED;
}

int
net_ready_report(uint32_t addr, uint32_t gw)
{
	SceUInt64 now = ksceKernelGetSystemTimeWide();

	if (!g_reporter) {
		g_reporter = 1;
		klog("network: SceShell reports network readiness after a wake");
	}
	if (!g_st.net_wait)
		return VT_NET_IDLE;
	g_last_report = now;
	if (addr == 0 || g_st.suspended || now - g_wake_at < NET_SETTLE_US)
		return VT_NET_WAITING;
	return net_ready_release("reported by SceShell", addr, gw);
}

void
net_ready_work_done(const char *what)
{
	if (!g_work_pending)
		return;
	g_work_pending = 0;
	klog("wake work done (%s), %u ms after network ready", what,
	    (unsigned)((ksceKernelGetSystemTimeWide() - g_ready_at) / 1000));
}

/* Timer thread: the wait's progress lines, and the no-reporter fallback. */
static void
net_wait_tick(void)
{
	static SceUInt64 told_for, next_tell;
	SceUInt64 now = ksceKernelGetSystemTimeWide(), waited;

	if (!g_st.net_wait || g_st.suspended)
		return;
	if (told_for != g_wake_at) {
		told_for = g_wake_at;
		next_tell = g_wake_at + 10ULL * 1000 * 1000;
	}
	waited = now - g_wake_at;
	if (!g_reporter && waited >= NET_NO_REPORTER_US) {
		klog("network: no readiness report from SceShell (older plugin?): ending the wait after %u s",
		    (unsigned)(waited / 1000000));
		net_ready_release("no reporter, timed", 0, 0);
		return;
	}
	if (now < next_tell)
		return;
	/* Every 10 s for 5 minutes, then every minute. */
	next_tell = now + (waited < 300ULL * 1000 * 1000 ? 10ULL : 60ULL) * 1000 * 1000;
	klog("network: not ready %u s after the wake (%s, last report %s): %s, still waiting",
	    (unsigned)(waited / 1000000),
	    g_reporter ? "SceShell reports it" : "no reporter yet",
	    g_last_report > g_wake_at ? "recent" : "none since the wake",
	    g_st.locked ? "staying locked, traffic held" :
	    g_st.want ? "tunnel traffic held" : "tunnel off");
}

/* Called from the timer thread (every 250 ms): the network wait's lines, and
 * end a suspend that was called off, by the rules in the file comment. */
void
suspend_tick(void)
{
	SceUInt64 idle;

	net_wait_tick();
	if (!g_st.suspended || g_committed)
		return;
	idle = ksceKernelGetSystemTimeWide() - g_last_event;
	if (g_resume_side && idle > CANCEL_SETTLE_US) {
		if (g_deep) {
			net_wait_begin("suspend called off after net_ps suspended");
		} else {
			g_st.suspended = 0;
			klog("suspend called off (resume events): back");
			come_back("suspend called off");
		}
	} else if (!g_resume_side && !g_deep && idle > ABANDON_US) {
		g_st.suspended = 0;
		klog("suspend abandoned (no event for 60 s, net_ps not suspended): back");
		come_back("suspend abandoned");
	} else if (!g_resume_side && g_deep && idle > DEEP_GIVEUP_US) {
		/* Last resort: with tunnel routes in place a quiet tunnel takes
		 * the whole console offline, so do not stay quiet for ever. */
		klog("suspend: reached 0x200, no 0x1000 and no resume event for 300 s");
		net_wait_begin("suspend abandoned late");
	} else if (!g_resume_side && g_deep && idle > ABANDON_US) {
		static SceUInt64 told;
		if (told != g_last_event) {
			told = g_last_event;
			klog("suspend: reached 0x200 without 0x1000 and no resume event: staying quiet (up to 300 s)");
		}
	}
}

int
suspend_register(void)
{
	return ksceKernelRegisterSysEventHandler("vpnturbo", sysevent, NULL);
}
