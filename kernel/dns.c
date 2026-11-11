/*
 * dns.c - the profile's DNS servers while the tunnel is up.
 *
 * net_ps keeps one global 0x28-byte DNS record (word 0 primary, word 1
 * secondary, network byte order; word 2 flags). The system's network
 * settings write it with sceNetSyscallControl(-1, 0x20000002, rec, 0x28)
 * after DHCP or a static setup and zero it on teardown. Every process's
 * resolver reads it at the start of each query with
 * sceNetSyscallControl(-1, 2, rec, 0x28) and, when word 2 bit 0 is set,
 * empties its own answer cache first.
 *
 * The hook sits on that one syscall export (SceNetPsForSyscalls 0x2CBED2C6,
 * NID 0xEA0C1B71; exported by net_ps on every firmware 3.60-3.74, and the
 * record's get and set behave the same on all of them). It changes only what a READ returns, after the original
 * has run:
 *   - override on (tunnel UP, wanted, not paused, full tunnel with its
 *     routes in place, profile DNS set): words 0/1 become the profile's
 *     servers;
 *   - traffic held or routed into the tunnel while it is not up (see
 *     wanted_mode): the profile's servers if they are
 *     routed into the tunnel, else none (0), so no plaintext query reaches
 *     a server outside it. The one exception is the endpoint lookup
 *     (lookup_exempt): the app and SceShell's endpoint-lookup thread still
 *     get the system's servers then;
 *   - LOCKED: words 0/1 become 0, so no plaintext query is sent,
 *     endpoint lookups included;
 *   - otherwise the system's record passes through untouched.
 * Writes pass through to net_ps unchanged (the system's servers are "kept
 * aside" in net_ps's own record) and are only noted for vtGetDns and the
 * log. So there is nothing to restore when the tunnel goes down: the next
 * read simply returns the system's servers again (the mode is re-derived
 * from want/paused on every read, so Disconnect and Pause take effect on
 * the very next query; dns_refresh logs the switch at once and moves the
 * flush generation on, instead of waiting for the next read to notice it,
 * which was seen to log it ~236 s late). Each time the effective
 * pair changes, a generation number moves on and every process gets the
 * flush bit once on its next read, so no resolver keeps answers from the
 * other servers.
 *
 * Safety: if the export cannot be hooked the module logs it and runs
 * without the override. The hook never touches net_ps state, holds no
 * net_ps lock (it runs before and after the original, outside net_ps's own
 * lock), copies at most 12 bytes to or from the caller's buffer with the
 * checked user-copy calls, and ignores any call that is not exactly a
 * global (socket < 0) DNS get or set with a buffer of at least 0x28 bytes.
 * It never WAITS for anything: every process's resolver passes through it
 * (SceShell's among them), so its own lock is only tried. If another
 * thread holds it (a few instructions' worth), a read still gets the right
 * servers, only its flush bookkeeping waits for the next read; a write is
 * still passed to net_ps and only its note for vtGetDns is skipped.
 */
#include "tn_kern.h"
#include "dns.h"

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/processmgr.h>
#include <taihen.h>

#include <string.h>

/* SceSysrootForKernel: a process's title id (as control.c). */
int ksceKernelSysrootGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

#define NETPS_SYSCALLS_LIB  0x2CBED2C6u   /* SceNetPsForSyscalls */
#define NID_SYSCALL_CONTROL 0xEA0C1B71u   /* sceNetSyscallControl */

#define CTL_DNS_GET  2
#define CTL_DNS_SET  0x20000002
#define DNS_REC_LEN  0x28
#define DNS_FLUSH    1u                   /* word 2 bit 0: empty the resolver cache */

#define SEEN_SLOTS   32

static SceUID g_hook = -1;
/* taiHEN makes the hook live a moment before it stores the ref, and calls
 * keep coming from other threads meanwhile: wait for the ref (bounded; a
 * single load once it is set) and never continue through 0. */
static volatile tai_hook_ref_t g_ref;
#define HOOK_NO_REF  ((int)0x80010005)

static tai_hook_ref_t ref_wait(void)
{
	for (int i = 0; i < 2000 && g_ref == 0; i++)
		ksceKernelDelayThread(10);   /* at most ~20 ms, only in the gap */
	return g_ref;
}
static SceUID g_lock = -1;

static volatile uint32_t g_prof[2];      /* profile DNS, host order */

/* SceShell's endpoint-lookup thread (dns_set_lookup_thread), -1 = none. */
static volatile SceUID g_lk_pid = -1, g_lk_tid = -1;

/* Under g_lock. */
static uint32_t g_sys[2];                /* last system write, host order */
static int      g_sys_seen;
static int      g_mode;                  /* MODE_*: what reads are given now */
static uint32_t g_eff[2];                /* the override pair (host order) when g_mode != MODE_SYSTEM */
static uint32_t g_gen;                   /* bumped at every change of the effective pair */
static struct { SceUID pid; uint32_t gen; } g_seen[SEEN_SLOTS];
static unsigned g_seen_next;

enum { MODE_SYSTEM = 0, MODE_PROFILE, MODE_BLOCKED };

static uint32_t bswap(uint32_t v) { return __builtin_bswap32(v); }

static int  trylock(void) { return g_lock >= 0 && ksceKernelTryLockMutex(g_lock, 1) >= 0; }
static void unlock(void)  { ksceKernelUnlockMutex(g_lock, 1); }

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

/*
 * What a read should return now. Reads only aligned words of g_st.
 * *exempt is set when the endpoint lookup may still be given the system's
 * servers (lookup_exempt): traffic held by the kill switch or Lockdown
 * mode, or routed into a tunnel that is re-handshaking - never while
 * LOCKED.
 *
 * The rules (earlier, everything but LOCKED and UP
 * got the system's servers, which are usually on the LAN and so, with
 * allow local on (the default), reached directly in plaintext while all
 * other traffic was held):
 *   - paused, no profile, or disconnected without Lockdown: the system's
 *     record (as if the module were not there);
 *   - LOCKED: none, for everyone;
 *   - Lockdown mode with the tunnel off, once its block is in place
 *     (VT_RT_TUNNEL): none (the tunnel is not coming back by itself, so
 *     the profile's servers could only time out);
 *   - UP: the profile's servers with a full tunnel whose routes are in
 *     place and DNS = set (as before); a profile without DNS = leaves DNS
 *     unmanaged (the system's servers, as the reference setup script without DNS =), and so
 *     does a split tunnel;
 *   - not UP but the tunnel routes still in place with a full tunnel (or
 *     Lockdown) and traffic held (tn_hold_reason: connecting, stalled or
 *     in error with the kill switch on): the profile's servers if every
 *     one of them is routed into the tunnel (where the query waits or is
 *     dropped, never leaks), otherwise none;
 *   - the same routes, kill switch off, re-handshaking after a drop: the
 *     profile's servers on the same condition, otherwise the system's
 *     (unmanaged, as while UP);
 *   - otherwise (connecting before the routes exist, a drop with the kill
 *     switch off, which falls back to direct access by design, or a
 *     split tunnel): the system's servers.
 * The routes condition (route_flags) also keeps the profile's servers from
 * being handed out before they are reachable: UP is reported a step before
 * the routes are added, and a server that cannot be reached makes every
 * resolver in every process - SceShell's among them - wait out its whole
 * timeout.
 */
static int tunnelled(uint32_t ip)
{
	return ip == 0 || routes_iface_for(ip) == VT_IFACE_TUNNEL;
}

static int wanted_mode(uint32_t out[2], int *exempt)
{
	int state = g_st.state, tun, blanket, prof;

	out[0] = out[1] = 0;
	*exempt = 0;
	if (g_st.paused || !g_st.configured)
		return MODE_SYSTEM;
	/* The tunnel routes, or the reject routes standing in for them before
	 * the interface is up (routes.c, THE BLOCK): held either way. */
	tun = (g_st.route_flags & (VT_RT_TUNNEL | VT_RT_BLOCK)) != 0;
	blanket = (g_st.full_tunnel && !(g_st.flags & VT_PROFILE_NO_ROUTES)) || g_st.lockdown;
	if (!g_st.want) {
		/* Not wanted, whatever g_st.state still says (the timer bridges
		 * it up to 250 ms later). */
		if (g_st.lockdown && tun) {
			*exempt = 1;
			return MODE_BLOCKED;
		}
		return MODE_SYSTEM;
	}
	if (state == VT_STATE_LOCKED || g_st.locked)
		return MODE_BLOCKED;                 /* no DNS at all while locked */
	if (!tun || !blanket)
		return MODE_SYSTEM;
	if (state == VT_STATE_UP) {
		if (!g_prof[0] || !g_st.full_tunnel || (g_st.flags & VT_PROFILE_NO_ROUTES))
			return MODE_SYSTEM;              /* DNS unmanaged (or a split tunnel) */
		out[0] = g_prof[0];
		out[1] = g_prof[1];
		return MODE_PROFILE;
	}
	/* Not up, the tunnel routes in place. */
	prof = g_prof[0] && tunnelled(g_prof[0]) && tunnelled(g_prof[1]);
	if (prof) {
		out[0] = g_prof[0];
		out[1] = g_prof[1];
	}
	if (tn_hold_reason() != VT_HOLD_NONE) {
		*exempt = 1;                         /* held: kill switch (or Lockdown) */
		return prof ? MODE_PROFILE : MODE_BLOCKED;
	}
	/* Re-handshaking with the kill switch off: a managed profile's servers
	 * follow the routes into the tunnel; an unmanaged one keeps the
	 * system's, as while UP. */
	if (prof) {
		*exempt = 1;
		return MODE_PROFILE;
	}
	return MODE_SYSTEM;
}

/*
 * The endpoint lookup. A hostname endpoint is looked
 * up by the app (before vtSetProfile) and by SceShell's endpoint-lookup
 * thread (shell/src/resolve.c, which registers itself with
 * vtSetEndpoint(0, VT_EP_LOOKUP_THREAD)). While traffic is held those two
 * still get the system's servers, so a tunnel can start or follow a
 * changed address; every other process, and every other SceShell thread,
 * gets what wanted_mode says. Never while LOCKED (the remembered
 * address is retried instead), and nothing is changed in the routes: the
 * query leaves only if the system's server is reachable while held (a
 * LAN resolver with allow local on, or the gateway itself; otherwise the
 * hold drops it and the endpoint cache is what lets the tunnel start).
 * What this can leak: the names those two callers look up (the endpoint's,
 * and the app's own service names), to the network's resolver. A route
 * for the system's server out of the hold was considered and not made: it
 * would let every process's traffic to that address out, on any port.
 */
static int lookup_exempt(SceUID pid)
{
	char tid[32];

	if (pid == g_lk_pid && ksceKernelGetThreadId() == g_lk_tid)
		return 1;
	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0)
		return 0;
	return strncmp(tid, "VPNT00001", sizeof(tid)) == 0;
}

void
dns_set_lookup_thread(SceUID pid, SceUID tid)
{
	g_lk_tid = -1;
	g_lk_pid = pid;
	g_lk_tid = tid;
	klog("dns: endpoint lookups from thread 0x%08X (pid 0x%08X) keep the system's servers while held",
	    (unsigned)tid, (unsigned)pid);
}

void
dns_profile(uint32_t out[2])
{
	out[0] = g_prof[0];
	out[1] = g_prof[1];
}

/* Under g_lock: 1 if this process should flush its cache on this read. */
static int needs_flush(SceUID pid)
{
	unsigned i;

	if (g_gen == 0)
		return 0;                            /* nothing has changed since boot */
	for (i = 0; i < SEEN_SLOTS; i++) {
		if (g_seen[i].pid == pid) {
			if (g_seen[i].gen == g_gen)
				return 0;
			g_seen[i].gen = g_gen;
			return 1;
		}
	}
	i = g_seen_next++ % SEEN_SLOTS;          /* new or evicted: flush to be sure */
	g_seen[i].pid = pid;
	g_seen[i].gen = g_gen;
	return 1;
}

/* Under g_lock: move to `mode` if it changed (log, new generation). */
static void note_mode(int mode, const uint32_t want[2], const char *why)
{
	static const char *const names[] = { "system servers", "profile servers", "blocked (no servers)" };

	if (mode == g_mode && (mode == MODE_SYSTEM ||
	    (want[0] == g_eff[0] && want[1] == g_eff[1])))
		return;
	g_mode = mode;
	g_eff[0] = want[0];
	g_eff[1] = want[1];
	g_gen++;
	klog("dns: resolvers now get %s (%s)", names[mode], why);
}

void
dns_refresh(const char *why)
{
	uint32_t want[2];
	int ex, mode = wanted_mode(want, &ex);

	if (mode == g_mode && mode == MODE_SYSTEM)
		return;                              /* the common case, no lock */
	if (trylock()) {
		note_mode(mode, want, why);
		unlock();
	}
}

void
dns_snapshot(struct dns_snap *d)
{
	uint32_t want[2];
	int ex;

	memset(d, 0, sizeof(*d));
	d->hooked = g_hook >= 0;
	d->mode = wanted_mode(want, &ex);
	d->eff[0] = want[0];
	d->eff[1] = want[1];
	d->sys_seen = g_sys_seen;
	d->sys[0] = g_sys[0];
	d->sys[1] = g_sys[1];
}

/*
 * Unlocked: 1 if this process needs no cache flush (nothing changed since
 * boot, or it has read since the last change). A racing writer can only
 * make it answer 0, and the locked path then decides, as before.
 */
static int seen_current(SceUID pid)
{
	uint32_t gen = g_gen;
	unsigned i;

	if (gen == 0)
		return 1;
	for (i = 0; i < SEEN_SLOTS; i++)
		if (g_seen[i].pid == pid)
			return g_seen[i].gen == gen;
	return 0;
}

static void on_read(void *ubuf)
{
	uint32_t w[3], want[2];
	int mode, flush, exempt;
	SceUID pid;

	g_wakes.dns++;
	/* Fast path ("Idle cost"): no profile, or the tunnel off
	 * without Lockdown, is the system's record for everyone - one look at
	 * g_st and no lock while nothing changed for this process. */
	if (g_mode == MODE_SYSTEM && (!g_st.configured || g_st.paused ||
	    (!g_st.want && !g_st.lockdown)) && seen_current(ksceKernelGetProcessId()))
		return;
	pid = ksceKernelGetProcessId();
	mode = wanted_mode(want, &exempt);
	flush = 0;
	if (trylock()) {
		note_mode(mode, want, "a resolver read");
		flush = needs_flush(pid);
		unlock();
	}
	/* The endpoint lookup keeps the system's servers while held (above);
	 * the mode noted for everyone else is not changed by it. */
	if (mode != MODE_SYSTEM && exempt && lookup_exempt(pid)) {
		static unsigned told;
		if (told++ < 5)
			klog("dns: endpoint lookup (pid 0x%08X) given the system's servers while held",
			    (unsigned)pid);
		mode = MODE_SYSTEM;
	}

	if (mode == MODE_SYSTEM && !flush)
		return;                              /* the common case: untouched */
	if (ksceKernelMemcpyUserToKernel(w, ubuf, sizeof(w)) < 0)
		return;
	if (mode != MODE_SYSTEM) {
		w[0] = bswap(want[0]);
		w[1] = bswap(want[1]);
	}
	if (flush)
		w[2] |= DNS_FLUSH;
	ksceKernelMemcpyKernelToUser(ubuf, w, sizeof(w));
}

static void on_write(const void *ubuf)
{
	uint32_t w[2];

	if (ksceKernelMemcpyUserToKernel(w, ubuf, sizeof(w)) < 0)
		return;
	if (trylock()) {
		g_sys[0] = bswap(w[0]);
		g_sys[1] = bswap(w[1]);
		g_sys_seen = 1;
		unlock();
	}
	klog("dns: system set %u.%u.%u.%u, %u.%u.%u.%u (pid 0x%08X)",
	    IP4(bswap(w[0])), IP4(bswap(w[1])), (unsigned)ksceKernelGetProcessId());
}

/* sceNetSyscallControl(s, code, buf, len) */
static int hook_control(int s, int code, void *buf, int len)
{
	tai_hook_ref_t ref = ref_wait();
	if (!ref)
		return HOOK_NO_REF;   /* the control call fails (SCE_ERROR_ERRNO_EIO), no work of ours */
	int ret = TAI_CONTINUE(int, ref, s, code, buf, len);

	if (ret < 0 || s >= 0 || buf == NULL || len < DNS_REC_LEN)
		return ret;
	if (code == CTL_DNS_GET)
		on_read(buf);
	else if (code == CTL_DNS_SET)
		on_write(buf);
	return ret;
}

void
dns_set_profile(const uint32_t dns[2])
{
	g_prof[1] = dns[0] ? dns[1] : 0;
	g_prof[0] = dns[0];
}

void
dns_init(void)
{
	g_lock = ksceKernelCreateMutex("vpnturbo_dns", 0, 0, NULL);
	if (g_lock < 0) {
		klog("dns: no lock (0x%08X): override off", (unsigned)g_lock);
		return;
	}
	g_ref = 0;
	g_hook = taiHookFunctionExportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_ref, "SceNetPs",
	    NETPS_SYSCALLS_LIB, NID_SYSCALL_CONTROL, hook_control);
	if (g_hook < 0)
		klog("dns: control hook failed (0x%08X): override off", (unsigned)g_hook);
	else
		klog("dns: control hook in place");
}

void
dns_fini(void)
{
	if (g_hook >= 0) {
		taiHookReleaseForKernel(g_hook, g_ref);
		g_hook = -1;
		g_ref = 0;
	}
}

int
vtGetDns(VtDns *ud)
{
	VtDns k;

	memset(&k, 0, sizeof(k));
	k.size = sizeof(k);
	k.profile[0] = g_prof[0];
	k.profile[1] = g_prof[1];
	if (g_hook >= 0)
		k.flags |= VT_DNS_HOOKED;

	{
		uint32_t want[2];
		int ex, mode = wanted_mode(want, &ex);

		if (g_sys_seen) {                /* two words: a torn read is harmless */
			k.system[0] = g_sys[0];
			k.system[1] = g_sys[1];
			k.flags |= VT_DNS_SYSTEM;
		}
		if (g_hook < 0) {
			/* no hook: nothing is known (and nothing overridden); the
			 * caller reads the resolver's own view instead */
		} else if (mode == MODE_PROFILE) {
			k.effective[0] = want[0];
			k.effective[1] = want[1];
			k.flags |= VT_DNS_OVERRIDE;
		} else if (mode == MODE_BLOCKED) {
			k.flags |= VT_DNS_BLOCKED;
		} else {
			k.effective[0] = k.system[0];
			k.effective[1] = k.system[1];
		}
	}
	return ksceKernelMemcpyKernelToUser(ud, &k, sizeof(k));
}
