/*
 * dns.h - DNS override. See dns.c.
 */
#ifndef TN_DNS_H
#define TN_DNS_H

#include <stdint.h>
#include <psp2kern/types.h>

/* Module start: hook net_ps's control syscall. A failure is logged and the
 * module carries on without the override. */
void dns_init(void);
void dns_fini(void);

/* The loaded profile's DNS = servers (VtProfile.dns, host order, 0 = none).
 * Called from load_profile; takes effect on the next resolver read. */
void dns_set_profile(const uint32_t dns[2]);

/* The loaded profile's DNS servers (host order, 0 = none), for the routes
 * (routes.c, the profile DNS /31 into the tunnel). */
void dns_profile(uint32_t out[2]);

/* SceShell's endpoint-lookup thread, which keeps the system's servers while
 * traffic is held (never while LOCKED); vtSetEndpoint(0, VT_EP_LOOKUP_THREAD). */
void dns_set_lookup_thread(SceUID pid, SceUID tid);

/* Re-derive the mode now (disconnect, pause, options, the timer): logs a
 * switch and starts a flush generation at once. Try-lock only, no wait. */
void dns_refresh(const char *why);

/* The restore check's view (MODE_*: 0 system, 1 profile, 2 blocked). */
struct dns_snap {
	int      hooked, mode, sys_seen;
	uint32_t eff[2], sys[2];
};
void dns_snapshot(struct dns_snap *d);

#endif /* TN_DNS_H */
