/*
 * tn_crypto.h - which ChaCha20-Poly1305 the core was built with, the
 * portable reference that is always linked beside it, and a
 * self-test and benchmark that run on the host and in the Vita kernel.
 *
 * Build switch (core.mk): TN_CRYPTO=cryptogams or TN_CRYPTO=portable.
 *
 *   portable    OpenBSD's chachapoly.c, chacha_private.h and poly1305.c as
 *               they are. chacha20poly1305_encrypt/decrypt are those.
 *   cryptogams  CRYPTOGAMS ARMv4/NEON assembly behind the same four
 *               OpenBSD entry points (crypto/chachapoly_cg.c). OpenBSD's C is
 *               still compiled, renamed to *_ref, as the reference.
 *
 * In the cryptogams build the NEON paths are selected at run time by one
 * word (tn_crypto_set_neon); the default is on (TN_CRYPTO_NEON=1 in
 * core.mk), because every Vita core has NEON. Off gives CRYPTOGAMS'
 * integer-only paths.
 *
 * Include after the core's flags (-include tn_shim.h), like tn_core.h.
 */
#ifndef TN_CRYPTO_H
#define TN_CRYPTO_H

#include <crypto/chachapoly.h>

/* "portable", "cryptogams-neon" or "cryptogams-scalar". */
const char	*tn_crypto_impl(void);

/*
 * cryptogams build: 1 selects the NEON paths, 0 the integer-only ones;
 * returns the previous setting. portable build: does nothing, returns -1.
 * Takes effect at the next call; both paths give identical output, so it is
 * safe to flip while traffic runs.
 */
int		 tn_crypto_set_neon(int on);

#ifdef TN_CRYPTO_CRYPTOGAMS
/* OpenBSD's C, renamed when chachapoly.c is compiled (core.mk). */
void	chacha20poly1305_encrypt_ref(uint8_t *, const uint8_t *, const size_t,
	    const uint8_t *, const size_t, const uint64_t,
	    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);
int	chacha20poly1305_decrypt_ref(uint8_t *, const uint8_t *, const size_t,
	    const uint8_t *, const size_t, const uint64_t,
	    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);
void	xchacha20poly1305_encrypt_ref(uint8_t *, const uint8_t *,
	    const size_t, const uint8_t *, const size_t,
	    const uint8_t[XCHACHA20POLY1305_NONCE_SIZE],
	    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);
int	xchacha20poly1305_decrypt_ref(uint8_t *, const uint8_t *,
	    const size_t, const uint8_t *, const size_t,
	    const uint8_t[XCHACHA20POLY1305_NONCE_SIZE],
	    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);
#else
#define chacha20poly1305_encrypt_ref	chacha20poly1305_encrypt
#define chacha20poly1305_decrypt_ref	chacha20poly1305_decrypt
#define xchacha20poly1305_encrypt_ref	xchacha20poly1305_encrypt
#define xchacha20poly1305_decrypt_ref	xchacha20poly1305_decrypt
#endif

/*
 * Known-answer test (RFC 8439 A.5, the protocol's nonce layout) and a
 * cross-check of every built path against the reference over lengths
 * 0..TN_CRYPTO_SELFTEST_MAXLEN at buffer offsets 0..3, in place and not.
 * Returns 0, or the number of failures. Integer only; kernel-safe. Uses
 * about 3.2 KiB of stack.
 */
#define TN_CRYPTO_SELFTEST_MAXLEN	1440
int	tn_crypto_selftest(void);

/*
 * Benchmark: `packets` packets of `len` bytes (1420 is the tunnel MTU)
 * sealed in place, then opened in place, with one key and a counting nonce,
 * as the data path does. One row per path built (reference, CRYPTOGAMS
 * scalar, CRYPTOGAMS NEON), then one row each for the handshake primitives
 * (BLAKE2s over `len` bytes, `packets` times; X25519, packets/64 + 1 scalar
 * multiplications).
 *
 * Times are nanoseconds from getnanouptime (microsecond resolution on the
 * Vita). kbps_* is throughput in kilobytes (1000 B) per second, computed in
 * integers. Blocks the calling thread for the whole run: on the Vita, call
 * it from a kernel thread of the module (module_start or a syscall), not
 * from an interrupt, and with the tunnel down.
 *
 * Returns the number of rows written (at most `maxrows`).
 */
struct tn_crypto_bench_row {
	const char	*name;
	uint32_t	 ops;		/* operations timed */
	uint32_t	 bytes;		/* bytes per operation (0: X25519) */
	uint64_t	 seal_ns;	/* AEAD: encrypt; others: the operation */
	uint64_t	 open_ns;	/* AEAD: decrypt; others: 0 */
	uint32_t	 kbps_seal;
	uint32_t	 kbps_open;
	int		 ok;		/* AEAD: every packet opened and matched */
};
#define TN_CRYPTO_BENCH_MAXLEN	2048
int	tn_crypto_bench(struct tn_crypto_bench_row *rows, int maxrows,
	    uint32_t packets, uint32_t len);

#endif /* TN_CRYPTO_H */
