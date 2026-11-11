/*
 * https_get.h - one HTTPS GET with the app's own TLS: mbedTLS and the
 * embedded root bundle, for the exit-IP check. Console only.
 */
#ifndef HTTPS_GET_H
#define HTTPS_GET_H

#include <stddef.h>
#include <stdint.h>

typedef enum HgResult {
	HG_OK,
	HG_BAD_URL,      /* not https://host[:port][/path] */
	HG_RESOLVE,      /* the host name did not resolve */
	HG_CONNECT,      /* no TCP connection */
	HG_TIMEOUT,
	HG_CERT_TIME,    /* the certificate is not valid now: the clock is off */
	HG_TLS,          /* handshake failed or the certificate did not verify */
	HG_HTTP,         /* not a 200 answer, or not a readable one */
} HgResult;

/*
 * GET url and put the body (dechunked, NUL-terminated, cut to cap - 1
 * bytes) in body. Blocks for at most about timeout_ms after the lookup;
 * call it on a worker thread. *peer_ip: the address connected to, host
 * order (0 if none).
 */
/* The last lookup's resolver error (sceNetResolverStartNtoa), 0 if it
 * resolved, -1 if it answered with no address; for the app's log */
extern int hg_lookup_err;

HgResult hg_get(const char *url, char *body, size_t cap, uint32_t *peer_ip, int timeout_ms);

#endif
