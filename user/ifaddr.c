/*
 * ifaddr.c - see ifaddr.h.
 *
 * The kernel module attaches the tunnel interface and writes its routes,
 * but its address and IFF_UP are set from user space with the same ioctls
 * the system uses for Wi-Fi (SIOCAIFADDR, SIOCSIFFLAGS), here from the
 * SceShell plugin's worker, so it works whichever surface connected: the
 * app, Settings or the quick menu. The interface is the first "other" unit
 * that accepts the address.
 */
#include <psp2/net/net.h>
#include <psp2/net/net_syscalls.h>
#include <string.h>

#include "ifaddr.h"
#include "i18n.h"
#include "log.h"

typedef struct {
	uint8_t  len, family;
	uint16_t port;
	uint32_t addr;
	uint16_t vport;
	uint8_t  zero[6];
} Sin;

typedef struct { char name[16]; Sin addr, brd, mask; } IfAliasReq;
typedef struct { char name[16]; union { short flags; Sin addr; uint8_t pad[16]; } u; } IfReq;

#define SIOCAIFADDR  0x8040691a
#define SIOCDIFADDR  0x80206919
#define SIOCGIFFLAGS 0xc0206911
#define SIOCGIFADDR  0xc0206921
#define SIOCSIFFLAGS 0x80206910
#define IFF_UP 1

static char g_ifname[16];
static uint32_t g_addr;

static void sin_set(Sin *s, uint32_t ip)
{
	memset(s, 0, sizeof(*s));
	s->len = sizeof(*s);
	s->family = SCE_NET_AF_INET;
	s->addr = __builtin_bswap32(ip);   /* network order */
}

int vtw_ifaddr_apply(uint32_t addr, uint8_t prefix)
{
	uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : (prefix ? 0xFFFFFFFFu << (32 - prefix) : 0);
	int s = sceNetSyscallSocket("vpnturbo_ifaddr", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
	if (s < 0)
		return s;
	int r = -1;
	for (int unit = 0; unit < 8; unit++) {
		IfAliasReq req;
		memset(&req, 0, sizeof(req));
		memcpy(req.name, "other0", 7);
		req.name[5] = (char)('0' + unit);
		sin_set(&req.addr, addr);
		sin_set(&req.brd, (addr & mask) | ~mask);
		sin_set(&req.mask, mask);
		r = sceNetSyscallIoctl(s, SIOCAIFADDR, &req);
		if (r >= 0) {
			strcpy(g_ifname, req.name);
			break;
		}
	}
	if (r >= 0) {
		IfReq fr;
		memset(&fr, 0, sizeof(fr));
		strcpy(fr.name, g_ifname);
		if (sceNetSyscallIoctl(s, SIOCGIFFLAGS, &fr) >= 0) {
			fr.u.flags |= IFF_UP;
			r = sceNetSyscallIoctl(s, SIOCSIFFLAGS, &fr);
		}
	}
	sceNetSyscallClose(s);
	if (r >= 0)
		g_addr = addr;
	VTW_LOG("ifaddr: %s /%u on %s -> 0x%08X", r >= 0 ? "set" : "failed", (unsigned)prefix,
	        r >= 0 ? g_ifname : "-", (unsigned)r);
	return r;
}

/* ---- the restore check's interface half ---------------------------------- */

static const char *const k_if[VTW_IFSNAP_N] = { "other0", "other1", "wlan0" };

void vtw_ifsnap_take(VtwIfSnap *sn)
{
	memset(sn, 0, sizeof(*sn));
	int s = sceNetSyscallSocket("vpnturbo_ifsnap", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
	if (s < 0)
		return;
	for (int i = 0; i < VTW_IFSNAP_N; i++) {
		IfReq r;
		memset(&r, 0, sizeof(r));
		strcpy(r.name, k_if[i]);
		if (sceNetSyscallIoctl(s, SIOCGIFFLAGS, &r) < 0)
			continue;
		sn->ok[i] = 1;
		sn->flags[i] = (uint16_t)r.u.flags;
		memset(&r, 0, sizeof(r));
		strcpy(r.name, k_if[i]);
		if (sceNetSyscallIoctl(s, SIOCGIFADDR, &r) >= 0 && r.u.addr.family == SCE_NET_AF_INET)
			sn->addr[i] = __builtin_bswap32(r.u.addr.addr);
	}
	sceNetSyscallClose(s);
	sn->have = 1;
}

static void if_text(char *b, int cap, const VtwIfSnap *sn, int i)
{
	uint32_t a = sn->addr[i];
	if (!sn->ok[i]) {
		vtw_format(b, cap, "absent");
		return;
	}
	vtw_format(b, cap, "flags %u%s addr %u.%u.%u.%u", (unsigned)sn->flags[i],
	           (sn->flags[i] & IFF_UP) ? " (up)" : " (down)",
	           (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 8) & 0xff),
	           (unsigned)(a & 0xff));
}

void vtw_ifsnap_log(const char *what, const VtwIfSnap *sn)
{
	char b[3][64];
	for (int i = 0; i < VTW_IFSNAP_N; i++)
		if_text(b[i], sizeof(b[i]), sn, i);
	VTW_LOG("ifsnap %s: %s %s | %s %s | %s %s", what, k_if[0], b[0], k_if[1], b[1], k_if[2], b[2]);
}

/* other*: "inactive" = absent, or down with no address. */
static int same_if(const VtwIfSnap *a, const VtwIfSnap *b, int i)
{
	if (i < 2) {
		int ia = !a->ok[i] || (!(a->flags[i] & IFF_UP) && !a->addr[i]);
		int ib = !b->ok[i] || (!(b->flags[i] & IFF_UP) && !b->addr[i]);
		if (ia || ib)
			return ia == ib;
	}
	return a->ok[i] == b->ok[i] && a->addr[i] == b->addr[i] &&
	       (a->flags[i] & IFF_UP) == (b->flags[i] & IFF_UP);
}

void vtw_ifsnap_compare(const char *what, const VtwIfSnap *a, const VtwIfSnap *b, int tunnel_kept)
{
	if (!a->have || !b->have) {
		VTW_LOG("restore check (interfaces, %s): not taken (socket failed)", what);
		return;
	}
	int diffs = 0;
	int from = tunnel_kept ? 2 : 0;   /* other0/other1 stay up by design */
	for (int i = from; i < VTW_IFSNAP_N; i++)
		diffs += !same_if(a, b, i);
	if (!diffs) {
		VTW_LOG("restore check (interfaces, %s): identical%s", what,
		        tunnel_kept ? " (the tunnel interface is kept up by design)" : "");
		return;
	}
	VTW_LOG("restore check (interfaces, %s): DIFFERS in %d", what, diffs);
	for (int i = from; i < VTW_IFSNAP_N; i++) {
		if (same_if(a, b, i))
			continue;
		char x[64], y[64];
		if_text(x, sizeof(x), a, i);
		if_text(y, sizeof(y), b, i);
		VTW_LOG("  %s before: %s; now: %s", k_if[i], x, y);
	}
}

/*
 * Taking the tunnel interface down (IFF_UP off) broke other plugins' TCP
 * servers in SceShell: right after a Disconnect, vitacompanion's
 * FTP and command ports accepted connections and never answered, until netctl
 * happened to re-join Wi-Fi. With the interface left up (address and IFF_UP),
 * FTP survived every disconnect in testing. So by default the
 * interface stays up for as long as the module is loaded; with no route of
 * ours pointing at it, it carries nothing.
 */
void vtw_ifaddr_release(int drop_addr, int take_down)
{
	if (!g_addr)
		return;
	int s = sceNetSyscallSocket("vpnturbo_ifaddr", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
	if (s >= 0) {
		int r = 0;
		if (drop_addr) {
			IfReq req;
			memset(&req, 0, sizeof(req));
			strcpy(req.name, g_ifname);
			sin_set(&req.u.addr, g_addr);
			r = sceNetSyscallIoctl(s, SIOCDIFADDR, &req);
		}
		if (take_down) {
			IfReq fr;
			memset(&fr, 0, sizeof(fr));
			strcpy(fr.name, g_ifname);
			if (sceNetSyscallIoctl(s, SIOCGIFFLAGS, &fr) >= 0) {
				fr.u.flags &= ~IFF_UP;
				sceNetSyscallIoctl(s, SIOCSIFFLAGS, &fr);
			}
		}
		sceNetSyscallClose(s);
		VTW_LOG("ifaddr: released (address %s, interface %s) -> 0x%08X",
		        drop_addr ? "removed" : "kept", take_down ? "down" : "up", (unsigned)r);
	}
	if (drop_addr)
		g_addr = 0;
}

void vtw_ifaddr_clear(void)
{
	vtw_ifaddr_release(1, 1);
}
