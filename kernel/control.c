/*
 * control.c - the SceVpnTurbo syscall library (vpnturbo.h), the
 * caller check, and the two lent-thread entry points.
 *
 * Structures are copied with ksceKernelMemcpyUserToKernel /
 * ...KernelToUser. Keys arrive through vtSetProfile, are handed straight to
 * the protocol core and wiped from this layer's stack copy: they are
 * never stored here and no call returns them.
 *
 * INTERFACE ADDRESS (documented choice): the kernel attaches the
 * interface, sets the MTU and writes routes, but it does NOT set the
 * interface's IPv4 address or IFF_UP. The only proven way to do that on the
 * device is a userland SIOCAIFADDR / SIOCSIFFLAGS, which an early test app
 * demonstrated. So the caller (the app, or the SceShell plugin on a
 * toggle) sets the address from user space using the tunnel_address /
 * tunnel_prefix / ifname this module reports in VtStatus. The kernel-only
 * ifconfig path (calling sceNetSyscallIoctl from inside this syscall) is
 * left as a documented future option; it is not used.
 */
#include "tn_kern.h"
#include <psp2kern/power.h>
#include "dns.h"
#include "overlay.h"
#include "strand.h"

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>
#include "vt_paths.h"

/* Secure wipe the compiler will not drop (keys). */
static void
wipe(void *p, unsigned n)
{
	volatile unsigned char *v = p;
	while (n--)
		*v++ = 0;
}

/* SceSysrootForKernel: resolve the caller's title id. */
int ksceKernelSysrootGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

#define TITLE_APP      "VPNT00001"
#define TITLE_SETTINGS "NPXS10015"
#define TITLE_SHELL    "main"          /* SceShell runs as title id "main" */

enum { CALL_FULL, CALL_LIMITED, CALL_OPEN };

/*
 * The app and SceShell may make every control call; the Settings app
 * may connect/disconnect/pause/resume and read status only. Returns 1 if the
 * current caller may make a call of class `need`. A title id that cannot be
 * read is REFUSED (it used to be allowed, so any process
 * whose title id lookup failed had full control); the log line says so, the
 * first few times, so a device quirk shows up in kernel.log.
 */
static void
tid_unreadable(SceUID pid)
{
	static unsigned told;

	if (told++ < 8)
		klog("caller: title id of pid 0x%08X unreadable: refused", (unsigned)pid);
}

static int
caller_allowed(int need)
{
	char tid[32];
	SceUID pid = ksceKernelGetProcessId();

	if (need == CALL_OPEN)
		return 1;
	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0) {
		tid_unreadable(pid);
		return 0;
	}
	if (strncmp(tid, TITLE_APP, sizeof(tid)) == 0 ||
	    strncmp(tid, TITLE_SHELL, sizeof(tid)) == 0)
		return 1;                       /* full access */
	if (need == CALL_LIMITED && strncmp(tid, TITLE_SETTINGS, sizeof(tid)) == 0)
		return 1;
	klog("caller: '%s' denied (need %d)", tid, need);
	return 0;
}

/* CALL_FULL for the app and SceShell, CALL_LIMITED for Settings, -1 for
 * anyone else (logged), and for an unreadable title id. */
static int
caller_class(void)
{
	char tid[32];
	SceUID pid = ksceKernelGetProcessId();

	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0) {
		tid_unreadable(pid);
		return -1;
	}
	if (strncmp(tid, TITLE_APP, sizeof(tid)) == 0 || strncmp(tid, TITLE_SHELL, sizeof(tid)) == 0)
		return CALL_FULL;
	if (strncmp(tid, TITLE_SETTINGS, sizeof(tid)) == 0)
		return CALL_LIMITED;
	klog("caller: '%s' denied (options)", tid);
	return -1;
}

/* 1 if the caller is the app itself (title id only; no log, no fallback). */
static int
caller_is_app(void)
{
	char tid[32];

	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(ksceKernelGetProcessId(), tid, sizeof(tid)) < 0)
		return 0;
	return strncmp(tid, TITLE_APP, sizeof(tid)) == 0;
}

/* ---- the control mutex --------
 *
 * Every control syscall body (vtSetProfile, vtSetEndpoint, vtConnect,
 * vtDisconnect, vtMigrate, vtSetOptions, vtPause, vtResume, a vtNetReady
 * report) runs under one recursive kernel mutex, so the app, the quick menu
 * and Settings can no longer interleave a connect with a disconnect, a
 * profile load with a migrate. The timer thread only TRIES it (pause
 * expiry, the options retry). It is never taken by a net_ps callback, the
 * lent RX/TX steps, the interface TX thread, the DNS hook or the display
 * hook, and never by vtGetStatus, vtGetDns or vtGetRouteFor.
 *
 * NO net_ps CALL UNDER IT. The interface attach and the route
 * removals of routes_sync (Disconnect, Pause, options) used to run with it
 * held; they now run after it is released (attach_now, the routes_sync /
 * routes_disconnect calls at the end of each syscall). The attach is made
 * single by its own atomic flag (iface.c), routes_sync by the routes
 * ownership flag (try only), so neither needs this mutex.
 *
 * A BOUNDED WAIT THAT DOES NOT DEPEND ON THE KERNEL'S TIMEOUT. A waiter
 * polls with a try every 5 ms (ksceKernelLockMutex's timeout argument is
 * not relied on, and a thread waiting on a kernel mutex can
 * be neither suspended nor killed), so a call never waits more than
 * CTL_WAIT_US: it then gets -VT_ERR_BUSY and the plugin or the app retries.
 * After 1 s of waiting it logs the holder: "watchdog: vtX waited 1000 ms
 * for the control lock (held by vtY, thread T, for N ms)".
 *
 * Lock order: control mutex -> routes ownership flag (try only) -> the
 * protocol lock (proto.c; its gate, then the core's rwlock) -> the core
 * mutex -> leaf mutexes (ring, latency, ARP, DNS). Nothing under it takes it
 * again except through the recursion (vtMigrate -> do_connect, vtNetReady ->
 * net_ready_report).
 */
#define CTL_WAIT_US  (3 * 1000 * 1000)
#define CTL_POLL_US  (5 * 1000)
#define CTL_TELL_US  (1000 * 1000)

static SceUID g_ctl = -1;
static const char *volatile g_ctl_who;   /* the call holding it (depth 0 -> 1) */
static volatile SceUID g_ctl_tid = -1;
static volatile SceUInt64 g_ctl_since;
static unsigned g_ctl_depth;            /* written by the holder only */

void
control_init(void)
{
	g_ctl = ksceKernelCreateMutex("vpnturbo_ctl", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
	if (g_ctl < 0)
		klog("control: mutex 0x%08X (calls unserialised)", (unsigned)g_ctl);
}

static void
ctl_took(const char *who)
{
	if (g_ctl_depth++ == 0) {
		g_ctl_who = who;
		g_ctl_tid = ksceKernelGetThreadId();
		g_ctl_since = ksceKernelGetSystemTimeWide();
	}
}

static int
ctl_enter(const char *who)
{
	SceUInt64 t0;
	int told = 0;

	if (g_ctl < 0)
		return 0;
	if (ksceKernelTryLockMutex(g_ctl, 1) >= 0) {
		ctl_took(who);
		return 0;
	}
	t0 = ksceKernelGetSystemTimeWide();
	for (;;) {
		SceUInt64 now, waited;
		ksceKernelDelayThread(CTL_POLL_US);
		if (ksceKernelTryLockMutex(g_ctl, 1) >= 0)
			break;
		now = ksceKernelGetSystemTimeWide();
		waited = now - t0;
		if (!told && waited >= CTL_TELL_US) {
			const char *h = g_ctl_who;
			SceUInt64 since = g_ctl_since;
			told = 1;
			klog("watchdog: %s (thread 0x%08X) waited %u ms for the control lock (held by %s, thread 0x%08X, for %u ms)",
			    who, (unsigned)ksceKernelGetThreadId(), (unsigned)(waited / 1000),
			    h ? h : "?", (unsigned)g_ctl_tid,
			    since && now > since ? (unsigned)((now - since) / 1000) : 0u);
		}
		if (waited >= CTL_WAIT_US) {
			static unsigned refused;
			if (refused++ < 8)
				klog("control: %s refused after %u ms (busy: %s), retry", who,
				    (unsigned)(waited / 1000), g_ctl_who ? g_ctl_who : "?");
			return -(int)VT_ERR_BUSY;
		}
	}
	if (told)
		klog("watchdog: %s got the control lock after %u ms", who,
		    (unsigned)((ksceKernelGetSystemTimeWide() - t0) / 1000));
	ctl_took(who);
	return 0;
}

static int
ctl_try(const char *who)
{
	if (g_ctl < 0)
		return 1;
	if (ksceKernelTryLockMutex(g_ctl, 1) < 0)
		return 0;
	ctl_took(who);
	return 1;
}

static void
ctl_leave(void)
{
	int last;

	if (g_ctl < 0) {
		tn_wake();
		return;
	}
	last = --g_ctl_depth == 0;
	if (last) {
		g_ctl_who = NULL;
		g_ctl_tid = -1;
		g_ctl_since = 0;
	}
	ksceKernelUnlockMutex(g_ctl, 1);
	/* Whatever the call changed (connect, pause,
	 * options, a profile) is seen by the lent steps and the timer now,
	 * not at the end of their idle wait. Event flags only. */
	if (last)
		tn_wake();
}

/* ---- options file (kill switch, allow local, Lockdown) ------------------- */

#define OPT_DIR    VT_DATA_DIR
#define OPT_PATH   OPT_DIR "/options.txt"
#define OPT_VALVE  OPT_DIR "/lockdown_off"
#define ENOENT_    ((int)0x80010002)

static volatile int g_opt_loaded;        /* read (or found absent): no retry */
static unsigned g_opt_tries;

static void
options_save(void)
{
	char buf[80];
	int n = snprintf(buf, sizeof(buf), "kill_switch=%d\nallow_local=%d\nlockdown=%d\nfast_send=%d\n",
	    g_st.kill_switch, g_st.allow_local, g_st.lockdown, g_st.fast_send);
	SceUID fd;

	ksceIoMkdir(OPT_DIR, 0777);
	fd = ksceIoOpen(OPT_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) {
		klog("options: not saved (0x%08X)", (unsigned)fd);
		return;
	}
	if (n > 0 && n < (int)sizeof(buf))
		ksceIoWrite(fd, buf, (SceSize)n);
	ksceIoClose(fd);
}

static int
key_is(const char *b, int n, const char *k)
{
	int i;
	for (i = 0; k[i]; i++)
		if (i >= n || b[i] != k[i])
			return 0;
	return i == n;
}

/* Module start (no profile, no net_ps yet): just the flags. */
void
options_load(void)
{
	char buf[128];
	SceIoStat st;
	SceUID fd = ksceIoOpen(OPT_PATH, SCE_O_RDONLY, 0);
	int n, i = 0;

	if (fd < 0) {
		g_opt_loaded = fd == ENOENT_;   /* absent: the defaults stand */
		if (!g_opt_loaded && g_opt_tries++ == 0)
			klog("options: not readable yet (0x%08X), retrying", (unsigned)fd);
	} else {
		n = ksceIoRead(fd, buf, sizeof(buf) - 1);
		ksceIoClose(fd);
		g_opt_loaded = 1;
		if (n < 0)
			n = 0;
		buf[n] = 0;
		while (i < n) {
			int ks = i, ke, v = 0;
			while (i < n && buf[i] != '=' && buf[i] != '\n') i++;
			ke = i;
			if (i < n && buf[i] == '=') {
				i++;
				while (i < n && buf[i] >= '0' && buf[i] <= '9' && v < 10)
					v = v * 10 + (buf[i++] - '0');
				if (key_is(buf + ks, ke - ks, "kill_switch")) g_st.kill_switch = v != 0;
				else if (key_is(buf + ks, ke - ks, "allow_local")) g_st.allow_local = v != 0;
				else if (key_is(buf + ks, ke - ks, "lockdown")) g_st.lockdown = v != 0;
				else if (key_is(buf + ks, ke - ks, "fast_send")) g_st.fast_send = v != 0;
			}
			while (i < n && buf[i] != '\n') i++;
			i++;
		}
	}
	if (!g_opt_loaded)
		return;
	/* The safety valve: one boot with Lockdown off, saved, file deleted. */
	if (ksceIoGetstat(OPT_VALVE, &st) >= 0) {
		int r = ksceIoRemove(OPT_VALVE);
		klog("options: %s found: Lockdown turned off%s (remove -> 0x%08X)", "lockdown_off",
		    g_st.lockdown ? "" : " (it was off)", (unsigned)r);
		g_st.lockdown = 0;
		options_save();
	}
	klog("options: kill_switch=%d allow_local=%d lockdown=%d fast_send=%d%s", g_st.kill_switch,
	    g_st.allow_local, g_st.lockdown, g_st.fast_send, fd < 0 ? " (defaults)" : "");
}

/*
 * Faster sending takes effect when the plugin creates its threads, so a
 * change restarts the console: the overlay shows a banner meanwhile
 * (options_restart_pending), the timer thread restarts it.
 */
#define RESTART_DELAY_US (3ULL * 1000 * 1000)
static volatile SceUInt64 g_restart_at;

int
options_restart_pending(void)
{
	return g_restart_at != 0;
}

void
options_restart_tick(void)
{
	if (!g_restart_at || (SceUInt64)ksceKernelGetSystemTimeWide() < g_restart_at)
		return;
	klog("options: restarting now (faster sending changed)");
	ksceKernelDelayThread(200 * 1000);   /* the log writer's next pass */
	kscePowerRequestColdReset();
}

/* Timer thread: retry once a second for a minute while ur0: is not readable. */
void
options_retry_tick(void)
{
	static SceUInt64 next;
	SceUInt64 now = ksceKernelGetSystemTimeWide();

	if (g_opt_loaded || g_opt_tries > 60 || now < next)
		return;
	if (!ctl_try("options retry"))
		return;                         /* a control call is running: next tick */
	next = now + 1000000ULL;
	options_load();
	ctl_leave();
	if (g_opt_loaded && g_st.lockdown)
		routes_mark_dirty();
	else if (!g_opt_loaded && g_opt_tries > 60)
		klog("options: still not readable after a minute: defaults (Lockdown off)");
}

/*
 * The interface attach (a net_ps call) is made with NO lock of ours held:
 * the syscalls decide under the control mutex that one is
 * needed and make it after releasing the mutex (attach_now). iface_attach's
 * own atomic flag keeps it single (a second caller meanwhile gets
 * -VT_ERR_BUSY and the work stays deferred for the timer). While the module
 * stays out of net_ps after a wake it is deferred (g_attach_deferred) and
 * made by vtNetReady or the timer thread once the network is ready.
 */
static volatile int g_attach_deferred;

/* Under the control mutex: 1 if the caller should attach after leaving. */
static int
attach_wanted(void)
{
	if (g_st.attached || !(g_st.want || TN_LD_HOLD()))
		return 0;
	if (TN_NO_NETPS()) {
		g_attach_deferred = 1;
		return 0;
	}
	return 1;
}

/* No lock of ours held. `what` begins the log line ("... -> 0x%08X").
 * Returns iface_attach's result (0 if nothing to do). */
static int
attach_now(const char *why)
{
	int r;

	if (g_st.attached || !(g_st.want || TN_LD_HOLD()))
		return 0;
	if (TN_NO_NETPS()) {
		g_attach_deferred = 1;
		return 0;
	}
	r = iface_attach();
	klog("%s -> 0x%08X", why, (unsigned)r);
	if (r == -(int)VT_ERR_BUSY)
		g_attach_deferred = 1;          /* another attach is running: the timer re-checks */
	else if (r == 0)
		routes_mark_dirty();
	return r;
}

/* ---- helpers ------------------------------------------------------------- */

/*
 * Latency ping target (latency.c): the profile's first DNS server when it is
 * inside AllowedIPs and not our own address, else the .1 of the tunnel's /24
 * when that is a different address inside AllowedIPs, else none. Needs the
 * core's AllowedIPs loaded (tnp_allowed).
 */
static uint32_t
ping_target(const VtProfile *p)
{
	uint32_t a = p->address, gw = (a & 0xFFFFFF00u) | 1u;

	if (p->dns[0] != 0 && p->dns[0] != a && tnp_allowed(p->dns[0]))
		return p->dns[0];
	if (a != 0 && gw != a && tnp_allowed(gw))
		return gw;
	return 0;
}

/*
 * AllowedIPs from the module's own copy (g_st.allowed, written by
 * load_profile under the control mutex), lock-free, for the lent RX step's
 * route work (routes.c, the profile-DNS routes): it must never wait for the
 * protocol lock. A read racing a profile load sees the old or
 * the new list, or a mix for one pass; the next pass corrects it.
 */
int
tn_allowed_cfg(uint32_t ip)
{
	unsigned i, n = g_st.allowed_count;

	if (n > VT_MAX_ALLOWED)
		n = VT_MAX_ALLOWED;
	for (i = 0; i < n; i++) {
		unsigned p = g_st.allowed[i].prefix;
		uint32_t m = p == 0 ? 0 : (p >= 32 ? 0xffffffffu : ~((1u << (32 - p)) - 1));
		if ((ip & m) == (g_st.allowed[i].net & m))
			return 1;
	}
	return 0;
}

static int
full_tunnel(const VtProfile *p)
{
	unsigned i;
	int lo = 0, hi = 0;

	for (i = 0; i < p->allowed_count && i < VT_MAX_ALLOWED; i++) {
		if (p->allowed[i].prefix == 0)
			return 1;                               /* 0.0.0.0/0 */
		if (p->allowed[i].prefix == 1 && p->allowed[i].net == 0x00000000u)
			lo = 1;
		if (p->allowed[i].prefix == 1 && p->allowed[i].net == 0x80000000u)
			hi = 1;
	}
	return lo && hi;
}

static uint32_t
prefix_mask_(unsigned p)
{
	return p == 0 ? 0 : (p >= 32 ? 0xffffffffu : ~((1u << (32 - p)) - 1));
}

/*
 * Kernel-side checks of a VtProfile: the app's and the
 * plugin's parsers already enforce these (conf/), but the module must not
 * trust its callers. NULL = acceptable, else the reason (logged). All of it
 * runs before anything is changed, so a refused profile leaves the loaded
 * one exactly as it was.
 */
static const char *
profile_invalid(const VtProfile *p)
{
	unsigned i;

	if (p->size != sizeof(VtProfile))
		return "size";
	if (p->allowed_count == 0 || p->allowed_count > VT_MAX_ALLOWED)
		return "AllowedIPs count";
	if (p->address_prefix > 32)
		return "address prefix";
	for (i = 0; i < p->allowed_count; i++)
		if (p->allowed[i].prefix > 32)
			return "AllowedIPs prefix";
	if (p->mtu != 0 && (p->mtu < 576 || p->mtu > 1440))
		return "MTU outside 576-1440";
	if (p->endpoint_ip != 0 && p->endpoint_port == 0)
		return "endpoint port";
	for (i = 0; i < VT_NAME_MAX && p->name[i]; i++)
		;
	if (i == VT_NAME_MAX)
		return "name not terminated";
	return NULL;
}

/*
 * Load a profile into the core and the routing fields. migrate: vtMigrate
 * - a profile with the same private key and tunnel address keeps the
 * interface and its address as they are (the shell only re-addresses the
 * interface when tunnel_address changes) and the tunnel routes in place,
 * so only the peer and the endpoint route change.
 *
 * Routes are not taken down here: routes.c remembers what it applied and
 * swaps old for new on its next pass (the new endpoint /32 is added before
 * the old one goes; tunnel routes stay when they are the same), so a migrate
 * or a reconnect with the kill switch on never leaves a gap or a stale route.
 */
static int
load_profile(const VtProfile *up, int migrate)
{
	VtProfile p;
	struct tnp_config cfg;
	unsigned i;
	int r, same = 0, addr_changed = 0;

	if (ksceKernelMemcpyUserToKernel(&p, up, sizeof(p)) < 0)
		return VT_ERR_BAD_PROFILE;
	{
		const char *why = profile_invalid(&p);
		if (why) {
			wipe(&p, sizeof(p));
			klog("profile: rejected (%s); the loaded profile is kept", why);
			return VT_ERR_BAD_PROFILE;
		}
	}
	/* Host bits of AllowedIPs cleared (in the core's copy and ours). */
	for (i = 0; i < p.allowed_count; i++)
		p.allowed[i].net &= prefix_mask_(p.allowed[i].prefix);

	if (g_st.configured) {
		addr_changed = p.address != g_st.tunnel_address;
		same = migrate && !addr_changed && p.address_prefix == g_st.tunnel_prefix &&
		    tnp_same_private(p.private_key);
	}

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.private_key, p.private_key, 32);
	memcpy(cfg.peer_public, p.peer_public, 32);
	memcpy(cfg.preshared, p.preshared, 32);
	cfg.tunnel_address = p.address;
	cfg.keepalive_s = p.keepalive_s;
	cfg.allowed_count = p.allowed_count;
	for (i = 0; i < cfg.allowed_count; i++) {
		cfg.allowed[i].net = p.allowed[i].net;
		cfg.allowed[i].prefix = p.allowed[i].prefix;
	}

	r = tnp_set_config(&cfg);
	wipe(&cfg, sizeof(cfg));  /* wipe the key copy */
	if (r != 0) {
		wipe(&p, sizeof(p));
		if (r == -2) {
			/* The core was cleared before the failure (not expected:
			 * see tnp_set_config): say so rather than keep a profile
			 * the core no longer has. */
			g_st.configured = 0;
			g_st.want = 0;
			klog("profile: core setup failed after the old peer was cleared: no profile loaded");
		} else {
			klog("profile: key refused; the loaded profile is kept");
		}
		return VT_ERR_BAD_PROFILE;
	}

	/* Keep only routing/display fields; keys stay in the core alone. */
	transport_set_endpoint(p.endpoint_ip, p.endpoint_port);   /* drops a roam */
	g_st.listen_port = p.listen_port;
	g_st.tunnel_address = p.address;
	g_st.tunnel_prefix = p.address_prefix;
	g_st.mtu = p.mtu ? p.mtu : DEFAULT_MTU;
	g_st.flags = p.flags;
	g_st.allowed_count = p.allowed_count;
	for (i = 0; i < p.allowed_count; i++) {
		g_st.allowed[i].net = p.allowed[i].net;
		g_st.allowed[i].prefix = p.allowed[i].prefix;
	}
	g_st.full_tunnel = full_tunnel(&p);
	g_st.ping_target = ping_target(&p);
	dns_set_profile(p.dns);           /* the profile's DNS = for dns.c */
	latency_reset();
	memcpy(g_st.name, p.name, VT_NAME_MAX);
	g_st.name[VT_NAME_MAX - 1] = 0;
	g_st.configured = 1;
	g_st.last_error = VT_ERR_NONE;
	if (addr_changed)
		g_st.iface_up = 0;    /* wait for desc[0x10] with the new address */
	if (!same)
		g_st.ever_up = 0;
	if (migrate)
		klog("migrate: %s", same ? "same key and address, interface kept" :
		    addr_changed ? "new address, reconnecting" : "new key, reconnecting");

	wipe(&p, sizeof(p));
	klog("profile loaded (%s tunnel, latency ping %s)",
	    g_st.full_tunnel ? "full" : "split", g_st.ping_target ? "on" : "off");
	return 0;
}

/* ---- syscalls ------------------------------------------------------------ */

int
vtSetProfile(const VtProfile *p)
{
	int r, att = 0;

	if (!caller_allowed(CALL_FULL))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtSetProfile")) < 0)
		return r;
	r = load_profile(p, 0);
	if (r == 0 && g_st.want && !g_st.paused) {
		/* A new profile while connected: the core was reset, so start
		 * its handshake again (routes follow on the next pass). */
		routes_mark_dirty();
		tnp_start_handshake();
		iface_kick_tx();
	}
	if (r == 0)
		att = attach_wanted();      /* Lockdown: at boot, the preload */
	ctl_leave();
	if (att)
		attach_now("lockdown: interface attach (profile loaded)");
	return -r;
}

static int set_endpoint(uint32_t ip, uint32_t port);

int
vtSetEndpoint(uint32_t ip, uint32_t port)
{
	int r;

	if (!caller_allowed(CALL_FULL))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtSetEndpoint")) < 0)
		return r;
	r = set_endpoint(ip, port);
	ctl_leave();
	return r;
}

static int
set_endpoint(uint32_t ip, uint32_t port)
{
	if (ip == 0 && port == VT_EP_LOOKUP_THREAD) {
		/* SceShell's endpoint-lookup thread registers itself (dns.c,
		 * lookup_exempt; vpnturbo.h). SceShell only: the app is covered by
		 * its title id, any other thread of any process is not. */
		static SceUID last_tid = -1;
		char tid[32];
		SceUID pid = ksceKernelGetProcessId(), th = ksceKernelGetThreadId();
		memset(tid, 0, sizeof(tid));
		if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0 ||
		    strncmp(tid, TITLE_SHELL, sizeof(tid)) != 0)
			return -VT_ERR_DENIED;
		if (th != last_tid) {
			last_tid = th;
			dns_set_lookup_thread(pid, th);
		}
		return 0;
	}
	if (!g_st.configured)
		return -VT_ERR_NO_PROFILE;
	if (ip == 0 || port > 0xffff)
		return -VT_ERR_BAD_PROFILE;
	if (ip == g_st.endpoint_ip && (port == 0 || port == g_st.endpoint_port))
		return 0;
	/* routes.c adds the new endpoint /32 and then drops the old one; a
	 * roam is dropped (the address was chosen explicitly). */
	transport_set_endpoint(ip, port ? (uint16_t)port : g_st.endpoint_port);
	klog("endpoint set");
	if (g_st.want && !g_st.paused) {
		routes_mark_dirty();
		tnp_rehandshake();        /* to the new address now, not at the next retry */
		iface_kick_tx();
	}
	return 0;
}

/* vtConnect (or the Lockdown hold) while the module waits for the network
 * after a wake: the interface attach (a net_ps call) is made once the
 * network is reported ready, with no lock held: right after vtNetReady
 * releases the control mutex, or by the timer's control_tick (the
 * no-reporter release). */
static void
deferred_attach_run(void)
{
	int r;

	if (!g_attach_deferred || TN_NO_NETPS())
		return;
	g_attach_deferred = 0;
	r = attach_now("connect: deferred interface attach");
	if (r < 0 && r != -(int)VT_ERR_BUSY)
		g_st.last_error = VT_ERR_SOCKET;
}

/*
 * Timer thread, every 250 ms: control-state work that is not a syscall,
 * under a TRY of the control mutex (skipped this tick if a call holds it):
 * a user pause ending, and an interface attach a wake deferred whose
 * release could not take the mutex.
 */
void
control_tick(void)
{
	overlay_hud_sync();             /* the HUD lines follow the tunnel's state */
	if (g_attach_deferred && !TN_NO_NETPS())
		deferred_attach_run();          /* no lock of ours held */
	if (!g_st.pause_until_s)
		return;
	if (!ctl_try("pause expiry"))
		return;
	if (g_st.pause_until_s) {
		uint32_t now = (uint32_t)(ksceKernelGetSystemTimeWide() / 1000000ULL);
		if (now >= g_st.pause_until_s) {
			g_st.paused = 0;
			g_st.pause_until_s = 0;
			strand_disarm("pause ended");
			if (g_st.want) {
				routes_mark_dirty();
				tnp_start_handshake();
				iface_rearm("pause ended");
			}
			klog("pause expired");
		}
	}
	ctl_leave();
}

/* The attach a connect needs, with no lock held: its error is the call's. */
static int
connect_attach(const char *why)
{
	int r = attach_now(why);

	if (r == -(int)VT_ERR_BUSY)
		return 0;                       /* another attach is running; deferred */
	if (r < 0) {
		g_st.last_error = VT_ERR_SOCKET;
		return r;
	}
	return 0;
}

/* Under the control mutex. *attach: the caller attaches the interface
 * after releasing it (connect_attach). */
/* VT_ERR_NO_SHELL: nobody has lent a thread this boot, a minute or more
 * after boot (the plugin lends them about 15 s in) */
#define SHELL_GRACE_US (60ull * 1000 * 1000)
static int shell_missing(void);
static int shell_lent(void);

static int
do_connect(int *attach)
{
	if (!g_st.configured) {
		g_st.last_error = VT_ERR_NO_PROFILE;
		return -VT_ERR_NO_PROFILE;
	}
	if (!g_st.want && shell_missing()) {
		g_st.last_error = VT_ERR_NO_SHELL;
		klog("connect refused: no SceShell thread lent this boot (plugin inert or not loaded)");
		return -VT_ERR_NO_SHELL;
	}
	if (!g_st.want && !shell_lent()) {
		/* Within the grace minute but no thread lent yet: nothing would
		 * make the socket or the routes, and a connect at
		 * 56 s with the plugin inert (boot guard) would freeze the console.
		 * Busy: the caller retries; after the minute it is
		 * VT_ERR_NO_SHELL above. */
		klog("connect refused for now: no SceShell thread lent yet (%u s after boot)",
		    (unsigned)(ksceKernelGetSystemTimeWide() / 1000000ull));
		return -(int)VT_ERR_BUSY;
	}
	if (!g_st.want) {
		g_st.ever_up = 0;
		restore_request(1, 0);    /* the restore check's before-snapshot */
	}
	if (!g_st.want) {
		transport_set_endpoint(g_st.endpoint_ip, g_st.endpoint_port);   /* a new connection: no roam */
		overlay_exit_ip_reset();
	}
	g_st.want = 1;
	g_st.paused = 0;
	g_st.pause_until_s = 0;
	g_st.lock_drops = 0;
	strand_disarm("connect");         /* the tunnel is coming back: no connection is stranded */
	overlay_hud_sync();               /* a connection starts from idle: the last exit address goes */
	if (!g_st.attached && TN_NO_NETPS()) {
		g_attach_deferred = 1;    /* no net_ps until the network is ready */
		klog("connect: interface attach deferred until the network is ready");
	} else if (!g_st.attached) {
		*attach = 1;              /* after the mutex is released */
	}
	routes_connect();             /* the lent RX thread applies them */
	latency_reset();              /* a new connection: loss count, give-up */
	tnp_start_handshake();
	iface_rearm("connect");       /* no desc[0x10] when the plugin keeps other0 UP */
	return 0;
}

int
vtConnect(void)
{
	int r, att = 0;

	if (!caller_allowed(CALL_LIMITED))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtConnect")) < 0)
		return r;
	r = do_connect(&att);
	if (r == 0)
		klog("connect");
	ctl_leave();
	if (r == 0 && att)
		r = connect_attach("connect: interface attach");
	return r;
}

/*
 * Disconnect: back to native networking,
 * whatever the kill switch says - the kill switch only holds while the
 * tunnel is wanted. In this order, none of it waiting for anything:
 *   - want, the resume lockdown and the pause are cleared, so the DNS override
 *     is off for the very next resolver read (dns_refresh logs it now);
 *   - the protocol is stopped (session keys zeroed; the profile stays);
 *   - the send queue is emptied and the interface's own queue drained (the
 *     TX thread drops what it holds: "off");
 *   - every route of ours is removed now (endpoint /32, gateway /31,
 *     broadcast /32, tunnel routes, subnet halves; or on the next RX step if
 *     it owns the table), then the host entries they cloned are swept;
 *   - the SceShell plugin sees IDLE with no hold and removes the interface
 *     address and takes it down; the transport socket and the route monitor
 *     are closed on the next RX step;
 *   - the restore check compares with the before-connect snapshot ~2-7 s
 *     later ("restore check (after disconnect): ...").
 * With Lockdown on, the routes become the Lockdown hold instead (traffic
 * blocked; VT_HOLD_LOCKDOWN_MODE), and the plugin keeps the address.
 */
int
vtDisconnect(void)
{
	int r;

	if (!caller_allowed(CALL_LIMITED))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtDisconnect")) < 0)
		return r;
	g_st.want = 0;
	g_st.paused = 0;
	g_st.pause_until_s = 0;
	g_st.locked = 0;
	g_st.ever_up = 0;
	g_attach_deferred = 0;
	dns_refresh("disconnect");
	tnp_stop();
	transport_set_endpoint(g_st.endpoint_ip, g_st.endpoint_port);   /* no roam */
	transport_flush();
	latency_reset();
	iface_kick_tx();
	tn_bridge_state();
	overlay_hud_sync();               /* idle: the exit address goes */
	ctl_leave();
	routes_disconnect(0);         /* PF_ROUTE removals: no lock of ours held */
	klog("disconnect: %s", g_st.lockdown ? "Lockdown on: traffic blocked until the next connect" :
	    "tunnel routes removed; the default route is checked next");
	return 0;
}

/*
 * Migrate. Implemented now: the profile is swapped in and the
 * new peer handshaken at once; with the same key and address the interface
 * keeps its address and the tunnel routes stay, so open connections keep
 * their source address and nothing leaks in between (packets are dropped
 * for about one handshake round trip). The new endpoint's /32 is added
 * before the old one is removed. Not yet: keeping the old peer's session
 * carrying traffic until the new handshake completes (a second
 * noise_remote): the fully seamless path.
 */
int
vtMigrate(const VtProfile *p)
{
	int r, att = 0;

	if (!caller_allowed(CALL_FULL))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtMigrate")) < 0)
		return r;
	r = load_profile(p, 1);
	if (r != 0) {
		ctl_leave();
		return -r;
	}
	routes_mark_dirty();
	r = do_connect(&att);
	if (r == 0)
		klog("connect (migrate)");
	ctl_leave();
	if (r == 0 && att)
		r = connect_attach("connect (migrate): interface attach");
	return r;
}

int
vtSetOptions(const VtOptions *o)
{
	VtOptions k;

	int who = caller_class(), ld, fs, changed, att;

	if (who < 0)
		return -VT_ERR_DENIED;
	if (ksceKernelMemcpyUserToKernel(&k, o, sizeof(k)) < 0 || k.size != sizeof(VtOptions))
		return -VT_ERR_BAD_PROFILE;
	{
		int r = ctl_enter("vtSetOptions");
		if (r < 0)
			return r;
	}
	ld = VT_OPT_LOCKDOWN(&k) == VT_LOCKDOWN_ON ? 1 :
	    VT_OPT_LOCKDOWN(&k) == VT_LOCKDOWN_OFF ? 0 : g_st.lockdown;
	fs = VT_OPT_SENDCORE(&k) == VT_SENDCORE_GAME ? 1 :
	    VT_OPT_SENDCORE(&k) == VT_SENDCORE_SYSTEM ? 0 : g_st.fast_send;
	if (who != CALL_FULL) {
		/* Settings: Lockdown and faster sending only. */
		k.kill_switch = g_st.kill_switch;
		k.allow_local = g_st.allow_local;
	}
	changed = (k.kill_switch ? 1 : 0) != g_st.kill_switch ||
	    (k.allow_local ? 1 : 0) != g_st.allow_local || ld != g_st.lockdown ||
	    fs != g_st.fast_send;
	if (fs != g_st.fast_send) {
		klog("options: faster sending %s; restarting in 3 s to apply it",
		    fs ? "on (TX thread on the game cores)" : "off");
		g_restart_at = ksceKernelGetSystemTimeWide() + RESTART_DELAY_US;
	}
	g_st.fast_send = (uint8_t)fs;
	g_st.kill_switch = k.kill_switch ? 1 : 0;
	g_st.allow_local = k.allow_local ? 1 : 0;
	if (ld && !g_st.lockdown && g_st.paused) {
		/* Lockdown refuses a pause: one running ends now. */
		g_st.paused = 0;
		g_st.pause_until_s = 0;
		if (g_st.want) {
			tnp_start_handshake();
			iface_rearm("resume");
		}
		klog("lockdown: the pause ends");
	}
	if (ld != g_st.lockdown)
		klog("lockdown: %s%s", ld ? "on" : "off",
		    !ld && !g_st.want ? " (tunnel off: normal networking restored)" : "");
	g_st.lockdown = (uint8_t)ld;
	klog("options: kill_switch=%d allow_local=%d lockdown=%d", g_st.kill_switch,
	    g_st.allow_local, g_st.lockdown);
	if (changed)
		options_save();
	/* Removals now (Lockdown off while disconnected releases the hold;
	 * allow local on takes the LAN halves down); additions (Lockdown on,
	 * allow local off) on the next lent RX step. The kill switch alone
	 * changes nothing while disconnected. */
	att = attach_wanted();
	dns_refresh("options");
	ctl_leave();
	routes_sync();                /* PF_ROUTE removals: no lock of ours held */
	if (att)
		attach_now("lockdown: interface attach (Lockdown on)");
	return 0;
}

int
vtPause(uint32_t seconds)
{
	int r;

	if (!caller_allowed(CALL_LIMITED))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtPause")) < 0)
		return r;
	if (g_st.lockdown) {
		ctl_leave();
		/* Lockdown: only tunnel traffic, ever; a pause would let
		 * everything out for minutes. Turning Lockdown off is the
		 * explicit way. */
		klog("pause refused: Lockdown is on");
		return -(int)VT_ERR_LOCKDOWN;
	}
	g_st.paused = 1;
	g_st.pause_until_s = (uint32_t)(ksceKernelGetSystemTimeWide() / 1000000ULL) +
	    (seconds ? seconds : 300);
	/* "Tunnel and kill switch off": every route goes, kill switch or
	 * not, and a pause also releases the resume lockdown; the DNS override is
	 * off at once; the plugin removes the interface address while paused.
	 * Only the session (and its socket) is kept for the resume. */
	g_st.locked = 0;
	dns_refresh("pause");
	transport_flush();
	tn_bridge_state();
	klog("pause %u s", (unsigned)(seconds ? seconds : 300));
	ctl_leave();
	routes_disconnect(1);         /* PF_ROUTE removals: no lock of ours held */
	klog("pause: returning");
	return 0;
}

int
vtResume(void)
{
	int r;

	if (!caller_allowed(CALL_LIMITED))
		return -VT_ERR_DENIED;
	if ((r = ctl_enter("vtResume")) < 0)
		return r;
	g_st.paused = 0;
	g_st.pause_until_s = 0;
	strand_disarm("resume");
	if (g_st.want) {
		routes_mark_dirty();
		tnp_start_handshake();
		iface_rearm("resume");
	}
	klog("resume");
	ctl_leave();
	return 0;
}

/*
 * After a wake: the SceShell plugin reports netctl's connection (see vpnturbo.h
 * and suspend.c). Never calls net_ps itself and never waits; the release
 * only sets flags (and, rarely, does a deferred interface attach).
 */
int
vtNetReady(uint32_t address, uint32_t gateway)
{
	/* The query (once a second) is open, so it never logs; a report is
	 * the app's or SceShell's only. */
	int r;

	if (!caller_allowed(address == 0 ? CALL_OPEN : CALL_FULL))
		return -VT_ERR_DENIED;
	if (address == 0)
		return net_ready_report(0, 0);  /* a query: flags only, no mutex */
	if ((r = ctl_enter("vtNetReady")) < 0)
		return r;
	r = net_ready_report(address, gateway);
	ctl_leave();
	deferred_attach_run();        /* a connect made during the wait: no lock held */
	return r;
}

/* ---- protection state ---------------------------------------- */

void
tn_lock_arm(const char *why)
{
	struct tnp_stats st;

	if (g_st.locked)
		return;
	tnp_get_stats(&st);
	g_st.lock_hs = st.handshakes;
	g_st.locked = 1;
	klog("lockdown: on (%s)", why);
}

void
tn_lock_update(void)
{
	struct tnp_stats st;

	if (!g_st.locked)
		return;
	if (!g_st.want || g_st.paused) {
		g_st.locked = 0;
		klog("lockdown: off (%s)", g_st.paused ? "paused" : "disconnected");
		return;
	}
	tnp_get_stats(&st);
	if (st.state == TNP_UP && st.handshakes != g_st.lock_hs) {
		g_st.locked = 0;
		klog("lockdown: off (fresh handshake)");
	}
}

int
tn_hold_reason(void)
{
	struct tnp_stats st;

	if (g_st.paused || !g_st.configured)
		return VT_HOLD_NONE;
	if (!g_st.want)                 /* Disconnect holds nothing */
		return g_st.lockdown ? (int)VT_HOLD_LOCKDOWN_MODE : VT_HOLD_NONE;
	if (g_st.locked)
		return VT_HOLD_LOCKDOWN;
	if (!TN_KILL() || !(g_st.route_flags & (VT_RT_TUNNEL | VT_RT_BLOCK)))
		return VT_HOLD_NONE;
	tnp_get_stats(&st);
	return st.state != TNP_UP ? VT_HOLD_KILL_SWITCH : VT_HOLD_NONE;
}

int
vtGetStatus(VtStatus *s)
{
	VtStatus k;
	struct tnp_stats st;
	uint32_t usize = 0, n;

	/* Fill only as much as the caller has room for: an API 2 client passes
	 * 120 bytes, reads api_version and knows the module is newer. */
	if (ksceKernelMemcpyUserToKernel(&usize, s, sizeof(usize)) < 0)
		return -VT_ERR_BAD_PROFILE;
	if (usize < VT_STATUS_SIZE_V2)
		return -VT_ERR_BAD_PROFILE;
	n = usize < sizeof(k) ? usize : (uint32_t)sizeof(k);

	tn_lock_update();
	tn_bridge_state();
	tnp_get_stats(&st);
	if (caller_is_app())
		latency_app_seen();       /* the app is showing it: ping (latency.c) */

	memset(&k, 0, sizeof(k));
	k.size = n;
	k.api_version = VT_API_VERSION;
	k.state = (uint32_t)g_st.state;
	k.last_error = (uint32_t)g_st.last_error;
	k.rx_bytes = st.rx_bytes;
	k.tx_bytes = st.tx_bytes;
	k.rx_packets = st.rx_packets;
	k.tx_packets = st.tx_packets;
	k.handshake_age_s = st.handshake_age_s;
	if (g_st.paused && g_st.pause_until_s) {
		uint32_t now = (uint32_t)(ksceKernelGetSystemTimeWide() / 1000000ULL);
		k.pause_left_s = g_st.pause_until_s > now ? g_st.pause_until_s - now : 0;
	}
	k.endpoint_ip = g_st.endpoint_ip;
	k.endpoint_port = g_st.endpoint_port;
	k.kill_switch = g_st.kill_switch;
	k.allow_local = g_st.allow_local;
	k.tunnel_address = g_st.tunnel_address;
	k.tunnel_prefix = g_st.tunnel_prefix;
	k.wlan_prefix = g_st.wlan_prefix;
	k.mtu = g_st.mtu;
	k.wlan_address = g_st.wlan_address;
	k.wlan_gateway = g_st.wlan_gateway;
	memcpy(k.profile_name, g_st.name, VT_NAME_MAX);
	latency_get(st.handshake_rtt_ms, &k.latency_ms, &k.latency_source, &k.ping_loss);
	k.lock_drops = g_st.lock_drops;
	k.hold = (uint8_t)tn_hold_reason();
	k.route_flags = g_st.route_flags;
	VT_ST_LOCKDOWN(&k) = g_st.lockdown;
	VT_ST_SENDCORE(&k) = g_st.fast_send;
	VT_ST_FEATURES(&k) = (uint16_t)(VT_FEAT_LOCKDOWN | VT_FEAT_RESTORE |
	    VT_FEAT_BLOCK_ROUTES | VT_FEAT_NOTES | VT_FEAT_NO_SHELL | VT_FEAT_SENDCORE);
	if (shell_missing())
		VT_ST_NOTES(&k) |= VT_NOTE_NO_SHELL;
	/* A hostname endpoint with no address yet
	 * under Lockdown; the UI explains it (vpnturbo.h). */
	if (g_st.configured && g_st.lockdown && g_st.endpoint_ip == 0)
		VT_ST_NOTES(&k) |= VT_NOTE_EP_UNRESOLVED;
	return ksceKernelMemcpyKernelToUser(s, &k, n);
}

int
vtGetRouteFor(uint32_t ip, int *iface)
{
	/* From the routes actually in place (routes.c), including held ones. */
	int v = routes_iface_for(ip);

	return ksceKernelMemcpyKernelToUser(iface, &v, sizeof(v));
}

/* ---- lent threads -------------------------------------------------- */

/*
 * Who may lend. Without a check any process could make
 * these calls: close and remake the tunnel's UDP socket in its own process,
 * take over the PF_ROUTE monitor and route sockets (and the replies read
 * back through its buffer), and race the static buffers the loops share
 * (transport.c g_rx_kernel and out[], proto.c g_rx_scratch). So:
 *
 *   - only SceShell (title id "main"; an unreadable title id is refused
 *     here, unlike the control calls: nothing else is expected to lend);
 *   - the first SceShell process accepted is pinned, every other pid is
 *     refused while it exists. SceShell does not restart without a reboot
 *     (a SceShell crash restarts the console), so the pin is in practice
 *     for the whole boot; it moves only if the pinned pid is gone (its
 *     title id no longer reads) and a new "main" process calls;
 *   - one caller per direction at a time (an atomic in-use flag); a second
 *     gets -VT_ERR_BUSY and the plugin retries after its usual pause.
 *
 * The pinned pid is compared first, so a step costs no title-id lookup.
 */
static volatile SceUID g_lend_pid = -1;

static int
shell_missing(void)
{
	return g_lend_pid < 0 && (SceUInt64)ksceKernelGetSystemTimeWide() > SHELL_GRACE_US;
}

static int shell_lent(void) { return g_lend_pid >= 0; }
static volatile int g_lend_busy[2];

static void
lend_refused(const char *why, SceUID pid)
{
	static unsigned told;

	if (told++ < 8)
		klog("transport: lend refused for pid 0x%08X (%s)", (unsigned)pid, why);
}

static int
lend_enter(int dir)
{
	SceUID pid = ksceKernelGetProcessId();

	if (pid != g_lend_pid) {
		char tid[32];
		SceUID old = g_lend_pid;

		memset(tid, 0, sizeof(tid));
		if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0) {
			lend_refused("title id unreadable", pid);
			return -VT_ERR_DENIED;
		}
		if (strncmp(tid, TITLE_SHELL, sizeof(tid)) != 0) {
			lend_refused("not SceShell", pid);
			return -VT_ERR_DENIED;
		}
		if (old >= 0) {
			char otid[32];
			memset(otid, 0, sizeof(otid));
			if (ksceKernelSysrootGetProcessTitleId(old, otid, sizeof(otid)) >= 0) {
				lend_refused("another SceShell process is pinned", pid);
				return -VT_ERR_DENIED;
			}
		}
		if (!__atomic_compare_exchange_n(&g_lend_pid, &old, pid, 0,
		    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			if (g_lend_pid != pid) {
				lend_refused("pinned meanwhile", pid);
				return -VT_ERR_DENIED;
			}
		} else {
			klog("transport: lent threads pinned to SceShell pid 0x%08X%s",
			    (unsigned)pid, old >= 0 ? " (the pinned one is gone)" : "");
		}
	}
	if (__atomic_exchange_n(&g_lend_busy[dir], 1, __ATOMIC_ACQUIRE)) {
		lend_refused(dir ? "a tx step is already running" : "an rx step is already running", pid);
		return -(int)VT_ERR_BUSY;
	}
	return 0;
}

static void
lend_leave(int dir)
{
	__atomic_store_n(&g_lend_busy[dir], 0, __ATOMIC_RELEASE);
}

/*
 * THE LENT STEPS RUN ON A STACK OF THEIR OWN (see
 * the Lockdown connect crash). A
 * syscall runs on the calling thread's kernel stack, which is small for a
 * user thread; the RX step's route work (routes_apply -> the periodic check
 * and the re-assertion -> RTM_GET -> net_ps's own sendto/recvfrom, with the
 * log formatting on top) needed more than that, and the first step after a
 * connect under the Lockdown hold took exactly that path. Sony's own
 * syscalls do the same for deep work (ksceIoOpen runs its worker with
 * ksceKernelRunWithStack(0x2000)). LENT_STACK leaves room for net_ps's
 * depth under ours. If the call cannot run (it never ran the function),
 * the step runs on the caller's stack as before, logged once.
 */
#define LENT_STACK 0x4000

struct lent_call { void *buf; uint32_t size; int r; volatile int ran; int dir; };

static int
lent_body(void *arg)
{
	struct lent_call *c = arg;

	c->ran = 1;
	c->r = c->dir ? transport_tx_loop(c->buf, c->size) : transport_rx_loop(c->buf, c->size);
	return 0;
}

static int
lent_run(int dir, void *buf, uint32_t size)
{
	static int told[2];
	struct lent_call c;
	int k;

	c.buf = buf;
	c.size = size;
	c.r = 0;
	c.ran = 0;
	c.dir = dir;
	k = ksceKernelRunWithStack(LENT_STACK, lent_body, &c);
	if (!c.ran) {
		if (!told[dir]++)
			klog("transport: lent %s step on its own stack refused (0x%08X): on the caller's stack",
			    dir ? "tx" : "rx", (unsigned)k);
		lent_body(&c);
	} else if (!told[dir]++) {
		klog("transport: lent %s steps run on a %u KB stack of their own", dir ? "tx" : "rx",
		    (unsigned)(LENT_STACK / 1024));
	}
	return c.r;
}

int
vtTransportRx(void *buf, uint32_t size)
{
	int r = lend_enter(0);

	if (r < 0)
		return r;
	/* Runs on a SceShell thread; the socket it creates is SceShell-owned. */
	r = lent_run(0, buf, size);
	lend_leave(0);
	return r;
}

int
vtTransportTx(void *buf, uint32_t size)
{
	int r = lend_enter(1);

	if (r < 0)
		return r;
	r = lent_run(1, buf, size);
	lend_leave(1);
	return r;
}
