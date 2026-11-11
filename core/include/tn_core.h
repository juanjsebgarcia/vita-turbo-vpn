/*
 * tn_core.h - what the interface layer sees of the protocol core.
 *
 * The Noise handshake, transport keys and replay window (wg_noise.h) and
 * the cookie/rate-limit machinery (wg_cookie.h) are OpenBSD's, unchanged.
 * This header adds the timer constants and wire formats that live in
 * OpenBSD's if_wg.c, which is not vendored.
 *
 * Build any file that includes this with the same flags as the core (see
 * core.mk): -include tn_shim.h, shim/include and third_party/openbsd/sys on
 * the include path, and TN_SHIM_HOST or TN_SHIM_VITA.
 */
#ifndef TN_CORE_H
#define TN_CORE_H

#include "tn_shim.h"
#include <net/wg_noise.h>
#include <net/wg_cookie.h>

/*
 * From OpenBSD sys/net/if_wg.c (v1.50, same commit as the vendored files).
 * The keypair limits (REKEY_AFTER_TIME 120, REKEY_AFTER_TIME_RECV 165,
 * REJECT_AFTER_TIME 180, REKEY_AFTER_MESSAGES, REJECT_AFTER_MESSAGES) and
 * COOKIE_SECRET_MAX_AGE 120 come from wg_noise.h and wg_cookie.h.
 */
#define REKEY_TIMEOUT		5	/* s */
#define REKEY_TIMEOUT_JITTER	334	/* ms, arc4random_uniform bound */
#define KEEPALIVE_TIMEOUT	10	/* s */
#define MAX_TIMER_HANDSHAKES	(90 / REKEY_TIMEOUT)
#define NEW_HANDSHAKE_TIMEOUT	(REKEY_TIMEOUT + KEEPALIVE_TIMEOUT)
#define UNDERLOAD_TIMEOUT	1	/* s */
#define DEFAULT_MTU		1420

/* Message types, little-endian on the wire. */
#define TN_PKT_INITIATION	htole32(1)
#define TN_PKT_RESPONSE		htole32(2)
#define TN_PKT_COOKIE		htole32(3)
#define TN_PKT_DATA		htole32(4)

#define TN_PKT_WITH_PADDING(n)	(((n) + (16 - 1)) & (~(16 - 1)))

struct tn_pkt_initiation {
	uint32_t		t;
	uint32_t		s_idx;
	uint8_t			ue[NOISE_PUBLIC_KEY_LEN];
	uint8_t			es[NOISE_PUBLIC_KEY_LEN + NOISE_AUTHTAG_LEN];
	uint8_t			ets[NOISE_TIMESTAMP_LEN + NOISE_AUTHTAG_LEN];
	struct cookie_macs	m;
};

struct tn_pkt_response {
	uint32_t		t;
	uint32_t		s_idx;
	uint32_t		r_idx;
	uint8_t			ue[NOISE_PUBLIC_KEY_LEN];
	uint8_t			en[0 + NOISE_AUTHTAG_LEN];
	struct cookie_macs	m;
};

struct tn_pkt_cookie {
	uint32_t		t;
	uint32_t		r_idx;
	uint8_t			nonce[COOKIE_NONCE_SIZE];
	uint8_t			ec[COOKIE_ENCRYPTED_SIZE];
};

struct tn_pkt_data {
	uint32_t		t;
	uint32_t		r_idx;
	uint8_t			nonce[sizeof(uint64_t)];
	uint8_t			buf[];
};

/* Wire sizes fixed by the protocol: 148, 92, 64 and 16 bytes. */
_Static_assert(sizeof(struct tn_pkt_initiation) == 148, "initiation size");
_Static_assert(sizeof(struct tn_pkt_response) == 92, "response size");
_Static_assert(sizeof(struct tn_pkt_cookie) == 64, "cookie size");
_Static_assert(sizeof(struct tn_pkt_data) == 16, "data header size");

#endif /* TN_CORE_H */
