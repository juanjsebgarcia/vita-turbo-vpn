/*
 * ifaddr.h - the tunnel interface's IPv4 address, set from SceShell.
 */
#ifndef VTW_IFADDR_H
#define VTW_IFADDR_H

#include <stdint.h>

/* Gives the tunnel interface addr/prefix (host order) and brings it up.
 * Returns 0, or a negative SceNet error. */
int vtw_ifaddr_apply(uint32_t addr, uint8_t prefix);

/* Removes the address set by vtw_ifaddr_apply, if any. */
void vtw_ifaddr_clear(void);

/* The restore check's interface half: flags and IPv4
 * address of other0, other1 and wlan0, read with SIOCGIFFLAGS/SIOCGIFADDR. */
#define VTW_IFSNAP_N 3
typedef struct {
	int      have;                    /* 1 = taken */
	int      ok[VTW_IFSNAP_N];        /* SIOCGIFFLAGS answered (the interface exists) */
	uint16_t flags[VTW_IFSNAP_N];
	uint32_t addr[VTW_IFSNAP_N];      /* host order, 0 = none */
} VtwIfSnap;

/* Off/paused: optionally remove the address and/or take the interface down
 * (the default keeps both: see ifaddr.c). vtw_ifaddr_clear() = both. */
void vtw_ifaddr_release(int drop_addr, int take_down);

void vtw_ifsnap_take(VtwIfSnap *s);
/* Logs "restore check (interfaces, <what>): identical" or each difference.
 * A tunnel interface that is absent and one that is down with no address
 * count as the same (net_ps cannot detach it once attached). */
void vtw_ifsnap_compare(const char *what, const VtwIfSnap *before, const VtwIfSnap *now,
                        int tunnel_kept);   /* 1: skip the tunnel interfaces */
void vtw_ifsnap_log(const char *what, const VtwIfSnap *s);

#endif
