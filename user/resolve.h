/*
 * resolve.h - the endpoint of a profile whose Endpoint is a hostname.
 */
#ifndef VTW_RESOLVE_H
#define VTW_RESOLVE_H

#include <stdint.h>

/* Remembers a hostname endpoint from the loaded profile ("" clears it);
 * `path` is the profile's file, used only (hashed with the host) to tell
 * whether the endpoint cache belongs to this profile. */
void vtw_endpoint_set_host(const char *path, const char *host, uint16_t port);

/* After the profile is loaded: when the endpoint cache
 * (ur0:data/vpnturbo/endpoint_cache) holds an address for this
 * profile, host and port, hands it to the kernel (vtSetEndpoint) so a
 * connect works without DNS (Lockdown after a reboot); a fresh lookup still
 * follows. Returns 1 if the cached address was set. */
int vtw_endpoint_use_cache(void);

/* Called from the worker's poll: while the kernel has no endpoint address
 * for the loaded profile, asks a separate thread to look the host up (at
 * most every 5 s), which hands the address to the kernel and caches it;
 * while the kernel runs on a cached address, looks up again (every 30 s
 * until one succeeds) to refresh it. Never blocks;
 * nothing is looked up while `state` is VT_STATE_LOCKED. */
void vtw_endpoint_poll(uint32_t kernel_endpoint_ip, uint32_t state);

#endif
