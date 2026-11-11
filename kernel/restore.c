/*
 * restore.c - the restore check: proof in
 * kernel.log that a Disconnect (or a Pause) leaves the console's networking
 * exactly as it was before Connect.
 *
 * Two snapshots, compared field by field:
 *   - "before": on the first lent RX step after vtConnect, before any route
 *     of the new connection is written;
 *   - "after": a few seconds after vtDisconnect or vtPause, once the sweep
 *     of cloned host entries is done and the SceShell plugin has taken the
 *     interface down (desc[0x11]; or after 5 s more, logged either way).
 * On demand too: while ur0:data/vpnturbo/debug_routes exists, a
 * snapshot every 30 s is compared with the last "before".
 *
 * What is compared (all of it cheap; seven RTM_GETs, no other net_ps call):
 *   (a) what the routing table answers for: the default route (by its
 *       mask), the Wi-Fi gateway, a LAN host (gateway + 1, or + 2 when that
 *       is the console), 255.255.255.255, a public address (1.1.1.1), the
 *       endpoint and the tunnel address - the route found, its gateway, its
 *       interface address and flags. A host entry cloned by ARP (host flag,
 *       link-level gateway) is compared by interface address and directness
 *       only: such entries come and go with traffic on any network;
 *   (b) the interface: the module's view (attached, address in place); the
 *       plugin logs its own interface check (flags and addresses of other0
 *       and wlan0) in shell.log, "restore check (interfaces)";
 *   (c) the DNS record: the system pair, what resolvers are given and the
 *       mode (dns.c);
 *   (d) the module: our route table (count and kinds), hold, lockdown,
 *       route_flags, the route monitor and UDP socket, our sockets open in
 *       SceShell's process, the send queue.
 * The result is one line, "restore check: identical (...)", or
 * "restore check: DIFFERS in N place(s)" followed by each difference as a
 * before line and an after line. The endpoint and tunnel addresses are
 * never printed ("=" stands for the probed address itself).
 *
 * Runs only inside routes_apply (the lent RX thread, the table owned), the
 * one context where a PF_ROUTE reply can be read back.
 */
#include "tn_kern.h"
#include "dns.h"

#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>
#include "vt_paths.h"

#define DEBUG_FLAG VT_DATA_DIR "/debug_routes"

#define RTF_GATEWAY_   0x0002
#define RTF_HOST_      0x0004
#define RTF_REJECT_    0x0008
#define RTF_STATIC_    0x0800
#define RTF_BLACKHOLE_ 0x1000

#define US(s) ((SceUInt64)(s) * 1000000ULL)

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

enum { PR_DEFAULT = 0, PR_GATEWAY, PR_LAN, PR_BCAST, PR_PUBLIC, PR_ENDPOINT, PR_TUNNEL, PR_N };
static const char *const k_pr[PR_N] = {
	"default", "gateway", "LAN host", "broadcast", "1.1.1.1", "endpoint", "tunnel addr",
};
/* Printed only as "="; never the address itself. */
static const uint8_t k_private[PR_N] = { 0, 0, 0, 0, 0, 1, 1 };

struct snap {
	int      valid;
	uint32_t probe[PR_N];
	struct rt_look rt[PR_N];
	struct dns_snap dns;
	int      napp;
	unsigned kinds;
	int      hold, lockdown, locked, attached, iface_up, udp, mon, socks;
	uint8_t  route_flags;
	unsigned queued;
	uint32_t frames, off, held;
};

static struct snap g_before, g_now;
static volatile int g_req;               /* 0, 1 before, 2 after disconnect, 4 after pause */
static volatile SceUInt64 g_due;
static volatile int g_debug;
static SceUInt64 g_next_debug;

static SceUInt64 now_us(void) { return ksceKernelGetSystemTimeWide(); }

void
restore_request(int kind, SceUInt64 due)
{
	g_due = due;
	g_req = kind;
	tn_wake();
}

int
restore_pending(void)
{
	return g_req != 0 || g_debug;
}

int
restore_debug_on(void)
{
	return g_debug;
}

void
restore_debug_tick(void)
{
	SceIoStat st;
	int on = ksceIoGetstat(DEBUG_FLAG, &st) >= 0;

	if (on != g_debug) {
		g_debug = on;
		g_next_debug = 0;
		klog("restore check: on-demand snapshots %s (debug_routes)", on ? "every 30 s" : "off");
	}
}

/* ---- taking a snapshot ------------------------------------------------------ */

static void
take(struct snap *sn, const struct snap *probes, int (*open_sock)(void), int *s,
    void *ubuf, uint32_t ubuf_size)
{
	int i;

	memset(sn, 0, sizeof(*sn));
	if (*s < 0)
		*s = open_sock();
	if (probes) {
		memcpy(sn->probe, probes->probe, sizeof(sn->probe));
	} else {
		/* The gateway from the default route itself, so the probes do not
		 * depend on what routes.c has learnt (it forgets on a connect). */
		struct rt_look d;
		uint32_t gw = 0, me = g_st.wlan_address;
		if (*s >= 0 && routes_lookup(*s, 0, 1, 0, ubuf, ubuf_size, &d) == 0 && d.gw_kind == 1)
			gw = d.gw;
		if (!gw)
			gw = g_st.wlan_gateway;
		sn->probe[PR_GATEWAY] = gw;
		sn->probe[PR_LAN] = gw ? (gw + 1 != me ? gw + 1 : gw + 2) : 0;
		sn->probe[PR_BCAST] = 0xffffffffu;
		sn->probe[PR_PUBLIC] = 0x01010101u;
		sn->probe[PR_ENDPOINT] = g_st.endpoint_ip;
		sn->probe[PR_TUNNEL] = g_st.tunnel_address;
	}
	for (i = 0; i < PR_N && *s >= 0; i++) {
		if (i == PR_DEFAULT)
			routes_lookup(*s, 0, 1, 0, ubuf, ubuf_size, &sn->rt[i]);
		else if (sn->probe[i])
			routes_lookup(*s, sn->probe[i], 0, 0, ubuf, ubuf_size, &sn->rt[i]);
	}
	dns_snapshot(&sn->dns);
	routes_table(&sn->napp, &sn->kinds);
	sn->hold = tn_hold_reason();
	sn->lockdown = g_st.lockdown;
	sn->locked = g_st.locked;
	sn->attached = g_st.attached;
	sn->iface_up = g_st.iface_up;
	sn->route_flags = g_st.route_flags;
	sn->udp = transport_socket_open();
	sn->mon = routes_monitor_open();
	sn->socks = g_socks_open - (*s >= 0 ? 1 : 0);   /* not this snapshot's own */
	sn->queued = transport_queued();
	iface_counts(&sn->frames, &sn->off, &sn->held);
	sn->valid = 1;
}

/* ---- formatting --------------------------------------------------------------- */

static int
is_clone(const struct rt_look *r)
{
	return r->ok && (r->flags & RTF_HOST_) && r->gw_kind == 2;
}

/* Reached on-link: an ARP host entry, or an interface (non-gateway) route. */
static int
direct(const struct rt_look *r)
{
	return is_clone(r) || !(r->flags & RTF_GATEWAY_);
}

static void
fmt_rt(char *b, int cap, int i, const struct snap *sn)
{
	const struct rt_look *r = &sn->rt[i];
	int n = 0;

	if (i != PR_DEFAULT && !sn->probe[i]) {
		snprintf(b, (size_t)cap, "not probed");
		return;
	}
	if (!r->ok) {
		snprintf(b, (size_t)cap, "no route (no answer)");
		return;
	}
	if (k_private[i] && r->dst == sn->probe[i])
		n = snprintf(b, (size_t)cap, "dst =");
	else
		n = snprintf(b, (size_t)cap, "dst %u.%u.%u.%u", IP4(r->dst));
	if (r->has_mask && n < cap)
		n += snprintf(b + n, (size_t)(cap - n), "/%d", r->mask ? 32 - __builtin_ctz(r->mask) : 0);
	if (n < cap) {
		if (r->gw_kind == 1)
			n += snprintf(b + n, (size_t)(cap - n), " gw %u.%u.%u.%u", IP4(r->gw));
		else
			n += snprintf(b + n, (size_t)(cap - n), " gw %s", r->gw_kind == 2 ? "link" : "-");
	}
	if (n < cap) {
		if (!r->has_ifa)
			n += snprintf(b + n, (size_t)(cap - n), " ifa ?");
		else if (r->ifa && r->ifa == g_st.tunnel_address)
			n += snprintf(b + n, (size_t)(cap - n), " ifa TUNNEL");
		else
			n += snprintf(b + n, (size_t)(cap - n), " ifa %u.%u.%u.%u", IP4(r->ifa));
	}
	if (n < cap)
		snprintf(b + n, (size_t)(cap - n), " f 0x%X", (unsigned)r->flags);
}

static int
same_rt(const struct rt_look *a, const struct rt_look *b)
{
	const int32_t keep = RTF_GATEWAY_ | RTF_REJECT_ | RTF_BLACKHOLE_ | RTF_STATIC_;

	if (a->ok != b->ok)
		return 0;
	if (!a->ok)
		return 1;
	if (a->has_ifa != b->has_ifa || a->ifa != b->ifa)
		return 0;
	if (is_clone(a) || is_clone(b))
		/* ARP entries come and go: the same interface, both direct. */
		return direct(a) && direct(b);
	return a->dst == b->dst && a->has_mask == b->has_mask && a->mask == b->mask &&
	    a->gw_kind == b->gw_kind && a->gw == b->gw && (a->flags & keep) == (b->flags & keep);
}

static void
fmt_dns(char *b, int cap, const struct dns_snap *d)
{
	static const char *const m[] = { "system", "PROFILE", "BLOCKED", "?" };
	snprintf(b, (size_t)cap, "%s%s sys %u.%u.%u.%u %u.%u.%u.%u gives %u.%u.%u.%u",
	    d->hooked ? "" : "no hook, ", m[d->mode & 3], IP4(d->sys[0]), IP4(d->sys[1]),
	    IP4(d->mode == 0 ? d->sys[0] : d->eff[0]));
}

static int
same_dns(const struct dns_snap *a, const struct dns_snap *b)
{
	return a->hooked == b->hooked && a->mode == b->mode &&
	    a->eff[0] == b->eff[0] && a->eff[1] == b->eff[1] &&
	    a->sys[0] == b->sys[0] && a->sys[1] == b->sys[1];
}

static void
fmt_mod(char *b, int cap, const struct snap *sn)
{
	snprintf(b, (size_t)cap, "rt %d k%02X rf%02X hold %d ld %d lk %d ifup %d mon %d udp %d socks %d q %u",
	    sn->napp, sn->kinds, (unsigned)sn->route_flags, sn->hold, sn->lockdown, sn->locked,
	    sn->iface_up, sn->mon, sn->udp, sn->socks, sn->queued);
}

static int
same_mod(const struct snap *a, const struct snap *b, int pause)
{
	if (a->napp != b->napp || a->kinds != b->kinds || a->route_flags != b->route_flags ||
	    a->hold != b->hold || a->lockdown != b->lockdown || a->locked != b->locked ||
	    a->iface_up != b->iface_up || a->mon != b->mon)
		return 0;
	if (pause)
		return 1;               /* a pause keeps the session and its socket */
	return a->udp == b->udp && a->socks == b->socks && a->queued == b->queued;
}

static void
log_snap(const char *what, const struct snap *sn)
{
	char b[96];
	int i;

	klog("snap %s:", what);
	for (i = 0; i < PR_N; i++) {
		fmt_rt(b, sizeof(b), i, sn);
		klog("  %s: %s", k_pr[i], b);
	}
	fmt_dns(b, sizeof(b), &sn->dns);
	klog("  dns: %s", b);
	fmt_mod(b, sizeof(b), sn);
	klog("  module: %s", b);
}

static void
compare(const char *what, const struct snap *a, const struct snap *b, int pause)
{
	char x[96], y[96];
	int i, diffs = 0;

	for (i = 0; i < PR_N; i++)
		diffs += !same_rt(&a->rt[i], &b->rt[i]);
	diffs += !same_dns(&a->dns, &b->dns);
	diffs += !same_mod(a, b, pause);
	if (!diffs) {
		klog("restore check (%s): identical (%d lookups, dns, module; frames off %u held %u)",
		    what, PR_N, b->off, b->held);
		return;
	}
	klog("restore check (%s): DIFFERS in %d place(s)", what, diffs);
	for (i = 0; i < PR_N; i++) {
		if (same_rt(&a->rt[i], &b->rt[i]))
			continue;
		fmt_rt(x, sizeof(x), i, a);
		fmt_rt(y, sizeof(y), i, b);
		klog("  %s before: %s", k_pr[i], x);
		klog("  %s now:    %s", k_pr[i], y);
	}
	if (!same_dns(&a->dns, &b->dns)) {
		fmt_dns(x, sizeof(x), &a->dns);
		fmt_dns(y, sizeof(y), &b->dns);
		klog("  dns before: %s", x);
		klog("  dns now:    %s", y);
	}
	if (!same_mod(a, b, pause)) {
		fmt_mod(x, sizeof(x), a);
		fmt_mod(y, sizeof(y), b);
		klog("  module before: %s", x);
		klog("  module now:    %s", y);
	}
}

/* ---- the step ----------------------------------------------------------------- */

void
restore_step(int phase, int busy, int (*open_sock)(void), int *s, void *ubuf, uint32_t ubuf_size)
{
	SceUInt64 now = now_us();
	int req = g_req;

	if (phase == 0) {
		if (req != 1)
			return;
		g_req = 0;
		take(&g_before, NULL, open_sock, s, ubuf, ubuf_size);
		log_snap("before connect", &g_before);
		return;
	}

	if (req == 2 || req == 4) {
		if (req == 2 && g_st.want) {
			g_req = 0;              /* reconnected first: nothing to check */
		} else if (req == 4 && !g_st.paused) {
			g_req = 0;              /* resumed first */
		} else if (now >= g_due && !busy &&
		    (!g_st.iface_up || TN_LD_HOLD() || now >= g_due + US(5))) {
			g_req = 0;
			if (!g_before.valid) {
				klog("restore check: no before-connect snapshot to compare with");
			} else {
				if (g_st.iface_up && !TN_LD_HOLD())
					klog("restore check: the interface is still up 7 s after %s",
					    req == 2 ? "the disconnect" : "the pause");
				take(&g_now, &g_before, open_sock, s, ubuf, ubuf_size);
				compare(req == 2 ? "after disconnect" : "after pause", &g_before, &g_now, req == 4);
			}
		}
	}

	if (g_debug && now >= g_next_debug && g_before.valid) {
		g_next_debug = now + US(30);
		take(&g_now, &g_before, open_sock, s, ubuf, ubuf_size);
		compare(g_st.want ? "on demand, connected" : "on demand", &g_before, &g_now, g_st.paused);
	}
}
