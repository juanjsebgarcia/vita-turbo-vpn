/*
 * routes.c - the route manager (the 4.4BSD routing socket).
 *
 * Routes are written through a PF_ROUTE socket with 4.4BSD rt_msghdr
 * messages, as SceShell's netctl does (the kernel send path is proven).
 * netctl's default route and its Wi-Fi subnet route are not changed; ours are
 * more specific, so the two never fight. The one exception is the refresh
 * when tunnel (or block) routes go in: the default route is deleted and
 * added back unchanged, so connections that remembered it look their route
 * up again (FLUSH OF REMEMBERED ROUTES, below). It is read first, put back
 * with its own gateway, checked, and retried in later steps until it is
 * back; a disconnect or pause checks that it exists.
 *
 * RECONCILER. This file keeps a table of the routes it has applied and, on
 * every lent RX step, computes the routes that should be in place from the
 * shared state; the difference is written. Additions go first, most
 * direct route first (endpoint, gateway), then the tunnel routes;
 * removals go last, most specific use first. A route whose destination
 * stays but whose gateway changes (a new tunnel address, a refresh after
 * the interface bounced) is deleted and re-added back to back. Removals
 * may also be applied at once from a syscall (routes_sync: disconnect,
 * pause, kill switch off, allow local on); additions and everything that
 * needs a reply (RTM_GET) happen only on the lent RX thread, the one
 * context where a PF_ROUTE reply can be read into a user buffer.
 *
 * WHAT SHOULD BE IN PLACE
 *   - nothing while paused by the user (pause lets traffic out even
 *     with the kill switch on) or with no profile;
 *   - tunnel not wanted (disconnected): nothing, kill switch or not
 *     (a deliberate Disconnect restores native
 *     networking; the kill switch only holds while the tunnel is wanted).
 *     Lockdown mode is the exception: then the tunnel set below without
 *     the endpoint route (0/1 + 128/1 into the interface, which drops it;
 *     the broadcast /32 so DHCP stays on the Wi-Fi; with allow local off
 *     the subnet halves and the gateway /31), once the gateway, the Wi-Fi
 *     address and the interface's address are known, else what is in place
 *     stays. Lockdown also gives a split tunnel (and Table = off) 0/1 +
 *     128/1, so traffic outside AllowedIPs is dropped, never sent direct;
 *   - wanted:
 *       endpoint /32 via the Wi-Fi gateway, as soon as the gateway is known
 *         (flags 0x803), so the tunnel's own packets leave over Wi-Fi;
 *       tunnel routes (0/1 + 128/1 for a full tunnel, else each AllowedIPs
 *         prefix; interface routes via the tunnel address, flags 0x801) when
 *         the tunnel is UP, or was UP and is re-handshaking, or the resume
 *         lockdown is on, or the kill switch is on (connecting,
 *         stalled or in error the routes stay and traffic is dropped in the
 *         tunnel instead of leaking). They are added only once the endpoint
 *         route is in place and the interface holds its address (desc[0x10]
 *         seen) - routing everything into the interface before both would take the
 *         console down. If those conditions are lost
 *         while held (lockdown or kill switch), the routes already in place
 *         are kept as they are;
 *       allow local off on a full tunnel: the Wi-Fi subnet split in
 *         two halves via the tunnel (one bit longer than netctl's subnet
 *         route, so they win), plus the gateway's /31 via the Wi-Fi address,
 *         longer still, so the gateway the endpoint route needs stays on the
 *         Wi-Fi. A /31 and not a /32: on this BSD stack a /32 interface route
 *         is not cloning and so gets no ARP entry; a /31 does. The cost is
 *         that the gateway's /31 neighbour is also reached directly.
 *
 * THE BLOCK. The tunnel routes need the interface up
 * with its address, which at a connect (kill switch) and at boot
 * (Lockdown) comes seconds after the gateway is known; before, traffic in
 * that window left directly. Now, while traffic is to be held and the
 * tunnel routes cannot be built yet, the same destinations get REJECT
 * routes via lo0 (gateway 127.0.0.1, flags 0x809, K_BLK): an interface
 * route through the loopback, whose output routine drops a packet on a
 * RTF_REJECT|RTF_BLACKHOLE route and returns EHOSTUNREACH / ENETUNREACH
 * for REJECT, as 4.4BSD's looutput does (rt_flags & 0x1008); the route is
 * built the 4.4BSD way (ifa_ifwithroute: a non-gateway route via an
 * interface's own address).
 * REJECT, not BLACKHOLE: a sender fails at once instead of waiting out its
 * timeouts. The set is the tunnel set's (0/1 + 128/1, or AllowedIPs; the
 * subnet halves with allow local off) with the broadcast /32 and the
 * gateway /31 on the Wi-Fi as usual, and the endpoint /32 first; no DNS
 * /31. It needs only the gateway and the Wi-Fi address (the broadcast
 * route keeps DHCP on the Wi-Fi), not the interface. When the interface is
 * ready each K_BLK is swapped for its tunnel route (same destination, new
 * gateway: deleted and re-added back to back on the lent RX thread; the
 * gap is two PF_ROUTE messages). Refused three times (lo0 without its
 * address, say), the block is given up for the connection and the window
 * is as before. VT_RT_BLOCK in route_flags; routes_iface_for says
 * VT_IFACE_NONE for a blocked address.
 *
 * ROAMING (proto.c, transport.c). The endpoint /32 is pinned to the
 * configured (or resolved) endpoint; a roam never moves it. The address a
 * completed handshake response came from, when it is another one, gets its
 * own /32 the same way (K_ROAM), and datagrams go there only once that is
 * in place (transport_roam_routed). At most one: a new roam replaces it,
 * and a profile, vtSetEndpoint, connect or disconnect drops it.
 *
 * WI-FI KNOWLEDGE (all from RTM_GET replies on the lent RX thread):
 *   - the gateway: RTM_GET of the endpoint before our /32 is in place gives
 *     netctl's default route; if our routes already cover the endpoint,
 *     RTM_GET of 0.0.0.0 with netmask 0 asks for the default route by its
 *     mask, and failing that the gateway of netctl's last default-route
 *     message (the monitor) is used;
 *   - the Wi-Fi address: the RTA_IFA of that reply (we ask for RTA_IFP|IFA);
 *   - the Wi-Fi subnet: RTM_GET of an address in the block the Wi-Fi address
 *     and the gateway share, picked so it is neither of them (the block's
 *     last address); the reply is netctl's interface route for the subnet,
 *     with its netmask, checked to hold both addresses. This avoids a new
 *     syscall for the shell to pass netctl's figures in, and it follows the
 *     routing table, which is what decides where packets go anyway.
 *
 * WI-FI CHANGES. A PF_ROUTE socket kept open on the lent RX thread receives
 * every routing message net_ps broadcasts; RTM_NEWADDR/RTM_DELADDR of a
 * Wi-Fi address, a new default gateway and the deletion of our endpoint
 * route mean the network changed. A check every 10 s (RTM_GET of the
 * endpoint and of the default route) catches a missed message, and a wake
 * always counts as a change. On a change: re-learn the gateway and address
 * (relearn) while the old endpoint route stays in place, swap the endpoint
 * route if the gateway moved (back to back), re-learn the subnet if the
 * network changed, and re-handshake; if tunnel routes were in place the resume
 * lockdown is armed for it too (TN_LOCK_ON_WIFI_CHANGE), so nothing leaks
 * while the tunnel is re-established. Our own messages carry SEQ_TAG and
 * are skipped.
 *
 * OWNERSHIP, NOT A MUTEX. The table and the knowledge belong to whoever
 * holds a try-only ownership flag (g_owner, an atomic exchange). It is held
 * across PF_ROUTE sendto/recvfrom (net_ps calls), which is why it is not a
 * kernel mutex: NOBODY ever waits for it. The lent RX step that finds it
 * taken skips its route work for that step (the next one is 2-20 ms
 * later); a syscall (disconnect, pause, options, connect: SceShell's UI
 * thread via the quick menu, Settings, the app) that finds it taken leaves
 * the work to the RX step (a flag). So a thread stuck in net_ps while
 * holding it can never take another thread with it (a thread waiting on a
 * kernel mutex can be neither suspended nor killed, which stalls the
 * system's suspend sequence and the LiveArea close), and no net_ps
 * callback into this module (desc[0x10]/[0x11]/[0x12], under the giant
 * lock) touches it - they only set flags. Order while held: the protocol
 * lock, only ever TRIED here (tnp_rehandshake_try; busy = next step), then
 * the transport ring lock; neither is held across a net_ps call.
 *
 * SWEEP. Every interface route of ours clones a host entry per destination
 * (resolved by the interface's synthetic ARP, iface.c). When the tunnel
 * routes go (Disconnect, Pause, the hold released) or the subnet halves go
 * (allow local on), each address the interface answered ARP for is looked
 * up and, if a host entry for it is still on the tunnel interface, deleted
 * (RTM_DELETE of the host), 16 per RX step; one line says how many. A
 * clone left behind would keep that destination going into the dead
 * interface (for a LAN host: even with allow local on).
 *
 * LAN PURGE. The other way round: a LAN host the
 * console talked to directly (allow local on, or before the connect, or an
 * FTP client) has a cloned host entry (RTF_HOST|RTF_LLINFO, ARP) on the
 * Wi-Fi. It is a /32, more specific than the subnet halves, so with allow
 * local off traffic to that host kept going direct until the entry expired.
 * So whenever the halves (or their REJECT stand-ins) are added with allow
 * local off - connect, options local=0, the Lockdown hold, a network
 * change - every such entry on the Wi-Fi for a host in the subnet is
 * deleted (RTM_DELETE of the host), except the gateway and its /31
 * neighbour (direct by design), the console's own address and the
 * network/broadcast addresses. They are found by:
 *   1. a routing-table dump: sysctl {CTL_NET, PF_ROUTE, 0, AF_INET,
 *      NET_RT_FLAGS, RTF_LLINFO} through net_ps's sceNetSyscallSysctl
 *      (SceNetPsForSyscalls 0x2CBED2C6 / 0x3D7495B0, exported on 3.60-3.74),
 *      called on the lent RX thread with its arguments in the lent user
 *      buffer (every pointer is copied in and out as user memory). A dump
 *      cut short by the buffer (ENOMEM: whole entries, end unknown, so the
 *      buffer is zeroed first) is repeated after the deletions, up to 8
 *      times while each one deletes something;
 *   2. otherwise (no dump, or one that stays cut short): RTM_GET of every
 *      host address of the subnet, 16 a step, for a /24 or smaller; for a
 *      larger subnet only the LAN hosts the monitor saw (below).
 * The monitor (while active) also records every LAN host entry net_ps
 * announces on the Wi-Fi (rtalloc1's RTM_ADD after a clone, 32 kept) and,
 * while the halves are in place, logs and deletes one that appears. Each
 * candidate is checked before it is deleted: a host entry for exactly that
 * address, not a gateway route, a link-layer gateway, and the Wi-Fi address
 * as its interface address. With allow local on (or the halves not in
 * place) nothing is touched; a purge under way stops. One line per purge.
 *
 * SOCKETS. The route monitor is open only while the module is active
 * (tunnel wanted or the Lockdown hold); the per-step route socket is closed
 * at the end of each step. g_socks_open counts every socket of ours in
 * SceShell's process (the restore check logs it).
 *
 * NO NET_PS WHILE SUSPENDED OR WAITING FOR THE NETWORK (TN_NO_NETPS: from
 * the first suspend event until vtNetReady after the wake): routes_apply
 * and routes_sync do nothing then, removals included; they are applied on
 * the first RX step after the network is reported ready.
 *
 * AFTER A WAKE OR A CHANGE THE STACK IS NOT TRUSTED. The network may have
 * reassociated; the stack may have deleted or orphaned routes that went
 * through the old address. So once the gateway is known again every route
 * in the table is CHECKED, verify first: each network-side one (endpoint,
 * gateway /31, broadcast) is looked up (RTM_GET); in place and pointing the
 * right way (gateway, interface address) = nothing sent; missing (the
 * lookup falls to a less specific route) or unanswered = RTM_ADD (EEXIST =
 * it was there after all); only one that points the wrong way (another
 * gateway, another address, or the tunnel) is deleted and re-added back to
 * back. Tunnel ones are re-added (EEXIST = still there). So the endpoint
 * route is never deleted just to be safe: that would leave a window with no
 * endpoint route on every wake.
 * While the tunnel is not UP the endpoint is looked up every 5 s (it must
 * resolve to our /32 via the gateway, never to the tunnel), one health line
 * is logged, and the network-side routes are checked again every 20 s. A
 * datagram of the tunnel's own reaching the tunnel interface (the loop
 * signature, iface.c) checks the endpoint route at once.
 *
 * LIMITED BROADCAST. With 0/1 + 128/1 in place, 255.255.255.255 matches
 * 128/1, so a DHCP request sent through the routing table after Wi-Fi
 * reassociates would go into the tunnel (and be held while LOCKED). On a
 * full tunnel a 255.255.255.255/32 interface route via the Wi-Fi address
 * (TN_BCAST_ROUTE) keeps it on the Wi-Fi, where it went before the tunnel.
 * Refused three times, it is given up for the connection (logged).
 *
 * DEVICE NOTE: the RTM_ADD send is proven. The RTM_GET reply
 * fields beyond the gateway (RTA_IFA, the netmask), the monitor messages and
 * the default-route-by-mask lookup follow 4.4BSD and are validated on a
 * console. Each degrades safely:
 * without RTA_IFA the subnet is not learnt (allow local off then reports
 * VT_RT_LOCAL_UNKNOWN); without monitor messages the 10 s check and the
 * wake still trigger a rediscovery.
 */
#include "tn_kern.h"
#include "dns.h"
#include "strand.h"

#include <psp2kern/netps.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>		/* KERNEL_PID */

#include <stdio.h>
#include <string.h>

/* taihenModuleUtils (no SDK header). */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);

/* The LAN purge's routing-table dump (file comment); 0 = RTM_GET sweep only. */
#ifndef TN_LAN_DUMP
#define TN_LAN_DUMP 1
#endif

/* With tunnel routes in place, a Wi-Fi change arms the resume lockdown as a
 * wake does (0 = only a wake locks). */
#ifndef TN_LOCK_ON_WIFI_CHANGE
#define TN_LOCK_ON_WIFI_CHANGE 1
#endif

/* The 255.255.255.255/32 route on the Wi-Fi with a full tunnel (above). */
#ifndef TN_BCAST_ROUTE
#define TN_BCAST_ROUTE 1
#endif

/* The profile's DNS servers into the tunnel when the LAN route would
 * otherwise take them direct (put_tunnel_set). */
#ifndef TN_DNS_ROUTE
#define TN_DNS_ROUTE 1
#endif

#define RTM_VERSION 3
#define RTM_ADD     1
#define RTM_DELETE  2
#define RTM_CHANGE  3
#define RTM_GET     4
#define RTM_NEWADDR 0xc
#define RTM_DELADDR 0xd

#define RTF_UP      0x0001
#define RTF_GATEWAY 0x0002
#define RTF_HOST    0x0004
#define RTF_REJECT  0x0008
#define RTF_LLINFO  0x0400   /* an ARP entry (as in 4.4BSD) */
#define RTF_STATIC  0x0800

/* lo0's address: the gateway of the reject routes (K_BLK, below). */
#define LO0_ADDR    0x7F000001u

#define RTA_DST     0x01
#define RTA_GATEWAY 0x02
#define RTA_NETMASK 0x04
#define RTA_IFP     0x10
#define RTA_IFA     0x20
enum { AX_DST = 0, AX_GATEWAY, AX_NETMASK, AX_GENMASK, AX_IFP, AX_IFA, AX_AUTHOR, AX_BRD, AX_MAX };

#define RTM_HDR   0x4c          /* rt_msghdr */
#define IFAM_HDR  20            /* ifa_msghdr, 4.4BSD layout (NEWADDR/DELADDR) */
#define SEQ_TAG   0x54550000u   /* our messages' rtm_seq ("TU"): the monitor skips them */
#define EEXIST_   17
#define EAGAIN_   (-35)
#define MSG_DONTWAIT_ 0x80

#define US(s) ((SceUInt64)(s) * 1000000ULL)

typedef struct {
	uint8_t  len;
	uint8_t  family;
	uint16_t port;
	uint32_t addr;    /* network order */
	uint16_t vport;
	uint8_t  zero[6];
} SockaddrIn;

/* ---- the applied table --------------------------------------------------- */

/* K_ROAM: the /32 of the address a handshake roamed to (as K_EP).
 * K_BLK: a reject route via lo0 standing in for a tunnel route (or a subnet
 * half) until the tunnel interface can take it (see THE BLOCK). */
enum { K_EP = 1, K_GWNET, K_TUN, K_LOCAL, K_BCAST, K_DNS, K_ROAM, K_BLK };

struct rt_ent {
	uint32_t dst, mask, gw;   /* host order */
	uint32_t gen;             /* g_st.iface_gen it was added under (tunnel side) */
	uint32_t seq;             /* rtm_seq of the last message about it */
	uint16_t flags;
	uint8_t  kind;            /* K_* */
	uint8_t  pad;
};

#define RT_MAX (VT_MAX_ALLOWED + 10)

static struct rt_ent g_app[RT_MAX];
static int g_napp;
static volatile int g_owner;              /* the try-only ownership flag (see OWNERSHIP) */
static uint32_t g_seq;
static volatile int g_dirty;

/* What is known about the Wi-Fi side (owned with g_owner). */
static uint32_t g_gw, g_wlan;            /* host order, 0 = unknown */
static uint8_t  g_sub_prefix;            /* 0 = subnet unknown */
static int      g_rehs_pending;          /* re-handshake once the gateway is known */
static int      g_rediscover;            /* after a change: re-learn, keeping the old meanwhile */
static unsigned g_relearn_tries;
static int      g_ep_lost;               /* our endpoint /32 vanished from the stack */
static uint32_t g_mon_gw, g_mon_wlan;    /* last default gateway / Wi-Fi address netctl announced */
static volatile int g_netchg;            /* a change to process (any thread may set it) */
static int g_sock_defer = -1;            /* a route socket left open when the Wi-Fi went away */
static const char *volatile g_netchg_why;
static SceUInt64 g_next_discover, g_next_subnet, g_next_verify, g_add_hold_until;

/* Re-assertion after a wake or change, and the not-UP health checks. */
static volatile int g_reassert;          /* re-assert every route once the gateway is known */
static const char *volatile g_reassert_why;
static volatile int g_ep_suspect;        /* loop signature seen (iface.c) */
static SceUInt64 g_next_refresh, g_last_ep_refresh;
static unsigned g_reassert_logs;
static unsigned g_health_logs;           /* health lines since the last change */
static unsigned g_bcast_refused;         /* the broadcast route: given up at 3 */
static unsigned g_blk_refused;           /* the reject routes: given up at 3 */
static int g_no_default_told, g_get_silent_told;
static volatile int g_connect_pending;   /* vtConnect found the lock busy */
static int g_in_rx;                      /* inside routes_apply (watchdog marks) */
static unsigned g_mon_count[4];          /* monitor: NEWADDR, DELADDR, ADD/CHANGE, DELETE */

/* The change monitor (lent RX thread only). */
static int g_mon = -1;
static unsigned g_mon_errs;
static SceUInt64 g_mon_retry, g_mon_next;
static volatile int g_mon_reopen;        /* rebuild it after a wake */

/* The sweep (file comment). */
static volatile int g_sweep;
static uint32_t g_sweep_ta;
static unsigned g_sw_seen, g_sw_del, g_sw_gone, g_sw_lost;

/* ---- small helpers -------------------------------------------------------- */

/* Never waits (see OWNERSHIP). 1 = now held by the caller. */
static int  trylock(void) { return __atomic_exchange_n(&g_owner, 1, __ATOMIC_ACQUIRE) == 0; }
static void unlock(void)  { __atomic_store_n(&g_owner, 0, __ATOMIC_RELEASE); }

/* Name the net_ps call the lent RX step is in, for the watchdog. */
static void ph(const char *what) { if (g_in_rx) wd_phase(WD_RX, what); }

static SceUInt64 now_us(void) { return ksceKernelGetSystemTimeWide(); }

/* Route work wanted at all: the tunnel wanted and not paused, or the
 * Lockdown hold with it off. */
static int active(void) { return (g_st.want && !g_st.paused) || TN_LD_HOLD(); }

/* Every socket of ours in SceShell's process is counted (g_socks_open). */
static void
sock_close(int s)
{
	ksceNetClose(s);
	g_socks_open--;
}

static uint32_t
h2n(uint32_t ip)
{
	return ((ip & 0xff) << 24) | ((ip & 0xff00) << 8) |
	    ((ip >> 8) & 0xff00) | (ip >> 24);
}

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int32_t  rd32ne(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

/*
 * Every routing message is received into the lent SceShell buffer and
 * only then copied into kernel memory; nothing reads the user buffer after
 * that copy, so a change to it later cannot reach the parsing. rt_copyin
 * copies `copy` bytes (<= what was received) and drops the message (0) if
 * the copy fails, the version is not RTM_VERSION, or the message's own
 * length (rtm_msglen) is under `min` or over what was received (cut short
 * by the buffer: not parsed). Returns the message's length otherwise.
 *
 * Residual risk: the lent buffer is SceShell's memory
 * and the lent threads are pinned to SceShell (control.c), so rewriting it
 * between recvfrom and the copy takes code running inside SceShell, which
 * can already make every control call; that is accepted.
 */
static unsigned g_rt_bad;                 /* messages dropped by rt_copyin */

static int
rt_copyin(uint8_t *dst, int copy, const void *ubuf, int n, int min)
{
	uint16_t len;

	if (n < 4 || n < min || copy > n)
		return 0;
	if (ksceKernelMemcpyUserToKernel(dst, ubuf, (SceSize)copy) < 0) {
		if (g_rt_bad++ < 5)
			klog("routes: message copy failed: dropped");
		return 0;
	}
	memcpy(&len, dst, 2);
	if (dst[2] != RTM_VERSION || len < min || len < 4 || len > n) {
		if (g_rt_bad++ < 5)
			klog("routes: malformed message dropped (version %u, length %u of %d)",
			    (unsigned)dst[2], (unsigned)len, n);
		return 0;
	}
	return len;
}

/*
 * rtm_pid of the replies to our own RTM_GETs. net_ps answers on every
 * PF_ROUTE socket, so another process could send an RTM_GET carrying our
 * rtm_seq and have its answer read as ours. The kernel side of a 4.4BSD
 * stack stamps rtm_pid with the sender; the first answer we read (at boot,
 * on the lent RX thread) sets the value, and a later answer with another
 * one is dropped. If net_ps passes our own field through (we send 0), the
 * check is weaker, not wrong.
 */
static int     g_rt_pid_known;
static int32_t g_rt_pid;

static uint32_t
prefix_mask(unsigned p)
{
	return p == 0 ? 0 : (p >= 32 ? 0xffffffffu : ~((1u << (32 - p)) - 1));
}

/* Prefix length of a contiguous mask, or -1. */
static int
mask_prefix(uint32_t m)
{
	uint32_t inv = ~m;
	int p = 0;

	if (inv & (inv + 1))
		return -1;
	while (p < 32 && (m & (0x80000000u >> p)))
		p++;
	return p;
}

static void
sin_set(SockaddrIn *s, uint32_t host_ip)
{
	memset(s, 0, sizeof(*s));
	s->len = sizeof(*s);
	s->family = 2;
	s->addr = h2n(host_ip);
}

static uint32_t
next_seq(void)
{
	g_seq = (g_seq + 1) & 0xffffu;
	return SEQ_TAG | g_seq;
}

/* rt_msghdr + the sockaddrs named in `addrs` (DST, GATEWAY, NETMASK only),
 * packed in RTAX order. RTA_IFP/RTA_IFA may be set without a sockaddr: on a
 * GET they ask the reply to carry the interface and its address. */
static int
build(uint8_t *msg, int type, int flags, int addrs, uint32_t seq,
    uint32_t dst, uint32_t gw, uint32_t mask)
{
	int n = RTM_HDR;

	memset(msg, 0, RTM_HDR + 3 * sizeof(SockaddrIn));
	msg[2] = RTM_VERSION;
	msg[3] = (uint8_t)type;
	*(int32_t *)(msg + 0x08) = flags;
	*(int32_t *)(msg + 0x0c) = addrs;
	*(uint32_t *)(msg + 0x14) = seq;
	if (addrs & RTA_DST)     { sin_set((SockaddrIn *)(msg + n), dst);  n += sizeof(SockaddrIn); }
	if (addrs & RTA_GATEWAY) { sin_set((SockaddrIn *)(msg + n), gw);   n += sizeof(SockaddrIn); }
	if (addrs & RTA_NETMASK) { sin_set((SockaddrIn *)(msg + n), mask); n += sizeof(SockaddrIn); }
	*(uint16_t *)(msg + 0) = (uint16_t)n;
	return n;
}

/*
 * Walk the sockaddrs of a routing message from `off`, in RTAX order. AF_INET
 * ones land in v[] with have[] = 1; others (AF_LINK) have[] = 2. A netmask
 * may come shortened (trailing zero bytes cut, any family, length 0 for
 * 0.0.0.0) and is read byte by byte. Sockaddrs are 4-byte aligned.
 */
static void
parse_addrs(const uint8_t *m, int n, int off, uint32_t addrs, uint32_t v[AX_MAX], uint8_t have[AX_MAX])
{
	int i;

	memset(have, 0, AX_MAX);
	memset(v, 0, AX_MAX * sizeof(v[0]));
	for (i = 0; i < AX_MAX && off < n; i++) {
		uint8_t len;
		if (!(addrs & (1u << i)))
			continue;
		len = m[off];
		if (i == AX_NETMASK) {
			uint32_t x = 0;
			int k;
			for (k = 0; k < 4; k++)
				if (4 + k < len && off + 4 + k < n)
					x |= (uint32_t)m[off + 4 + k] << (24 - 8 * k);
			v[i] = x;
			have[i] = 1;
		} else if (len >= 8 && off + 8 <= n && m[off + 1] == 2) {
			v[i] = rd32be(m + off + 4);
			have[i] = 1;
		} else {
			have[i] = 2;
		}
		off += len ? ((len + 3) & ~3) : 4;
	}
}

static int
route_socket(void)
{
	int s;

	ph("route socket");
	s = ksceNetSocket("vpnturbo_rt", 17 /*PF_ROUTE*/, 3 /*SOCK_RAW*/, 0);
	if (s < 0) {
		klog("routes: socket 0x%08X", (unsigned)s);
		g_st.last_error = VT_ERR_ROUTE;
	} else {
		g_socks_open++;
	}
	return s;
}

static int g_last_send_ret;               /* ksceNetSendto's result for the last route message */

/* Instrumentation: a net_ps call of ours that took more than 200 ms. */
static void
slow_note(const char *what, SceUInt64 t0)
{
	SceUInt64 dt = ksceKernelGetSystemTimeWide() - t0;
	if (dt > 200000)
		klog("routes: SLOW %s: %u ms (pid 0x%08X, paused %d)", what, (unsigned)(dt / 1000),
		    (unsigned)ksceKernelGetProcessId(), g_st.paused);
}

static uint32_t
send_route(int s, int type, const struct rt_ent *e)
{
	uint8_t msg[RTM_HDR + 3 * sizeof(SockaddrIn)];
	uint32_t seq = next_seq();
	int n = build(msg, type, e->flags, RTA_DST | RTA_GATEWAY | RTA_NETMASK,
	    seq, e->dst, e->gw, e->mask);
	ph(type == RTM_ADD ? "route add" : "route delete");
	SceUInt64 t0 = ksceKernelGetSystemTimeWide();
	g_last_send_ret = ksceNetSendto(s, msg, n, 0, NULL, 0);
	slow_note(type == RTM_ADD ? "route add" : "route delete", t0);
	return seq;
}

/* ---- RTM_GET ------------------------------------------------------------- */

struct rtinfo {
	int32_t  flags;
	uint32_t v[AX_MAX];
	uint8_t  have[AX_MAX];
};

/* Read the reply to message `seq` (route_output answers synchronously, so it
 * is queued by the time sendto returns; a few short non-blocking tries cover
 * any delay). The socket also receives other broadcasts; they are skipped.
 * Returns the reply's length in kbuf, or 0. Never blocks (the socket receive
 * timeout is not honoured in a lent thread). */
static int
read_reply(int s, uint32_t seq, int type, void *ubuf, uint32_t ubuf_size, uint8_t *kbuf, int kcap)
{
	int tries;

	if (!ubuf || ubuf_size < RTM_HDR)
		return 0;
	if (ubuf_size > (uint32_t)kcap)
		ubuf_size = (uint32_t)kcap;
	ph("route reply recv");
	for (tries = 0; tries < 24; tries++) {
		int n = ksceNetRecvfrom(s, ubuf, ubuf_size, MSG_DONTWAIT_, NULL, NULL);
		if (n == EAGAIN_ || n == 0) {
			if (tries >= 8)
				break;
			ksceKernelDelayThread(1000);
			continue;
		}
		if (n < RTM_HDR)
			break;
		n = rt_copyin(kbuf, n, ubuf, n, RTM_HDR);
		if (n == 0)
			continue;                   /* dropped, never parsed */
		if (kbuf[3] == (uint8_t)type && (uint32_t)rd32ne(kbuf + 0x14) == seq) {
			int32_t pid = rd32ne(kbuf + 0x10);
			if (!g_rt_pid_known) {
				g_rt_pid_known = 1;
				g_rt_pid = pid;
				klog("routes: replies carry rtm_pid 0x%08X", (unsigned)pid);
			} else if (pid != g_rt_pid) {
				static unsigned told;
				if (told++ < 5)
					klog("routes: reply with rtm_pid 0x%08X, not 0x%08X: dropped",
					    (unsigned)pid, (unsigned)g_rt_pid);
				continue;
			}
			return n;
		}
	}
	return 0;
}

/* RTM_GET dst (with a netmask when with_mask). 0 and *ri filled, or -1. */
static int
rt_get(int s, uint32_t dst, int with_mask, uint32_t mask, void *ubuf, uint32_t ubuf_size,
    struct rtinfo *ri)
{
	uint8_t msg[RTM_HDR + 3 * sizeof(SockaddrIn)];
	static uint8_t reply[0x100];    /* static: holder only (see g_des) */
	int pass;

	for (pass = 0; pass < 2; pass++) {
		/* First ask for the interface address too; if the stack refuses
		 * that, ask plainly. */
		int addrs = RTA_DST | (with_mask ? RTA_NETMASK : 0) | (pass == 0 ? RTA_IFP | RTA_IFA : 0);
		uint32_t seq = next_seq();
		int n = build(msg, RTM_GET, RTF_UP, addrs, seq, dst, 0, mask);
		ph("route get");
		if (ksceNetSendto(s, msg, n, 0, NULL, 0) < 0)
			return -1;
		n = read_reply(s, seq, RTM_GET, ubuf, ubuf_size, reply, sizeof(reply));
		if (n == 0)
			continue;
		if (rd32ne(reply + 0x18) != 0)
			continue;                   /* rtm_errno: no route, or refused */
		ri->flags = rd32ne(reply + 0x08);
		parse_addrs(reply, n, RTM_HDR, (uint32_t)rd32ne(reply + 0x0c), ri->v, ri->have);
		return 0;
	}
	return -1;
}

/* A reply that is netctl's default route (or another gateway route on the
 * Wi-Fi), not one of ours. */
static int
is_wifi_gateway_route(const struct rtinfo *ri)
{
	return (ri->flags & RTF_GATEWAY) && ri->have[AX_GATEWAY] == 1 &&
	    ri->v[AX_GATEWAY] != 0 && ri->v[AX_GATEWAY] != g_st.tunnel_address &&
	    !(ri->have[AX_IFA] == 1 && ri->v[AX_IFA] == g_st.tunnel_address);
}

/* ---- discovery ------------------------------------------------------------ */

static void default_giveup(const char *why);

static void
publish_wifi(void)
{
	if (g_st.wlan_address != g_wlan) {
		strand_disarm("the Wi-Fi's address changed");   /* its own connections are alive */
		if (g_wlan)                 /* forgotten at a connect (0) is not a change of network */
			default_giveup("the Wi-Fi's address changed");
	}
	g_st.wlan_gateway = g_gw;
	g_st.wlan_address = g_wlan;
	g_st.wlan_prefix = g_sub_prefix;
}

static void
forget_wifi(void)
{
	g_gw = g_wlan = 0;
	g_sub_prefix = 0;
	g_rediscover = 0;
	g_next_discover = g_next_subnet = 0;
	publish_wifi();
}

static int has_kind(int kind);
static int tunnel_covers(uint32_t ip);

/*
 * Learn the gateway and the Wi-Fi address, by the first that answers:
 *   1. RTM_GET of the endpoint, when neither our endpoint /32 nor a tunnel
 *      route covers it (then netctl's default route answers);
 *   2. RTM_GET of 0.0.0.0 with netmask 0: the default route by its mask;
 *   3. the gateway of the last default route netctl added (monitor).
 * After a change the old endpoint route is kept until one of these gives
 * the gateway (deleting it first would send the handshake itself into a
 * held full tunnel, where it is dropped); if none does after four tries,
 * the old gateway is kept (the usual wake: same network).
 */
static void
relearn(int s, void *ubuf, uint32_t ubuf_size)
{
	struct rtinfo ri;
	uint32_t old_gw = g_gw, old_wlan = g_wlan, gw = 0, ifa = 0;
	int how = 0;

	if (g_st.endpoint_ip && !has_kind(K_EP) && !tunnel_covers(g_st.endpoint_ip) &&
	    rt_get(s, g_st.endpoint_ip, 0, 0, ubuf, ubuf_size, &ri) == 0 &&
	    is_wifi_gateway_route(&ri))
		how = 1;
	else if (rt_get(s, 0, 1, 0, ubuf, ubuf_size, &ri) == 0 && is_wifi_gateway_route(&ri) &&
	    ri.have[AX_NETMASK] == 1 && ri.v[AX_NETMASK] == 0)
		how = 2;
	if (how) {
		gw = ri.v[AX_GATEWAY];
		ifa = ri.have[AX_IFA] == 1 ? ri.v[AX_IFA] : 0;
	} else if (g_mon_gw) {
		gw = g_mon_gw;
		how = 3;
	}
	if (!how) {
		if (g_rediscover && g_gw && ++g_relearn_tries >= 4) {
			g_rediscover = 0;
			klog("routes: gateway could not be re-learnt, keeping 0x%08X", (unsigned)g_gw);
		}
		return;
	}
	g_gw = gw;
	g_wlan = ifa ? ifa : g_mon_wlan ? g_mon_wlan : (gw == old_gw ? old_wlan : 0);
	g_rediscover = 0;
	if (g_gw != old_gw || g_wlan != old_wlan)
		g_sub_prefix = 0;           /* another network: learn its subnet again */
	publish_wifi();
	klog("routes: gateway 0x%08X%s, Wi-Fi address %s (%s)", (unsigned)g_gw,
	    old_gw && g_gw != old_gw ? " (changed)" : "", g_wlan ? "known" : "not reported",
	    how == 1 ? "endpoint lookup" : how == 2 ? "default route" : "netctl's route message");
}

/* Learn the Wi-Fi subnet: see the file comment. */
static void
discover_subnet(int s, void *ubuf, uint32_t ubuf_size)
{
	struct rtinfo ri;
	uint32_t a = g_wlan, gw = g_gw, cm, probe, mask;
	int c, p;

	if (!a || !gw || a == gw)
		return;
	c = __builtin_clz(a ^ gw);          /* leading bits the two share */
	if (c < 8)
		return;
	cm = prefix_mask((unsigned)c);
	probe = a | ~cm;                    /* the shared block's last address */
	if (probe == a || probe == gw)
		probe = a & cm;
	if (probe == a || probe == gw)
		probe = (a & cm) ^ (1u << (32 - c));
	if (rt_get(s, probe, 0, 0, ubuf, ubuf_size, &ri) != 0)
		return;
	if ((ri.flags & (RTF_GATEWAY | RTF_HOST)) || ri.have[AX_NETMASK] != 1)
		return;
	if (ri.have[AX_IFA] != 1 || ri.v[AX_IFA] != a)
		return;                     /* not the Wi-Fi's own subnet route */
	mask = ri.v[AX_NETMASK];
	p = mask_prefix(mask);
	if (p < 8 || p > 29 || p > c || (a & mask) != (gw & mask))
		return;
	g_sub_prefix = (uint8_t)p;
	publish_wifi();
	klog("routes: Wi-Fi subnet /%d", p);
}

/* ---- the desired set ------------------------------------------------------ */

static int
tunnel_covers(uint32_t ip)
{
	int i;
	for (i = 0; i < g_napp; i++)
		if ((g_app[i].kind == K_TUN || g_app[i].kind == K_LOCAL || g_app[i].kind == K_DNS ||
		    g_app[i].kind == K_BLK) && (ip & g_app[i].mask) == g_app[i].dst)
			return 1;
	return 0;
}

static int
has_kind(int kind)
{
	int i;
	for (i = 0; i < g_napp; i++)
		if (g_app[i].kind == kind)
			return 1;
	return 0;
}

static const struct rt_ent *
find_kind(int kind)
{
	int i;
	for (i = 0; i < g_napp; i++)
		if (g_app[i].kind == kind)
			return &g_app[i];
	return NULL;
}

static void
put(struct rt_ent *d, int *n, int kind, uint32_t dst, uint32_t mask, uint32_t gw, uint16_t flags, uint32_t gen)
{
	if (*n >= RT_MAX)
		return;
	memset(&d[*n], 0, sizeof(d[0]));
	d[*n].dst = dst & mask;
	d[*n].mask = mask;
	d[*n].gw = gw;
	d[*n].flags = flags;
	d[*n].kind = (uint8_t)kind;
	d[*n].gen = gen;
	(*n)++;
}

/* Copy the applied entries other than the endpoint (and roam) routes
 * unchanged (a hold). */
static void
keep_held(struct rt_ent *d, int *n)
{
	int i;
	for (i = 0; i < g_napp && *n < RT_MAX; i++)
		if (g_app[i].kind != K_EP && g_app[i].kind != K_ROAM)
			d[(*n)++] = g_app[i];
}

/* 0/1 + 128/1 into the interface: a full tunnel, or anything under
 * Lockdown (traffic outside AllowedIPs is then dropped, not sent direct). */
static int
blanket(void)
{
	return (g_st.full_tunnel && !(g_st.flags & VT_PROFILE_NO_ROUTES)) || g_st.lockdown;
}

static int
local_wanted(void)
{
	return blanket() && !g_st.allow_local;
}

/*
 * The profile's DNS servers (DNS =) when they sit in the Wi-Fi subnet and
 * allow local is on. netctl's subnet route is more
 * specific than 0/1 + 128/1, so without this the queries to them would go
 * straight onto the LAN, in plaintext, while the tunnel is up (and while it
 * is held). A profile's DNS = inside its AllowedIPs means "the resolver at
 * the far end" - the official clients add it back to the routes when they
 * exclude private addresses for the same reason - so it goes into the
 * tunnel: a /31 (a /32 interface route does not clone, so it would get no
 * ARP entry; see the gateway /31), costing its /31 neighbour direct access.
 *
 * Not done (the server stays reached directly), each a documented case:
 *   - it is the Wi-Fi gateway, the console's own address, or either is its
 *     /31 neighbour: the gateway is the endpoint route's next hop and must
 *     stay on the Wi-Fi. This is the home-router case (DNS = the router,
 *     192.0.2.1, with the console on that same LAN): direct to the router
 *     the profile names anyway;
 *   - allow local off: the subnet halves already take it into the tunnel
 *     (only the gateway's /31 stays direct: the case above);
 *   - the subnet is not known yet (the next step adds it once it is);
 *   - it is outside AllowedIPs (then it never belonged to the tunnel).
 * Only while the tunnel is wanted: with it off (Lockdown) resolvers are
 * given no server (dns.c).
 */
static void
put_dns_set(struct rt_ent *d, int *n, uint32_t gen)
{
	uint32_t dns[2], sub;
	int i;

	if (!TN_DNS_ROUTE || !g_st.want || !blanket() || local_wanted() ||
	    !g_sub_prefix || !g_gw || !g_wlan)
		return;
	dns_profile(dns);
	sub = prefix_mask(g_sub_prefix);
	for (i = 0; i < 2; i++) {
		uint32_t a = dns[i], pair = a & ~1u;
		if (!a || (a & sub) != (g_wlan & sub) || !tn_allowed_cfg(a))   /* never waits */
			continue;
		if (pair == (g_gw & ~1u) || pair == (g_wlan & ~1u))
			continue;
		if (i == 1 && pair == (dns[0] & ~1u))
			continue;                   /* the same /31 as the first */
		put(d, n, K_DNS, pair, 0xfffffffeu, g_st.tunnel_address, RTF_UP | RTF_STATIC, gen);
	}
}

/* The tunnel side: the tunnel routes and, with them, the broadcast /32 and
 * (allow local off) the gateway /31 and the subnet halves. */
static void
put_tunnel_set(struct rt_ent *d, int *n, uint32_t gen)
{
	int local = local_wanted() && g_sub_prefix && g_wlan;
	uint32_t ta = g_st.tunnel_address;
	unsigned i;

	if (local)
		put(d, n, K_GWNET, g_gw, 0xfffffffeu, g_wlan, RTF_UP | RTF_STATIC, 0);
	if (TN_BCAST_ROUTE && blanket() && g_wlan && g_bcast_refused < 3)
		put(d, n, K_BCAST, 0xffffffffu, 0xffffffffu, g_wlan, RTF_UP | RTF_STATIC, 0);
	if (blanket()) {
		put(d, n, K_TUN, 0x00000000u, 0x80000000u, ta, RTF_UP | RTF_STATIC, gen);
		put(d, n, K_TUN, 0x80000000u, 0x80000000u, ta, RTF_UP | RTF_STATIC, gen);
	} else {
		for (i = 0; i < g_st.allowed_count; i++)
			put(d, n, K_TUN, g_st.allowed[i].net, prefix_mask(g_st.allowed[i].prefix),
			    ta, RTF_UP | RTF_STATIC, gen);
	}
	if (local) {
		unsigned p = g_sub_prefix;
		uint32_t net = g_wlan & prefix_mask(p);
		put(d, n, K_LOCAL, net, prefix_mask(p + 1), ta, RTF_UP | RTF_STATIC, gen);
		put(d, n, K_LOCAL, net | (1u << (31 - p)), prefix_mask(p + 1), ta,
		    RTF_UP | RTF_STATIC, gen);
	}
	put_dns_set(d, n, gen);
}

/* THE BLOCK (file comment): the tunnel set as reject routes via lo0. */
static void
put_block_set(struct rt_ent *d, int *n)
{
	int local = local_wanted() && g_sub_prefix && g_wlan;
	const uint16_t f = RTF_UP | RTF_STATIC | RTF_REJECT;
	unsigned i;

	if (local)
		put(d, n, K_GWNET, g_gw, 0xfffffffeu, g_wlan, RTF_UP | RTF_STATIC, 0);
	if (TN_BCAST_ROUTE && blanket() && g_wlan && g_bcast_refused < 3)
		put(d, n, K_BCAST, 0xffffffffu, 0xffffffffu, g_wlan, RTF_UP | RTF_STATIC, 0);
	if (blanket()) {
		put(d, n, K_BLK, 0x00000000u, 0x80000000u, LO0_ADDR, f, 0);
		put(d, n, K_BLK, 0x80000000u, 0x80000000u, LO0_ADDR, f, 0);
	} else {
		for (i = 0; i < g_st.allowed_count; i++)
			put(d, n, K_BLK, g_st.allowed[i].net, prefix_mask(g_st.allowed[i].prefix),
			    LO0_ADDR, f, 0);
	}
	if (local) {
		unsigned p = g_sub_prefix;
		uint32_t net = g_wlan & prefix_mask(p);
		put(d, n, K_BLK, net, prefix_mask(p + 1), LO0_ADDR, f, 0);
		put(d, n, K_BLK, net | (1u << (31 - p)), prefix_mask(p + 1), LO0_ADDR, f, 0);
	}
}

/* The block can stand in: the Wi-Fi side is known (a blanket set needs the
 * Wi-Fi address for the broadcast route) and it was not given up. */
static int
block_possible(void)
{
	return g_blk_refused < 3 && g_gw && (g_wlan || !blanket());
}

static int
build_desired(struct rt_ent *d)
{
	struct tnp_stats st;
	int n = 0, ep, want_tun;
	uint32_t gen = g_st.iface_gen;

	if (!g_st.configured || g_st.paused)
		return 0;                           /* a pause lets traffic out */
	if (!g_st.want) {
		/* Disconnect restores everything, kill switch or
		 * not. Lockdown: hold (no endpoint route; see the file comment).
		 * The broadcast route is what keeps DHCP on the Wi-Fi, so the hold
		 * is built only with the Wi-Fi address known. */
		if (!g_st.lockdown)
			return 0;
		if (g_gw && g_wlan && g_st.iface_up && g_st.tunnel_address)
			put_tunnel_set(d, &n, gen);
		else if (!has_kind(K_TUN) && block_possible())
			put_block_set(d, &n);       /* THE BLOCK: before the interface is up */
		else
			keep_held(d, &n);
		return n;
	}

	tnp_get_stats(&st);
	if (st.state == TNP_UP)
		g_st.ever_up = 1;

	ep = g_st.endpoint_ip != 0 && g_gw != 0;
	if (ep) {
		uint32_t roam = transport_roam_ip();
		put(d, &n, K_EP, g_st.endpoint_ip, 0xffffffffu, g_gw,
		    RTF_UP | RTF_GATEWAY | RTF_STATIC, 0);
		if (roam && roam != g_st.endpoint_ip)
			put(d, &n, K_ROAM, roam, 0xffffffffu, g_gw,
			    RTF_UP | RTF_GATEWAY | RTF_STATIC, 0);
	}
	if ((g_st.flags & VT_PROFILE_NO_ROUTES) && !g_st.lockdown)
		return n;                           /* Table = off */

	want_tun = st.state == TNP_UP || (st.state == TNP_HANDSHAKING && g_st.ever_up) ||
	    g_st.locked || TN_KILL();
	if (!want_tun)
		return n;                           /* kill switch off, not up: direct */

	if (ep && g_st.iface_up && g_st.tunnel_address) {
		put_tunnel_set(d, &n, gen);
	} else if (g_st.locked || TN_KILL()) {
		/* Held, but cannot (re)build right now: no gateway yet after a
		 * change, or the interface is waiting for its (new) address.
		 * Nothing of the tunnel's in place yet (a connect): THE BLOCK,
		 * once the endpoint route can go first. Not for a hostname
		 * endpoint not resolved yet: its lookup would be rejected too
		 * (the system's resolver is rarely on the LAN), which with the
		 * kill switch alone used to work; the window stays for it. */
		if (!has_kind(K_TUN) && block_possible() && ep)
			put_block_set(d, &n);
		else
			keep_held(d, &n);
	}
	return n;
}

/* ---- reconcile ------------------------------------------------------------ */

static int
same(const struct rt_ent *a, const struct rt_ent *b)
{
	return a->dst == b->dst && a->mask == b->mask && a->gw == b->gw &&
	    a->flags == b->flags && a->kind == b->kind && a->gen == b->gen;
}

static int
find_in(const struct rt_ent *t, int n, const struct rt_ent *e, int exact)
{
	int i;
	for (i = 0; i < n; i++)
		if (exact ? same(&t[i], e) : (t[i].dst == e->dst && t[i].mask == e->mask))
			return i;
	return -1;
}

static void
app_drop(int i)
{
	if (i < 0 || i >= g_napp)
		return;
	memmove(&g_app[i], &g_app[i + 1], (unsigned)(g_napp - i - 1) * sizeof(g_app[0]));
	g_napp--;
}

/* ---- the LAN purge: state and tests (file comment) ------------------------ */

#define LAN_TRACK 32             /* LAN hosts the monitor saw on the Wi-Fi */
#define LAN_NEW   8              /* entries seen appearing, to delete next */

enum { LP_IDLE = 0, LP_DUMP, LP_SWEEP };
static volatile int g_lp_arm;            /* the halves were just added */
static int g_lp;                         /* LP_*; owned with g_owner */
static uint32_t g_lp_cur;                /* sweep cursor: an address, or a g_lan_seen index */
static unsigned g_lp_dumps, g_lp_del, g_lp_seen, g_lp_fail;
static SceUInt64 g_hc_next;                /* the host census (debug_routes) */
static volatile int g_hc_due;
static const char *g_lp_how;
static SceUInt64 g_lp_t0;
static uint32_t g_lan_seen[LAN_TRACK];
static unsigned g_lan_w;                 /* ring write position */
static uint32_t g_lan_new[LAN_NEW];
static int g_lan_new_n;
static unsigned g_lan_appeared;          /* appeared with the halves in place */

/* An address the purge may delete a Wi-Fi host entry for: in the Wi-Fi
 * subnet, not the console, not the gateway or its /31 neighbour (direct by
 * design, K_GWNET), not the network or broadcast address. */
static int
lan_candidate(uint32_t ip)
{
	uint32_t m;

	if (!g_sub_prefix || !g_wlan || !g_gw)
		return 0;
	m = prefix_mask(g_sub_prefix);
	return (ip & m) == (g_wlan & m) && ip != g_wlan && (ip & ~1u) != (g_gw & ~1u) &&
	    (ip & ~m) != 0 && (ip & ~m) != ~m && ip != g_st.tunnel_address;
}

/* A subnet half (the tunnel route or its REJECT stand-in). */
static int
lan_half(const struct rt_ent *e)
{
	uint32_t m;

	if (!g_sub_prefix || !g_wlan || (e->kind != K_LOCAL && e->kind != K_BLK))
		return 0;
	m = prefix_mask(g_sub_prefix);
	return e->mask == prefix_mask(g_sub_prefix + 1u) && (e->dst & m) == (g_wlan & m);
}

/* Allow local off and the halves in place: the only time the purge may
 * touch a route. */
static int
lan_guard(void)
{
	int i;

	if (!active() || !local_wanted() || !g_sub_prefix || !g_wlan || !g_gw)
		return 0;
	for (i = 0; i < g_napp; i++)
		if (lan_half(&g_app[i]))
			return 1;
	return 0;
}

static void
lan_track(uint32_t ip)
{
	unsigned i;

	for (i = 0; i < LAN_TRACK; i++)
		if (g_lan_seen[i] == ip)
			return;
	g_lan_seen[g_lan_w++ % LAN_TRACK] = ip;
}

static int
needs_work(const struct rt_ent *d, int nd, int removals_only)
{
	int i;
	for (i = 0; i < g_napp; i++)
		if (find_in(d, nd, &g_app[i], 1) < 0 &&
		    !(removals_only && find_in(d, nd, &g_app[i], 0) >= 0))
			return 1;
	if (removals_only)
		return 0;
	for (i = 0; i < nd; i++)
		if (find_in(g_app, g_napp, &d[i], 1) < 0)
			return 1;
	return 0;
}

static const char *const k_kind[] = { "?", "endpoint", "gateway", "tunnel", "local", "broadcast", "dns",
	"roam", "block" };

/*
 * FLUSH OF REMEMBERED ROUTES. Each open TCP connection keeps the route it was
 * given. One opened while the tunnel routes were away (before a connect,
 * during a pause) keeps the Wi-Fi default route and, since that route stays
 * valid when the tunnel's halves are added, goes on sending over the Wi-Fi
 * with the Wi-Fi address while the tunnel is up (no frame reaches the
 * tunnel after a resume while pages load with the home address). Deleting the default route and adding it straight
 * back, unchanged, marks the remembered route as gone: every such connection
 * looks its route up again, finds the tunnel, and its next segment reaches
 * the tunnel interface with the Wi-Fi source, where it is answered with a
 * reset (iface.c) and reconnects through the tunnel.
 *
 * Done when tunnel routes or the block's reject routes are added. With a full
 * tunnel the halves cover every address, so the moment without a default
 * route changes nothing. With AllowedIPs they do not: traffic to an address
 * outside the ranges has no route for the time two PF_ROUTE messages take.
 * The refresh is kept for split tunnels all the same: connections to
 * addresses inside the ranges need it.
 *
 * Guarded: only a default whose gateway is the one this module learnt is
 * refreshed; the delete's echo is read, and "refreshed" is claimed only when
 * the delete succeeded and a read shows the route back. A route that does not
 * come back is added again in later steps (g_def_gw) until it is (every
 * 250 ms for ten seconds, then every 2 s, for two minutes at most; a Wi-Fi
 * address that is removed or changes, or a lost network, ends it at once,
 * since the Wi-Fi sets its own default route when it reconnects), and a
 * disconnect or pause checks the route exists (g_def_check), so the console
 * is never left without a default route.
 */
#define DEF_FAST_US   (250 * 1000)          /* retry pace for the first ten seconds */
#define DEF_SLOW_US   (2 * 1000 * 1000)     /* then this, */
#define DEF_FAST_FOR  (10ull * 1000 * 1000)
#define DEF_GIVE_UP   (120ull * 1000 * 1000) /* until this long after the first retry */
static unsigned g_flush_n;
static volatile uint32_t g_def_gw;       /* a default route via this gateway is to be put back */
static SceUInt64 g_def_next;             /* no retry before this */
static SceUInt64 g_def_start;            /* when the retries began */
static unsigned g_def_tries;
static volatile int g_def_check;         /* a disconnect or pause: see that the default route exists */

static int
default_route(int s, void *ubuf, uint32_t ubuf_size, uint32_t *gw)
{
	struct rtinfo ri;

	if (rt_get(s, 0, 1, 0, ubuf, ubuf_size, &ri) != 0)
		return 0;
	if (!(ri.flags & RTF_GATEWAY) || ri.have[AX_GATEWAY] != 1 || !ri.v[AX_GATEWAY] ||
	    (ri.have[AX_DST] == 1 && ri.v[AX_DST] != 0) ||
	    (ri.have[AX_NETMASK] == 1 && ri.v[AX_NETMASK] != 0) ||
	    ri.v[AX_GATEWAY] == g_st.tunnel_address ||
	    (ri.have[AX_IFA] == 1 && ri.v[AX_IFA] == g_st.tunnel_address))
		return 0;
	*gw = ri.v[AX_GATEWAY];
	return 1;
}

/* One default-route message, its echo read. 1 and *err set (the rtm_errno), or 0:
 * no echo seen. */
static int
default_msg(int s, int type, uint32_t gw, void *ubuf, uint32_t ubuf_size, int32_t *err)
{
	static uint8_t echo[0x100];            /* static: the route-owning thread only (see g_des) */
	struct rt_ent e;
	uint32_t seq;
	int n;

	memset(&e, 0, sizeof(e));
	e.gw = gw;
	e.flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	seq = send_route(s, type, &e);
	n = read_reply(s, seq, type, ubuf, ubuf_size, echo, sizeof(echo));
	if (n < RTM_HDR)
		return 0;
	*err = rd32ne(echo + 0x18);
	return 1;
}

/* Add the default route via gw (a few tries) until a read shows it. */
static int
default_put_back(int s, void *ubuf, uint32_t ubuf_size, uint32_t gw)
{
	uint32_t back = 0;
	int tries;

	for (tries = 0; tries < 3; tries++) {
		int32_t err = 0;
		if (default_msg(s, RTM_ADD, gw, ubuf, ubuf_size, &err) && err != 0 && err != EEXIST_)
			klog("routes: default route add refused (errno %d)", (int)err);
		if (default_route(s, ubuf, ubuf_size, &back) && back == gw)
			return 1;
	}
	return 0;
}

static void
default_pending(uint32_t gw, SceUInt64 now)
{
	g_def_gw = gw;
	g_def_tries = 0;
	g_def_start = now;
	g_def_next = now + DEF_FAST_US;
	tn_wake();
}

/* Stop putting the default route back (any thread; it only clears). With the
 * tunnel off the module's cost must be nothing, so a retry never runs on: a
 * Wi-Fi that lost or changed its address, or a lost network, sets its own
 * default route at the next association. */
static void
default_giveup(const char *why)
{
	uint32_t gw = __atomic_exchange_n(&g_def_gw, 0, __ATOMIC_ACQ_REL);

	if (!gw)
		return;
	klog("routes: giving up: the default route via %u.%u.%u.%u did not come back; the Wi-Fi's own reconnect will set it (%s)",
	    IP4(gw), why);
}

static void
flush_remembered_routes(int s, void *ubuf, uint32_t ubuf_size)
{
	uint32_t gw = 0;
	int32_t err = 0;
	int got, deleted;

	if (!g_gw) {
		klog("routes: flush: the Wi-Fi gateway is not known: the default route is left alone");
		return;
	}
	if (!default_route(s, ubuf, ubuf_size, &gw)) {
		klog("routes: flush: no Wi-Fi default route to refresh; open connections keep their route");
		return;
	}
	if (gw != g_gw) {
		klog("routes: flush: the default route goes via %u.%u.%u.%u, not the learnt gateway %u.%u.%u.%u: left alone",
		    IP4(gw), IP4(g_gw));
		return;
	}
	got = default_msg(s, RTM_DELETE, gw, ubuf, ubuf_size, &err);
	deleted = got && err == 0;
	if (got && err != 0) {
		klog("routes: flush: the default route delete was refused (errno %d): left as it is", (int)err);
		return;
	}
	if (default_put_back(s, ubuf, ubuf_size, gw)) {
		if (deleted) {
			g_flush_n++;
			klog("routes: flush: default route via %u.%u.%u.%u refreshed, open connections look their route up again (#%u)",
			    IP4(gw), g_flush_n);
		} else {
			klog("routes: flush: the delete was not confirmed; the default route via %u.%u.%u.%u is in place, not known to be refreshed",
			    IP4(gw));
		}
		return;
	}
	klog("routes: flush: the default route via %u.%u.%u.%u did NOT come back: retrying in later steps", IP4(gw));
	default_pending(gw, now_us());
}

/* A later step: put the default route back (g_def_gw), or see after a
 * disconnect or pause that it is there (g_def_check). */
static void
default_step(int s, void *ubuf, uint32_t ubuf_size, SceUInt64 now)
{
	uint32_t gw = 0;
	int check = __atomic_exchange_n(&g_def_check, 0, __ATOMIC_ACQ_REL);

	/* A default route that is there ends it, whoever put it (netctl on
	 * another network may have): never one via a stale gateway on top. */
	if (default_route(s, ubuf, ubuf_size, &gw)) {
		if (g_def_gw)
			klog("routes: a default route via %u.%u.%u.%u is in place (after %u retries)", IP4(gw), g_def_tries);
		else
			klog("routes: the default route via %u.%u.%u.%u is in place", IP4(gw));
		g_def_gw = 0;
		return;
	}
	if (g_def_gw && g_gw && g_def_gw != g_gw) {
		klog("routes: the gateway changed (%u.%u.%u.%u): the default route is put back via it instead",
		    IP4(g_gw));
		g_def_gw = g_gw;
	}
	if (!g_def_gw && check) {
		if (!g_gw) {
			klog("routes: the default route is missing and the gateway is not known");
			return;
		}
		klog("routes: the default route is missing: adding it back via %u.%u.%u.%u", IP4(g_gw));
		g_def_gw = g_gw;
		g_def_tries = 0;
		g_def_start = now;
	}
	if (!g_def_gw)
		return;
	if (default_put_back(s, ubuf, ubuf_size, g_def_gw)) {
		klog("routes: the default route via %u.%u.%u.%u is back (after %u retries)", IP4(g_def_gw), g_def_tries);
		g_def_gw = 0;
		return;
	}
	g_def_tries++;
	if (now - g_def_start >= DEF_GIVE_UP)
		default_giveup("2 minutes");
	else
		g_def_next = now + (now - g_def_start < DEF_FAST_FOR ? DEF_FAST_US : DEF_SLOW_US);
}

/*
 * Bring g_app to d. removals_only (a syscall context): delete what is no
 * longer wanted, but leave a route that is only being replaced (same
 * destination, new gateway) to the RX thread, which swaps it back to back.
 */
static void
reconcile(int s, const struct rt_ent *d, int nd, int removals_only, void *ubuf, uint32_t ubuf_size)
{
	static const int order[] = { K_DNS, K_LOCAL, K_TUN, K_BLK, K_GWNET, K_BCAST, K_ROAM, K_EP };
	int i, k, added = 0, removed = 0, tun_added = 0, blk_added = 0;
	uint32_t tun_gone = 0;             /* the tunnel address of tunnel routes removed */
	SceUInt64 now = now_us();

	if (!removals_only && now >= g_add_hold_until) {
		for (i = 0; i < nd; i++) {
			struct rt_ent e = d[i];
			int c;
			if (find_in(g_app, g_napp, &e, 1) >= 0)
				continue;
			c = find_in(g_app, g_napp, &e, 0);
			if (c >= 0) {               /* same destination, new gateway */
				send_route(s, RTM_DELETE, &g_app[c]);
				app_drop(c);
				removed++;
			}
			if (g_napp >= RT_MAX)
				break;
			e.seq = send_route(s, RTM_ADD, &e);
			g_app[g_napp++] = e;
			added++;
			if (lan_half(&e) && local_wanted())
				g_lp_arm = 1;       /* the LAN purge, later this step */
			if (e.kind == K_TUN) {
				if (restore_debug_on())
					g_hc_due = 1;   /* the host census */
				tun_added = 1;
			}
			if (e.kind == K_BLK)
				blk_added = 1;
		}
	}
	for (k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++) {
		for (i = g_napp - 1; i >= 0; i--) {
			if (g_app[i].kind != order[k] || find_in(d, nd, &g_app[i], 1) >= 0)
				continue;
			if (removals_only && find_in(d, nd, &g_app[i], 0) >= 0)
				continue;
			send_route(s, RTM_DELETE, &g_app[i]);
			if (g_app[i].kind == K_TUN)
				tun_gone = g_app[i].gw;
			if (g_app[i].kind == K_TUN || g_app[i].kind == K_LOCAL || g_app[i].kind == K_DNS) {
				g_sweep_ta = g_app[i].gw;   /* the tunnel address it cloned under */
				g_sweep = 1;
			}
			app_drop(i);
			removed++;
		}
	}

	/* The kernel echoes each message with rtm_errno (SO_USELOOPBACK): an
	 * addition it refused (other than "already there") is forgotten and
	 * retried after a pause. Only readable on the lent RX thread. */
	if (added && ubuf && ubuf_size >= RTM_HDR) {
		uint8_t hdr[RTM_HDR];
		int tries;
		ph("route echo recv");
		for (tries = 0; tries < 2 * RT_MAX; tries++) {
			int n = ksceNetRecvfrom(s, ubuf, ubuf_size < 0x100 ? ubuf_size : 0x100,
			    MSG_DONTWAIT_, NULL, NULL);
			int32_t err;
			uint32_t seq;
			if (n < RTM_HDR)
				break;
			if (!rt_copyin(hdr, sizeof(hdr), ubuf, n, RTM_HDR))
				continue;
			if (hdr[3] != RTM_ADD)
				continue;
			err = rd32ne(hdr + 0x18);
			seq = (uint32_t)rd32ne(hdr + 0x14);
			if (err == 0 || err == EEXIST_)
				continue;
			for (i = 0; i < g_napp; i++)
				if (g_app[i].seq == seq) {
					klog("routes: %s route refused (errno %d), retrying in 2 s",
					    k_kind[g_app[i].kind], (int)err);
					if (g_app[i].kind == K_BCAST && ++g_bcast_refused >= 3)
						klog("routes: broadcast route given up for this connection");
					if (g_app[i].kind == K_BLK && ++g_blk_refused >= 3)
						klog("routes: reject routes refused: the block before the tunnel "
						    "routes is given up for this connection");
					app_drop(i);
					g_add_hold_until = now + US(2);
					break;
				}
		}
	}
	/* Connections the tunnel carried now send over the Wi-Fi with its
	 * address: the stranded-connection reset (strand.h) watches for them
	 * until the tunnel is back (10 minutes at most). Tunnel routes back (or
	 * never gone) end it. */
	if (tun_added)
		strand_disarm("tunnel routes back");
	else if (tun_gone && !has_kind(K_TUN))
		strand_arm(tun_gone, g_wlan ? g_wlan : g_st.wlan_address);
	/* Connections that remembered the Wi-Fi default route look it up again
	 * (only where a reply can be read: the lent RX thread). */
	if ((tun_added || blk_added) && ubuf)
		flush_remembered_routes(s, ubuf, ubuf_size);
	if (added || removed) {
		klog("routes: +%d -%d (endpoint %d, tunnel %d, local %d)", added, removed,
		    has_kind(K_EP), has_kind(K_TUN), has_kind(K_LOCAL));
		if (has_kind(K_TUN))
			g_st.last_error = g_st.last_error == VT_ERR_ROUTE ? VT_ERR_NONE : g_st.last_error;
	}
}

/*
 * The published copy of the table for routes_iface_for (vtGetRouteFor and
 * the DNS hook, any thread, never waiting). The table itself changes under
 * the ownership flag; readers used to walk it unlocked while reconcile
 * memmoved it. Now the holder copies it here under a sequence count (odd =
 * being written) at the end of every pass, and a reader retries a torn
 * copy a few times, then answers VT_IFACE_NONE (the DNS hook then treats
 * the server as not tunnelled: no server while held, the safe side).
 */
static struct { uint32_t dst, mask; uint8_t kind; } g_pub[RT_MAX];
static volatile uint32_t g_pub_seq;
static int g_pub_n;
static uint32_t g_pub_wlan;
static uint8_t g_pub_sub;

static void
publish_table(void)
{
	int i;

	__atomic_add_fetch(&g_pub_seq, 1, __ATOMIC_ACQ_REL);   /* odd */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < g_napp && i < RT_MAX; i++) {
		g_pub[i].dst = g_app[i].dst;
		g_pub[i].mask = g_app[i].mask;
		g_pub[i].kind = g_app[i].kind;
	}
	g_pub_n = i;
	g_pub_wlan = g_wlan;
	g_pub_sub = g_sub_prefix;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	__atomic_add_fetch(&g_pub_seq, 1, __ATOMIC_ACQ_REL);   /* even */
}

static void
publish_flags(void)
{
	uint8_t f = 0;

	publish_table();
	if (has_kind(K_EP))    f |= VT_RT_ENDPOINT;
	if (has_kind(K_TUN))   f |= VT_RT_TUNNEL;
	if (has_kind(K_BLK))   f |= VT_RT_BLOCK;
	if (has_kind(K_LOCAL)) f |= VT_RT_LOCAL;
	if (has_kind(K_DNS))   f |= VT_RT_DNS;
	if ((g_st.want || TN_LD_HOLD()) && local_wanted() && has_kind(K_TUN) && !has_kind(K_LOCAL))
		f |= VT_RT_LOCAL_UNKNOWN;
	g_st.route_flags = f;
	{
		/* The TX step sends to a roam address only once its /32 is in. */
		const struct rt_ent *r = find_kind(K_ROAM);
		transport_roam_routed(r && r->dst == transport_roam_ip() ? r->dst : 0);
	}
	{
		/* And to the endpoint only while that cannot loop:
		 * when a route of ours covers it (the Lockdown hold's 0/1 +
		 * 128/1, the block, a held tunnel set) its own /32 must be in
		 * place first, or the initiation would be routed into the
		 * tunnel interface (or rejected by the block). */
		uint32_t ep = g_st.endpoint_ip;
		int i, pinned = 0;
		for (i = 0; i < g_napp; i++)
			if ((g_app[i].kind == K_EP || g_app[i].kind == K_ROAM) && g_app[i].dst == ep)
				pinned = 1;
		transport_send_gate(!ep || pinned || !tunnel_covers(ep));
	}
}

/* ---- the change monitor --------------------------------------------------- */

static void
set_changed(const char *why)
{
	g_netchg_why = why;
	g_netchg = 1;
}

static void
monitor_msg(const uint8_t *m, int n)
{
	uint32_t v[AX_MAX];
	uint8_t have[AX_MAX];
	int type = m[3];

	if (m[2] != RTM_VERSION)
		return;
	if (type == RTM_NEWADDR || type == RTM_DELADDR) {
		uint32_t x;
		if (n < IFAM_HDR)
			return;
		parse_addrs(m, n, IFAM_HDR, (uint32_t)rd32ne(m + 4), v, have);
		if (have[AX_IFA] != 1)
			return;
		x = v[AX_IFA];
		if (x != 0 && x == g_st.tunnel_address && type == RTM_NEWADDR)
			iface_addr_added();         /* no desc[0x10] when it stayed UP */
		if (x == 0 || x == g_st.tunnel_address)
			return;                     /* the tunnel interface (the shell sets it) */
		g_mon_count[type == RTM_NEWADDR ? 0 : 1]++;
		if (type == RTM_DELADDR && x == g_wlan) {
			g_mon_wlan = 0;
			strand_disarm("the Wi-Fi's address was removed");
			default_giveup("the Wi-Fi's address was removed");
			set_changed("Wi-Fi address removed");
			net_lost("Wi-Fi address removed");   /* no net_ps call until it is back */
		} else if (type == RTM_NEWADDR && x != g_wlan) {
			g_mon_wlan = x;
			strand_disarm("a new Wi-Fi address");
			default_giveup("a new Wi-Fi address");
			set_changed("new Wi-Fi address");
		}
	} else if (type == RTM_ADD || type == RTM_DELETE || type == RTM_CHANGE) {
		int32_t flags;
		const struct rt_ent *ep;
		if (n < RTM_HDR || ((uint32_t)rd32ne(m + 0x14) & 0xffff0000u) == SEQ_TAG ||
		    rd32ne(m + 0x18) != 0)
			return;                     /* ours, or refused */
		g_mon_count[type == RTM_DELETE ? 3 : 2]++;
		flags = rd32ne(m + 0x08);
		parse_addrs(m, n, RTM_HDR, (uint32_t)rd32ne(m + 0x0c), v, have);
		if (type == RTM_ADD && (flags & (RTF_HOST | RTF_LLINFO | RTF_GATEWAY)) ==
		    (RTF_HOST | RTF_LLINFO) && have[AX_DST] == 1 && have[AX_IFA] == 1 &&
		    v[AX_IFA] == g_wlan && lan_candidate(v[AX_DST])) {
			/* A LAN host's ARP entry on the Wi-Fi (the LAN purge). */
			lan_track(v[AX_DST]);
			if (lan_guard() && g_lan_new_n < LAN_NEW) {
				g_lan_new[g_lan_new_n++] = v[AX_DST];
				if (g_lan_appeared++ < 10)
					klog("routes: LAN host %u.%u.%u.%u got a Wi-Fi entry with allow local off: deleting",
					    IP4(v[AX_DST]));
			}
		}
		if (have[AX_DST] == 1 && v[AX_DST] == 0 && (flags & RTF_GATEWAY) &&
		    (have[AX_NETMASK] != 1 || v[AX_NETMASK] == 0) &&
		    type != RTM_DELETE && have[AX_GATEWAY] == 1) {
			g_mon_gw = v[AX_GATEWAY];
			if (!g_gw)
				g_next_discover = 0;    /* a default route appeared: try now */
			else if (v[AX_GATEWAY] != g_gw)
				set_changed("new default gateway");
		}
		ep = find_kind(K_EP);
		if (type == RTM_DELETE && ep && have[AX_DST] == 1 && v[AX_DST] == ep->dst) {
			g_ep_lost = 1;
			set_changed("endpoint route removed");
		}
		ep = find_kind(K_ROAM);
		if (type == RTM_DELETE && ep && have[AX_DST] == 1 && v[AX_DST] == ep->dst) {
			g_ep_lost = 1;
			set_changed("roam route removed");
		}
	}
}

/* Read what net_ps broadcast since the last step. The socket belongs to
 * SceShell's process (made on the lent RX thread, like the UDP socket). */
static void
monitor_step(void *ubuf, uint32_t ubuf_size)
{
	uint8_t m[0x100];
	SceUInt64 now = now_us();
	int i;

	if (g_mon_reopen) {
		g_mon_reopen = 0;
		if (g_mon >= 0) {
			sock_close(g_mon);
			g_mon = -1;
			g_mon_retry = 0;
		}
	}
	if (now < g_mon_next)
		return;
	ph("monitor");
	g_mon_next = now + 100 * 1000;      /* ten reads a second is plenty */
	if (g_mon < 0) {
		if (now < g_mon_retry)
			return;
		g_mon = ksceNetSocket("vpnturbo_rtmon", 17 /*PF_ROUTE*/, 3 /*SOCK_RAW*/, 0);
		if (g_mon < 0) {
			klog("routes: monitor socket 0x%08X", (unsigned)g_mon);
			g_mon = -1;
			g_mon_retry = now + US(10);
			return;
		}
		g_socks_open++;
		klog("routes: change monitor up");
	}
	for (i = 0; i < 16; i++) {
		int n = ksceNetRecvfrom(g_mon, ubuf, ubuf_size < sizeof(m) ? ubuf_size : sizeof(m),
		    MSG_DONTWAIT_, NULL, NULL);
		if (n <= 0) {
			if (n != EAGAIN_ && n != 0 && ++g_mon_errs > 20) {
				klog("routes: monitor socket failing (0x%08X), reopening", (unsigned)n);
				sock_close(g_mon);
				g_mon = -1;
				g_mon_errs = 0;
				g_mon_retry = now + US(5);
			}
			break;
		}
		g_mon_errs = 0;
		n = rt_copyin(m, n, ubuf, n, 4);
		if (n == 0)
			continue;
		monitor_msg(m, n);
		if (TN_NO_NETPS())
			break;              /* the Wi-Fi went away, no more net_ps */
	}
}

/* What a lookup of the endpoint gave. */
enum { EPV_OURS = 0, EPV_TUNNEL, EPV_OTHER, EPV_NONE };
static const char *const k_epv[] = { "ours", "the tunnel (loop)", "another route", "no reply" };

static int
ep_verdict(const struct rt_ent *ep, const struct rtinfo *ri)
{
	uint32_t ta = g_st.tunnel_address;

	if (ta && ((ri->have[AX_IFA] == 1 && ri->v[AX_IFA] == ta) ||
	    (ri->have[AX_GATEWAY] == 1 && ri->v[AX_GATEWAY] == ta)))
		return EPV_TUNNEL;
	if (ep && ri->have[AX_DST] == 1 && ri->v[AX_DST] == ep->dst && (ri->flags & RTF_GATEWAY) &&
	    ri->have[AX_GATEWAY] == 1 && ri->v[AX_GATEWAY] == ep->gw)
		return EPV_OURS;
	return EPV_OTHER;
}

/*
 * The periodic check: every 10 s while UP, every 5 s otherwise. Our
 * endpoint route must be what a lookup of the endpoint finds (never the
 * tunnel), and the default route (if the stack answers a lookup by mask)
 * still the same. While not UP it also logs one health line (what the
 * endpoint resolves to, the default route, the monitor's message counts and
 * the transport's counters), so the log shows why no reply comes.
 */
static void
verify(int s, void *ubuf, uint32_t ubuf_size, int up)
{
	const struct rt_ent *ep = find_kind(K_EP);
	struct rtinfo ri, dr;
	int got, dgot, v = EPV_NONE;

	if (!ep)
		return;
	got = rt_get(s, ep->dst, 0, 0, ubuf, ubuf_size, &ri) == 0;
	if (got)
		v = ep_verdict(ep, &ri);
	dgot = rt_get(s, 0, 1, 0, ubuf, ubuf_size, &dr) == 0 && is_wifi_gateway_route(&dr) &&
	    dr.have[AX_NETMASK] == 1 && dr.v[AX_NETMASK] == 0;

	/* Every check (5 s) for the first minute after a change, then every 30 s. */
	if (!up && (g_health_logs++ < 12 || g_health_logs % 6 == 0)) {
		klog("routes: health: endpoint -> %s (gw %u.%u.%u.%u flags 0x%X), default %s, monitor +a%u -a%u +r%u -r%u",
		    k_epv[v], IP4(got && ri.have[AX_GATEWAY] == 1 ? ri.v[AX_GATEWAY] : 0),
		    got ? (unsigned)ri.flags : 0u,
		    dgot ? "via the Wi-Fi" : "not found", g_mon_count[0], g_mon_count[1],
		    g_mon_count[2], g_mon_count[3]);
		transport_log_counts("health");
	}
	if (!got) {
		if (!g_get_silent_told) {
			g_get_silent_told = 1;
			klog("routes: endpoint lookup unanswered (the check cannot tell)");
		}
	} else {
		g_get_silent_told = 0;
		if (v != EPV_OURS) {
			klog("routes: endpoint route lost (lookup gives %s)", k_epv[v]);
			g_ep_lost = 1;
			set_changed("endpoint route lost");
			return;
		}
	}
	if (dgot) {
		g_no_default_told = 0;
		if (dr.v[AX_GATEWAY] != g_gw)
			set_changed("default gateway changed");
		else if (g_wlan && dr.have[AX_IFA] == 1 && dr.v[AX_IFA] != g_wlan)
			set_changed("Wi-Fi address changed");
	} else if (!g_no_default_told) {
		g_no_default_told = 1;
		klog("routes: no default route found (Wi-Fi not connected yet?)");
	}
}

/*
 * Check and re-assert the table (see the file comment: verify first).
 * Network-side routes are looked up and only what is missing is added, and
 * only what points the wrong way is deleted and re-added; tunnel ones are
 * re-added. Each message's sendto result and echoed rtm_errno are logged
 * (17 = EEXIST: it was still there; 3 = ESRCH on a delete: already gone).
 * wifi_only: just the network side (the periodic check, a loop seen).
 */
struct sent_msg { uint32_t seq; int8_t type; int8_t kind; int8_t rv; int8_t pad; int ret; int err; };

enum { RV_NONE = 0, RV_OK, RV_MISSING, RV_STALE, RV_UNKNOWN };
static const char *const k_rv[] = { "", "in place", "missing", "pointing the wrong way", "lookup unanswered" };

static int
net_side(int kind)
{
	return kind == K_EP || kind == K_ROAM || kind == K_GWNET || kind == K_BCAST;
}

/* What an RTM_GET of a network-side route's destination says about it. */
static int
net_route_state(int s, const struct rt_ent *e, void *ubuf, uint32_t ubuf_size)
{
	struct rtinfo ri;
	uint32_t ta = g_st.tunnel_address;

	if (rt_get(s, e->dst, 0, 0, ubuf, ubuf_size, &ri) != 0)
		return RV_UNKNOWN;
	if (ri.have[AX_DST] != 1 || ri.v[AX_DST] != e->dst)
		return RV_MISSING;              /* fell to a less specific route */
	if (e->kind == K_GWNET && (ri.have[AX_NETMASK] != 1 || ri.v[AX_NETMASK] != e->mask))
		return RV_UNKNOWN;              /* a host entry for that address, not our /31 */
	if (ta && ((ri.have[AX_IFA] == 1 && ri.v[AX_IFA] == ta) ||
	    (ri.have[AX_GATEWAY] == 1 && ri.v[AX_GATEWAY] == ta)))
		return RV_STALE;                /* into the tunnel */
	if (e->kind == K_EP || e->kind == K_ROAM) {
		if (!(ri.flags & RTF_GATEWAY) || ri.have[AX_GATEWAY] != 1 || ri.v[AX_GATEWAY] != e->gw)
			return RV_STALE;
	} else if (ri.flags & RTF_GATEWAY) {
		return RV_STALE;                /* ours are interface routes */
	}
	if (g_wlan && ri.have[AX_IFA] == 1 && ri.v[AX_IFA] != g_wlan)
		return RV_STALE;                /* bound to an old address */
	return RV_OK;
}

static void
reassert(int s, void *ubuf, uint32_t ubuf_size, const char *why, int wifi_only)
{
	static struct sent_msg m[2 * RT_MAX];   /* static: see g_des */
	static uint8_t rv[RT_MAX];
	int nm = 0, i, tries, left, quiet, nok = 0;

	quiet = g_reassert_logs >= 8 && g_reassert_logs % 10 != 0;
	g_reassert_logs++;
	/* Lookups first, before any message, so no echo mixes with a reply. */
	for (i = 0; i < g_napp && i < RT_MAX; i++)
		rv[i] = net_side(g_app[i].kind) ? (uint8_t)net_route_state(s, &g_app[i], ubuf, ubuf_size) :
		    (uint8_t)RV_NONE;
	for (i = 0; i < g_napp && i < RT_MAX && nm + 2 <= (int)(sizeof(m) / sizeof(m[0])); i++) {
		struct rt_ent *e = &g_app[i];
		int net = net_side(e->kind);
		if (wifi_only && !net)
			continue;
		if (rv[i] == RV_OK) {
			nok++;
			if (!quiet)
				klog("routes:   %s: in place", k_kind[e->kind]);
			continue;
		}
		if (rv[i] == RV_STALE) {
			m[nm].seq = send_route(s, RTM_DELETE, e);
			m[nm].type = RTM_DELETE;
			m[nm].kind = (int8_t)e->kind;
			m[nm].rv = (int8_t)rv[i];
			m[nm].ret = g_last_send_ret;
			m[nm].err = -1;
			nm++;
		}
		e->seq = m[nm].seq = send_route(s, RTM_ADD, e);
		m[nm].type = RTM_ADD;
		m[nm].kind = (int8_t)e->kind;
		m[nm].rv = (int8_t)rv[i];
		m[nm].ret = g_last_send_ret;
		m[nm].err = -1;
		nm++;
	}
	g_last_ep_refresh = now_us();
	if (nm == 0) {
		if (!quiet)
			klog("routes: check (%s): %d network-side route(s) in place, nothing sent", why, nok);
		return;
	}
	/* The echoes (SO_USELOOPBACK), in order, among other broadcasts. */
	left = nm;
	if (ubuf && ubuf_size >= RTM_HDR) {
		ph("route echo recv");
		for (tries = 0; tries < 3 * nm + 8 && left > 0; tries++) {
			uint8_t hdr[RTM_HDR];
			int n = ksceNetRecvfrom(s, ubuf, ubuf_size < 0x100 ? ubuf_size : 0x100,
			    MSG_DONTWAIT_, NULL, NULL);
			uint32_t seq;
			if (n == EAGAIN_ && tries < 4) {
				ksceKernelDelayThread(1000);
				continue;
			}
			if (n < RTM_HDR)
				break;
			if (!rt_copyin(hdr, sizeof(hdr), ubuf, n, RTM_HDR))
				continue;
			seq = (uint32_t)rd32ne(hdr + 0x14);
			for (i = 0; i < nm; i++)
				if (m[i].err == -1 && m[i].seq == seq && m[i].type == hdr[3]) {
					m[i].err = rd32ne(hdr + 0x18);
					left--;
					break;
				}
		}
	}
	if (!quiet)
		klog("routes: check (%s): %d in place, %d messages, %d echoes", why, nok, nm, nm - left);
	for (i = 0; i < nm; i++) {
		/* An addition refused for a reason other than "still there" is
		 * forgotten, so the reconciler adds it again in 2 s. sendto's
		 * own result counts when no echo came (-17 = EEXIST too). */
		int err = m[i].err != -1 ? m[i].err : m[i].ret < 0 ? -m[i].ret : 0;
		if (m[i].type == RTM_ADD && err != 0 && err != EEXIST_) {
			int k;
			for (k = 0; k < g_napp; k++)
				if (g_app[k].seq == m[i].seq) {
					if (g_app[k].kind == K_BLK)
						g_blk_refused++;
					app_drop(k);
					g_add_hold_until = now_us() + US(2);
					break;
				}
			klog("routes: check: %s add refused (ret 0x%08X errno %d)",
			    k_kind[(int)m[i].kind], (unsigned)m[i].ret, m[i].err);
			continue;
		}
		if (quiet)
			continue;
		klog("routes:   %s %s%s%s: ret 0x%08X errno %d%s", k_kind[(int)m[i].kind],
		    m[i].type == RTM_ADD ? "add" : "delete",
		    m[i].rv != RV_NONE ? ", was " : "", m[i].rv != RV_NONE ? k_rv[(int)m[i].rv] : "",
		    (unsigned)m[i].ret, m[i].err,
		    m[i].type == RTM_ADD && err == EEXIST_ ? " (still there)" :
		    m[i].type == RTM_DELETE && m[i].err == 3 ? " (was already gone)" : "");
	}
}

static void
process_change(void)
{
	const char *why = g_netchg_why ? g_netchg_why : "?";

	g_netchg = 0;
	klog("routes: network change (%s)", why);
	if (g_st.want && !g_st.paused) {
		if (TN_LOCK_ON_WIFI_CHANGE && has_kind(K_TUN))
			tn_lock_arm(why);
		g_rehs_pending = 1;
	}
	if (g_ep_lost) {
		/* Gone from the stack already: forget it so it is added again. */
		int i;
		g_ep_lost = 0;
		for (i = g_napp - 1; i >= 0; i--)
			if (g_app[i].kind == K_EP || g_app[i].kind == K_ROAM)
				app_drop(i);
	}
	/* Re-learn, keeping the old gateway and endpoint route meanwhile;
	 * then re-assert every route (the stack is not trusted after this). */
	g_rediscover = 1;
	g_relearn_tries = 0;
	g_next_discover = 0;
	g_reassert = 1;
	g_reassert_why = why;
	g_reassert_logs = 0;
	g_health_logs = 0;
	g_next_verify = 0;
}

/* ---- the sweep (file comment) --------------------------------------------- */

#define SWEEP_STEP 16

static void
sweep_step(int s, void *ubuf, uint32_t ubuf_size)
{
	uint32_t ips[SWEEP_STEP];
	unsigned lost;
	int n = iface_arp_take(ips, SWEEP_STEP, &lost), i;

	g_sw_lost += lost;
	ph("sweep");
	for (i = 0; i < n; i++) {
		struct rtinfo ri;
		g_sw_seen++;
		if (rt_get(s, ips[i], 0, 0, ubuf, ubuf_size, &ri) != 0) {
			g_sw_gone++;
			continue;
		}
		if ((ri.flags & RTF_HOST) && ri.have[AX_DST] == 1 && ri.v[AX_DST] == ips[i] &&
		    ri.have[AX_IFA] == 1 && g_sweep_ta && ri.v[AX_IFA] == g_sweep_ta) {
			uint8_t msg[RTM_HDR + 3 * sizeof(SockaddrIn)];
			int len = build(msg, RTM_DELETE, RTF_HOST, RTA_DST, next_seq(), ips[i], 0, 0);
			ph("sweep delete");
			if (ksceNetSendto(s, msg, len, 0, NULL, 0) >= 0)
				g_sw_del++;
		} else {
			g_sw_gone++;            /* not on the tunnel interface (any more) */
		}
	}
	if (n < SWEEP_STEP) {
		g_sweep = 0;
		if (g_sw_seen || g_sw_del || g_sw_gone || g_sw_lost)
			klog("routes: sweep: %u ARP-answered checked, %u host entries on the tunnel deleted, %u not there, %u untracked",
			    g_sw_seen, g_sw_del, g_sw_gone, g_sw_lost);
		g_sw_seen = g_sw_del = g_sw_gone = g_sw_lost = 0;
	}
}

/* ---- the LAN purge (file comment) ----------------------------------------- */

#define NETPS_SYSCALLS_LIB  0x2CBED2C6u   /* SceNetPsForSyscalls */
#define NID_SYSCALL_SYSCTL  0x3D7495B0u   /* sceNetSyscallSysctl */
#define CTL_NET_      4
#define PF_ROUTE_     17
#define AF_INET_      2
#define NET_RT_FLAGS_ 2
#define ENOMEM_       12
#define LAN_STEP      16                  /* RTM_GETs per RX step */
#define LAN_DUMPS     8
#define LP_UHDR       0x40                /* args, name, oldlen at the start of ubuf */

typedef int (*sysctl_fn)(void *uargs);
static sysctl_fn g_sysctl;
static int g_sysctl_state;               /* 0 not resolved yet, 1 ok, -1 unavailable */
static uint8_t g_lp_buf[2048];           /* the dump, copied in (holder only) */

/* A route (RTM_GET reply or dump entry) that is a LAN host's own entry on
 * the Wi-Fi: a host route for exactly ip, no gateway flag, a link-layer
 * gateway (have 2 = not AF_INET), the Wi-Fi address as interface address. */
static int
lan_is_wifi_host(int32_t flags, const uint32_t v[AX_MAX], const uint8_t have[AX_MAX], uint32_t ip)
{
	return (flags & (RTF_HOST | RTF_GATEWAY)) == RTF_HOST && have[AX_DST] == 1 &&
	    v[AX_DST] == ip && have[AX_GATEWAY] == 2 && have[AX_IFA] == 1 && v[AX_IFA] == g_wlan;
}

static int
lan_delete(int s, uint32_t ip)
{
	uint8_t msg[RTM_HDR + 3 * sizeof(SockaddrIn)];
	int len = build(msg, RTM_DELETE, RTF_HOST, RTA_DST, next_seq(), ip, 0, 0);

	ph("lan purge delete");
	if (ksceNetSendto(s, msg, len, 0, NULL, 0) < 0)
		return 0;
	g_lp_del++;
	return 1;
}

/* Look ip up; delete its entry if it is a LAN host's on the Wi-Fi. */
static void
lan_check(int s, uint32_t ip, void *ubuf, uint32_t ubuf_size)
{
	struct rtinfo ri;

	if (!lan_candidate(ip))
		return;
	g_lp_seen++;
	if (rt_get(s, ip, 0, 0, ubuf, ubuf_size, &ri) == 0 &&
	    lan_is_wifi_host(ri.flags, ri.v, ri.have, ip))
		lan_delete(s, ip);
}

/*
 * One dump of the ARP entries (RTF_LLINFO), and the deletions it calls for.
 * 1 = complete, 0 = cut short by the buffer, -1 = no dump. The arguments
 * live in the lent user buffer: sceNetSyscallSysctl copies its argument
 * block, the MIB and oldlen in and the table out as user memory.
 */
static int
lan_dump(int s, void *ubuf, uint32_t ubuf_size, int *deleted)
{
	uint32_t hdr[LP_UHDR / 4];
	uintptr_t u = (uintptr_t)ubuf;
	uint32_t cap, n, off;
	int r;

	*deleted = 0;
	if (!TN_LAN_DUMP || g_sysctl_state < 0 || !ubuf || ubuf_size < LP_UHDR + 0x100)
		return -1;
	if (g_sysctl_state == 0) {
		uintptr_t f = 0;
		r = module_get_export_func(KERNEL_PID, "SceNetPs", NETPS_SYSCALLS_LIB,
		    NID_SYSCALL_SYSCTL, &f);
		g_sysctl_state = r >= 0 && f ? 1 : -1;
		g_sysctl = (sysctl_fn)f;
		if (g_sysctl_state < 0) {
			klog("routes: LAN purge: no route dump (resolve 0x%08X)", (unsigned)r);
			return -1;
		}
	}
	cap = (ubuf_size < sizeof(g_lp_buf) ? ubuf_size : (uint32_t)sizeof(g_lp_buf)) - LP_UHDR;
	cap &= ~3u;
	/* Zeroed first: a dump cut short (ENOMEM) does not say where it ends. */
	memset(g_lp_buf, 0, cap);
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = (uint32_t)(u + 0x20);           /* name */
	hdr[1] = 6;                              /* namelen */
	hdr[2] = (uint32_t)(u + LP_UHDR);        /* oldp */
	hdr[3] = (uint32_t)(u + 0x38);           /* oldlenp */
	hdr[8] = CTL_NET_;  hdr[9] = PF_ROUTE_;  hdr[10] = 0;
	hdr[11] = AF_INET_; hdr[12] = NET_RT_FLAGS_; hdr[13] = RTF_LLINFO;
	hdr[14] = cap;                           /* oldlen */
	if (ksceKernelMemcpyKernelToUser((void *)(u + LP_UHDR), g_lp_buf, cap) < 0 ||
	    ksceKernelMemcpyKernelToUser(ubuf, hdr, sizeof(hdr)) < 0)
		return -1;
	ph("lan purge dump");
	r = g_sysctl(ubuf);
	if (r < 0 && r != -ENOMEM_) {
		if (g_lp_fail++ < 3)
			klog("routes: LAN purge: route dump failed (0x%08X)", (unsigned)r);
		return -1;
	}
	n = cap;
	if (r == 0) {
		uint32_t got = 0;
		if (ksceKernelMemcpyUserToKernel(&got, (const void *)(u + 0x38), 4) < 0)
			return -1;
		if (got < n)
			n = got;
	}
	if (ksceKernelMemcpyUserToKernel(g_lp_buf, (const void *)(u + LP_UHDR), n) < 0)
		return -1;
	g_lp_dumps++;

	for (off = 0; off + RTM_HDR <= n; ) {
		const uint8_t *m = g_lp_buf + off;
		uint32_t v[AX_MAX];
		uint8_t have[AX_MAX];
		uint16_t len;
		memcpy(&len, m, 2);
		if (len < RTM_HDR || len > n - off || m[2] != RTM_VERSION)
			break;                      /* the end (zeroes), or malformed */
		parse_addrs(m, len, RTM_HDR, (uint32_t)rd32ne(m + 0x0c), v, have);
		if (have[AX_DST] == 1 && lan_candidate(v[AX_DST])) {
			g_lp_seen++;
			if (lan_is_wifi_host(rd32ne(m + 0x08), v, have, v[AX_DST]))
				*deleted += lan_delete(s, v[AX_DST]);
		}
		off += (len + 3u) & ~3u;
	}
	return r == 0 ? 1 : 0;
}

static void
lan_done(const char *why)
{
	klog("routes: LAN purge (%s%s): %u Wi-Fi host entries deleted, %u checked, %u dumps, %u ms",
	    g_lp_how ? g_lp_how : "?", why, g_lp_del, g_lp_seen, g_lp_dumps,
	    (unsigned)((now_us() - g_lp_t0) / 1000));
	g_lp = LP_IDLE;
}

/*
 * HOST CENSUS (diagnostic, only while debug_routes exists). A dump of every
 * host route (NET_RT_FLAGS, RTF_HOST), logged: when the tunnel routes have
 * just been added and every 30 s. It changes nothing. It shows whether the
 * stack keeps per-destination host routes through the Wi-Fi gateway that
 * would outrank the tunnel's halves (a /32 beats a /1). The endpoint and
 * tunnel addresses are shown as "endpoint" / "tunnel".
 */
static void
hc_addr(char *out, unsigned size, int have, uint32_t a)
{
	if (have != 1)
		snprintf(out, size, have ? "link" : "-");
	else if (a && a == g_st.endpoint_ip)
		snprintf(out, size, "endpoint");
	else if (a && a == g_st.tunnel_address)
		snprintf(out, size, "tunnel");
	else
		snprintf(out, size, "%u.%u.%u.%u", IP4(a));
}

static void
host_census(void *ubuf, uint32_t ubuf_size, const char *why)
{
	uint32_t hdr[LP_UHDR / 4];
	uintptr_t u = (uintptr_t)ubuf;
	uint32_t cap, n, off;
	unsigned total = 0, gw_wifi = 0, gw_tun = 0, link = 0, shown = 0;
	int r;

	if (!TN_LAN_DUMP || !ubuf || ubuf_size < LP_UHDR + 0x100)
		return;
	if (g_sysctl_state == 0) {
		uintptr_t f = 0;
		r = module_get_export_func(KERNEL_PID, "SceNetPs", NETPS_SYSCALLS_LIB,
		    NID_SYSCALL_SYSCTL, &f);
		g_sysctl_state = r >= 0 && f ? 1 : -1;
		g_sysctl = (sysctl_fn)f;
	}
	if (g_sysctl_state < 0) {
		klog("routes: host census (%s): no route dump", why);
		return;
	}
	cap = (ubuf_size < sizeof(g_lp_buf) ? ubuf_size : (uint32_t)sizeof(g_lp_buf)) - LP_UHDR;
	cap &= ~3u;
	memset(g_lp_buf, 0, cap);
	memset(hdr, 0, sizeof(hdr));
	hdr[0] = (uint32_t)(u + 0x20);
	hdr[1] = 6;
	hdr[2] = (uint32_t)(u + LP_UHDR);
	hdr[3] = (uint32_t)(u + 0x38);
	hdr[8] = CTL_NET_;  hdr[9] = PF_ROUTE_;  hdr[10] = 0;
	hdr[11] = AF_INET_; hdr[12] = NET_RT_FLAGS_; hdr[13] = RTF_HOST;
	hdr[14] = cap;
	if (ksceKernelMemcpyKernelToUser((void *)(u + LP_UHDR), g_lp_buf, cap) < 0 ||
	    ksceKernelMemcpyKernelToUser(ubuf, hdr, sizeof(hdr)) < 0)
		return;
	ph("host census dump");
	r = g_sysctl(ubuf);
	if (r < 0 && r != -ENOMEM_) {
		klog("routes: host census (%s): dump failed (0x%08X)", why, (unsigned)r);
		return;
	}
	n = cap;
	if (r == 0) {
		uint32_t got = 0;
		if (ksceKernelMemcpyUserToKernel(&got, (const void *)(u + 0x38), 4) < 0)
			return;
		if (got < n)
			n = got;
	}
	if (ksceKernelMemcpyUserToKernel(g_lp_buf, (const void *)(u + LP_UHDR), n) < 0)
		return;
	for (off = 0; off + RTM_HDR <= n; ) {
		const uint8_t *m = g_lp_buf + off;
		uint32_t v[AX_MAX];
		uint8_t have[AX_MAX];
		uint16_t len;
		int32_t flags;
		memcpy(&len, m, 2);
		if (len < RTM_HDR || len > n - off || m[2] != RTM_VERSION)
			break;
		flags = rd32ne(m + 0x08);
		parse_addrs(m, len, RTM_HDR, (uint32_t)rd32ne(m + 0x0c), v, have);
		total++;
		if (!(flags & RTF_GATEWAY))
			link++;
		else if (have[AX_IFA] == 1 && v[AX_IFA] == g_st.tunnel_address)
			gw_tun++;
		else
			gw_wifi++;
		if ((flags & RTF_GATEWAY) && shown < 24) {
			char d[20], g[20], a[20];
			hc_addr(d, sizeof(d), have[AX_DST], v[AX_DST]);
			hc_addr(g, sizeof(g), have[AX_GATEWAY], v[AX_GATEWAY]);
			hc_addr(a, sizeof(a), have[AX_IFA], v[AX_IFA]);
			klog("routes:   host %s gw %s ifa %s f 0x%X", d, g, a, (unsigned)flags);
			shown++;
		}
		off += (len + 3u) & ~3u;
	}
	klog("routes: host census (%s): %u host routes%s: %u through a gateway on the Wi-Fi, %u on the tunnel, %u link-level",
	    why, total, r == 0 ? "" : " (cut short)", gw_wifi, gw_tun, link);
}

/* The fallback: every host of a /24 or smaller subnet, else the hosts seen. */
static void
lan_sweep_start(void)
{
	g_lp = LP_SWEEP;
	if (g_sub_prefix >= 24) {
		g_lp_how = g_lp_dumps ? "dump, then RTM_GET sweep" : "RTM_GET sweep";
		g_lp_cur = (g_wlan & prefix_mask(g_sub_prefix)) + 1u;
	} else {
		g_lp_how = g_lp_dumps ? "dump, then hosts seen" : "hosts seen";
		g_lp_cur = 0;
	}
}

/* One RX step of it, with the table owned (lent RX thread only). */
static void __attribute__((noinline))
lan_step(int s, void *ubuf, uint32_t ubuf_size)
{
	int i;

	if (g_lp_arm) {
		g_lp_arm = 0;
		g_lp = LP_DUMP;
		g_lp_how = "dump";
		g_lp_dumps = g_lp_del = g_lp_seen = 0;
		g_lp_t0 = now_us();
	}
	if (!lan_guard()) {
		/* Allow local on, the halves gone, or the Wi-Fi unknown:
		 * nothing is touched. */
		g_lan_new_n = 0;
		if (g_lp != LP_IDLE)
			lan_done(", stopped");   /* allow local on, or the halves gone */
		return;
	}
	/* Entries seen appearing with the halves in place (the monitor). */
	for (i = 0; i < g_lan_new_n; i++)
		lan_check(s, g_lan_new[i], ubuf, ubuf_size);
	g_lan_new_n = 0;

	if (g_lp == LP_DUMP) {
		int del, r = lan_dump(s, ubuf, ubuf_size, &del);
		if (r == 1)
			lan_done("");
		else if (r == 0 && del > 0 && g_lp_dumps < LAN_DUMPS)
			;                           /* cut short: again next step, minus those */
		else
			lan_sweep_start();          /* no dump, or one that makes no progress */
		return;
	}
	if (g_lp == LP_SWEEP) {
		uint32_t m = prefix_mask(g_sub_prefix), last = (g_wlan & m) | ~m;
		for (i = 0; i < LAN_STEP; i++) {
			if (g_sub_prefix >= 24) {
				if ((g_lp_cur & m) != (g_wlan & m) || g_lp_cur >= last)
					break;
				lan_check(s, g_lp_cur++, ubuf, ubuf_size);
			} else {
				if (g_lp_cur >= LAN_TRACK)
					break;
				if (g_lan_seen[g_lp_cur])
					lan_check(s, g_lan_seen[g_lp_cur], ubuf, ubuf_size);
				g_lp_cur++;
			}
		}
		if (i < LAN_STEP)
			lan_done("");
	}
}

/* ---- for the restore check (restore.c) ------------------------------------ */

int
routes_lookup(int s, uint32_t dst, int with_mask, uint32_t mask, void *ubuf,
    uint32_t ubuf_size, struct rt_look *out)
{
	struct rtinfo ri;

	memset(out, 0, sizeof(*out));
	if (rt_get(s, dst, with_mask, mask, ubuf, ubuf_size, &ri) != 0)
		return -1;
	out->ok = 1;
	out->flags = ri.flags;
	out->dst = ri.have[AX_DST] == 1 ? ri.v[AX_DST] : 0;
	out->has_mask = ri.have[AX_NETMASK] == 1;
	out->mask = ri.v[AX_NETMASK];
	out->gw_kind = ri.have[AX_GATEWAY];
	out->gw = ri.have[AX_GATEWAY] == 1 ? ri.v[AX_GATEWAY] : 0;
	out->has_ifa = ri.have[AX_IFA] == 1;
	out->ifa = ri.have[AX_IFA] == 1 ? ri.v[AX_IFA] : 0;
	return 0;
}

void
routes_table(int *n, unsigned *kinds)
{
	int i;

	*n = g_napp;
	*kinds = 0;
	for (i = 0; i < g_napp; i++)
		*kinds |= 1u << g_app[i].kind;
}

int routes_monitor_open(void) { return g_mon >= 0; }

void
routes_disconnect(int pause)
{
	routes_sync();
	g_def_check = 1;                /* the RX step sees that the default route exists */
	tn_wake();
	/* The plugin removes the interface address within ~1 s of seeing the
	 * state; the after-snapshot waits for that (restore.c). */
	restore_request(pause ? 4 : 2, now_us() + US(2));
}


/* ---- public ---------------------------------------------------------------- */

void
routes_init(void)
{
	g_owner = 0;
}

void
routes_net_ready(uint32_t addr, uint32_t gw)
{
	/* Hints for relearn (method 3) and the address; the lent RX thread is
	 * not in routes_apply while net_wait is set, so plain writes are fine. */
	if (gw)
		g_mon_gw = gw;
	if (addr) {
		if (addr != g_wlan) {
			strand_disarm("the Wi-Fi's address changed");
			if (g_wlan)
				default_giveup("the Wi-Fi's address changed");
		}
		g_mon_wlan = addr;
	}
	if (gw && g_gw && gw != g_gw)
		klog("routes: the network came back with another gateway: a network change");
	routes_net_changed(gw && g_gw && gw != g_gw ? "network ready, new gateway" : "network ready after a wake");
	/* While the Wi-Fi was gone its subnet route
	 * was too, so LAN hosts matched the tunnel routes and net_ps made host
	 * entries for them on the tunnel interface (it answers every ARP).
	 * Back on the Wi-Fi, those entries beat the subnet route: with allow
	 * local on, the LAN (FTP from a PC) stayed unreachable until the
	 * disconnect's sweep. Sweep now instead; a deleted internet host entry
	 * is simply made again on the tunnel. */
	if (g_st.allow_local && g_st.tunnel_address) {
		g_sweep_ta = g_st.tunnel_address;
		g_sweep = 1;
	}
}

void routes_mark_dirty(void) { g_dirty = 1; tn_wake(); }
int  routes_dirty(void) { return g_dirty || g_netchg; }

/* The same test as routes_apply's early return (the RX step's idle
 * wait). Any flag that would make that pass do work is set together with a
 * tn_wake (routes_mark_dirty, routes_net_changed, restore_request, control
 * calls), and the idle wait is bounded anyway. */
int
routes_quiet(void)
{
	return !active() && g_napp == 0 && !g_netchg && g_mon < 0 && !g_sweep &&
	    !restore_pending() && !g_def_check && !g_def_gw;
}

void
routes_net_changed(const char *why)
{
	g_mon_reopen = 1;
	set_changed(why);
	tn_wake();
}

void
routes_ep_suspect(const char *why)
{
	g_reassert_why = why;
	g_ep_suspect = 1;
}

/* Under the lock. */
static void
connect_reset(void)
{
	g_connect_pending = 0;
	if (!has_kind(K_EP) && !has_kind(K_TUN))
		forget_wifi();          /* a gateway learnt on another network is stale
		                         * (a Lockdown hold kept it current: the monitor
		                         * ran meanwhile) */
	g_next_verify = 0;
	g_add_hold_until = 0;
	g_bcast_refused = 0;
	g_blk_refused = 0;
	g_health_logs = 0;
	g_dirty = 1;
}

void
routes_connect(void)
{
	if (!trylock()) {
		g_connect_pending = 1;  /* the RX step does it (see OWNERSHIP) */
		g_dirty = 1;
		return;
	}
	connect_reset();
	unlock();
}

/*
 * Big buffers are static, not on the stack: the
 * lent RX step runs in a syscall, on a small kernel stack (the Lockdown
 * connect crash). Each is used only by the holder
 * of the ownership flag (one thread at a time; nothing here recurses).
 */
static struct rt_ent g_des[RT_MAX];

int
routes_apply(void *ubuf, uint32_t ubuf_size)
{
	struct rt_ent *d = g_des;
	int nd, s = -1;
	SceUInt64 now;

	if (TN_NO_NETPS()) {
		/* A lost network or a suspend: no route work now, and none to come
		 * for the default route (the Wi-Fi sets its own when it
		 * reconnects); nothing of ours keeps polling meanwhile. */
		g_def_check = 0;
		default_giveup("the network was lost");
		return 0;
	}
	if (g_sock_defer >= 0) {
		wd_phase(WD_RX, "route socket close (deferred)");
		sock_close(g_sock_defer);
		g_sock_defer = -1;
	}
	tn_lock_update();
	if (!active() && g_napp == 0 && !g_netchg && g_mon < 0 && !g_sweep && !restore_pending() &&
	    !g_def_check && !g_def_gw)
		return 0;

	if (!trylock())
		return 0;               /* a syscall is applying removals: next step */
	g_in_rx = 1;
	now = now_us();
	if (g_connect_pending)
		connect_reset();
	/* 0. the restore check's before-connect snapshot, before any change */
	restore_step(0, 0, route_socket, &s, ubuf, ubuf_size);
	if (active()) {
		monitor_step(ubuf, ubuf_size);
		if (TN_NO_NETPS()) {
			/* the Wi-Fi went away. Arm the lock (flags only) and stop:
			 * no route check now, not even a close - a socket opened this
			 * step is closed after the network is back. */
			process_change();
			publish_flags();
			g_in_rx = 0;
			unlock();
			if (s >= 0)
				g_sock_defer = s;
			return 0;
		}
	} else if (g_mon >= 0) {
		ph("monitor close");
		sock_close(g_mon);      /* not active: no socket of ours stays open */
		g_mon = -1;
		g_mon_retry = 0;
		klog("routes: change monitor closed (tunnel off)");
	}
	if (g_netchg)
		process_change();
	if (g_dirty) {
		g_dirty = 0;
		g_next_discover = g_next_subnet = 0;
		g_add_hold_until = 0;
	}

	/* 1. bring the table in line (removes a stale endpoint route first) */
	nd = build_desired(d);
	if (needs_work(d, nd, 0) && (s = route_socket()) >= 0)
		reconcile(s, d, nd, 0, ubuf, ubuf_size);

	if (active()) {
		/* 2. learn (or after a change re-learn) the gateway and address
		 * (the Lockdown hold needs them too, with no endpoint) */
		if ((!g_gw || g_rediscover) && (g_st.endpoint_ip || !g_st.want) &&
		    now >= g_next_discover) {
			g_next_discover = now + 500 * 1000;
			if (s >= 0 || (s = route_socket()) >= 0) {
				relearn(s, ubuf, ubuf_size);
				g_next_verify = now + US(10);
			}
		}
		/* 3. the Wi-Fi subnet: needed for allow local off, shown always.
		 * Halves of a previous subnet would answer the probe: drop them
		 * first (they are re-added at once below). */
		if (g_gw && g_wlan && !g_sub_prefix && now >= g_next_subnet) {
			g_next_subnet = now + US(5);
			if (s >= 0 || (s = route_socket()) >= 0) {
				int i;
				for (i = g_napp - 1; i >= 0; i--)
					if (g_app[i].kind == K_LOCAL) {
						send_route(s, RTM_DELETE, &g_app[i]);
						app_drop(i);
					}
				discover_subnet(s, ubuf, ubuf_size);
			}
		}
		/* 4. after a change or a wake, once the gateway is known: every
		 * route re-asserted, then a fresh handshake on the new path. A
		 * loop seen by the interface refreshes the Wi-Fi side at once
		 * (at most every 2 s). */
		if (g_reassert && g_gw && !g_rediscover &&
		    (s >= 0 || (s = route_socket()) >= 0)) {
			g_reassert = 0;
			g_ep_suspect = 0;
			reassert(s, ubuf, ubuf_size, g_reassert_why ? g_reassert_why : "?", 0);
			g_next_refresh = now + US(20);
		} else if (g_ep_suspect && has_kind(K_EP) && now - g_last_ep_refresh >= US(2) &&
		    (s >= 0 || (s = route_socket()) >= 0)) {
			g_ep_suspect = 0;
			reassert(s, ubuf, ubuf_size, g_reassert_why ? g_reassert_why : "loop", 1);
		}
		if (g_rehs_pending && g_gw && !g_rediscover && g_st.want) {
			/* Never waits for the protocol lock: busy =
			 * the next step tries again. */
			if (tnp_rehandshake_try() == 0) {
				g_rehs_pending = 0;
				klog("routes: re-handshaking on the new network path");
				iface_kick_tx();
				net_ready_work_done("socket rebuilt, routes checked, handshake started");
			}
		}
	}

	/* 5. additions that the discovery made possible */
	nd = build_desired(d);
	if (needs_work(d, nd, 0) && (s >= 0 || (s = route_socket()) >= 0))
		reconcile(s, d, nd, 0, ubuf, ubuf_size);

	/* 6. the periodic check: 10 s while UP, 5 s otherwise; and while not
	 * UP the Wi-Fi side routes are refreshed every 20 s (a route that
	 * survived a Wi-Fi reassociation may be bound to the old address). */
	if (g_st.want && !g_st.paused && has_kind(K_EP) && now >= g_next_verify) {
		struct tnp_stats st;
		int up;
		tnp_get_stats(&st);
		up = st.state == TNP_UP;
		g_next_verify = now + US(up ? 10 : 5);
		if (s >= 0 || (s = route_socket()) >= 0) {
			verify(s, ubuf, ubuf_size, up);
			if (!up && !g_netchg && now >= g_next_refresh) {
				g_next_refresh = now + US(20);
				reassert(s, ubuf, ubuf_size, "no handshake yet", 1);
			}
		}
	}

	/* 5b. the Wi-Fi default route: put back after a refresh that lost it, or
	 * looked for after a disconnect or pause (flush_remembered_routes). The
	 * two minutes are counted here too, so a route socket that cannot be
	 * opened does not make it poll on. */
	if (g_def_gw && now - g_def_start >= DEF_GIVE_UP)
		default_giveup("2 minutes");
	if (g_def_check || (g_def_gw && now >= g_def_next)) {
		g_def_next = now + DEF_SLOW_US;
		if (s >= 0 || (s = route_socket()) >= 0)
			default_step(s, ubuf, ubuf_size, now);
	}

	/* 6b. the LAN purge: with allow local off and the halves just added,
	 * the LAN hosts' own entries on the Wi-Fi go (file comment) */
	if ((g_lp_arm || g_lp != LP_IDLE || g_lan_new_n) && (s >= 0 || (s = route_socket()) >= 0))
		lan_step(s, ubuf, ubuf_size);

	/* 6c. the host census (diagnostic, debug_routes only) */
	if (restore_debug_on() && (g_hc_due || now >= g_hc_next)) {
		host_census(ubuf, ubuf_size, g_hc_due ? "tunnel routes added" : "every 30 s");
		g_hc_due = 0;
		g_hc_next = now + US(30);
	}

	/* 7. the sweep of cloned host entries (16 a step), then the restore
	 * check's after-disconnect / on-demand snapshot */
	if (g_sweep && (s >= 0 || (s = route_socket()) >= 0))
		sweep_step(s, ubuf, ubuf_size);   /* harmless if routes came back: re-cloned */
	restore_step(1, g_sweep, route_socket, &s, ubuf, ubuf_size);

	publish_flags();
	g_in_rx = 0;
	unlock();
	if (s >= 0) {
		wd_phase(WD_RX, "route socket close");
		sock_close(s);
	}
	return 0;
}

void
routes_sync(void)
{
	struct rt_ent *d = g_des;      /* under the ownership flag, as routes_apply */
	int nd, s;

	if (TN_NO_NETPS()) {
		g_dirty = 1;            /* the RX thread does it once the network is ready */
		return;
	}
	if (!trylock()) {
		g_dirty = 1;            /* the RX step is in; it applies them (OWNERSHIP) */
		return;
	}
	nd = build_desired(d);
	if (needs_work(d, nd, 1)) {
		SceUInt64 t0 = ksceKernelGetSystemTimeWide();
		s = route_socket();
		slow_note("sync: route socket", t0);
		if (s >= 0) {
			t0 = ksceKernelGetSystemTimeWide();
			reconcile(s, d, nd, 1, NULL, 0);
			slow_note("sync: removals", t0);
			t0 = ksceKernelGetSystemTimeWide();
			sock_close(s);
			slow_note("sync: socket close", t0);
		}
	}
	publish_flags();
	g_dirty = 1;
	unlock();
}

void
routes_remove(void)
{
	routes_sync();
}

/* One answer from a consistent copy (g_pub), or -1 if it was torn. */
static int
iface_for_once(uint32_t ip)
{
	uint32_t s1 = __atomic_load_n(&g_pub_seq, __ATOMIC_ACQUIRE), wl, sub;
	int i, n, best = -1, bestp = -1, kind = 0, v;

	if (s1 & 1)
		return -1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	n = g_pub_n;
	if (n < 0 || n > RT_MAX)
		return -1;
	for (i = 0; i < n; i++) {
		int p = mask_prefix(g_pub[i].mask);
		if ((ip & g_pub[i].mask) == g_pub[i].dst && p > bestp) {
			best = i;
			bestp = p;
		}
	}
	if (best >= 0)
		kind = g_pub[best].kind;
	wl = g_pub_wlan;
	sub = g_pub_sub;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (__atomic_load_n(&g_pub_seq, __ATOMIC_ACQUIRE) != s1)
		return -1;

	if (best < 0 || kind == K_EP || kind == K_ROAM || kind == K_GWNET || kind == K_BCAST)
		v = VT_IFACE_WLAN;
	else if (sub && bestp < (int)sub && wl &&
	    (ip & prefix_mask(sub)) == (wl & prefix_mask(sub)))
		v = VT_IFACE_WLAN;          /* netctl's subnet route beats a shorter one (allow local on) */
	else if (kind == K_BLK)
		v = VT_IFACE_NONE;          /* rejected (THE BLOCK) */
	else
		v = VT_IFACE_TUNNEL;
	return v;
}

int
routes_iface_for(uint32_t ip)
{
	int i, v;

	/* Never waits: a few tries at a consistent copy (see publish_table). */
	for (i = 0; i < 8; i++)
		if ((v = iface_for_once(ip)) >= 0)
			return v;
	return VT_IFACE_NONE;
}
