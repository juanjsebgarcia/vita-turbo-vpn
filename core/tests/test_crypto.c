/*
 * test_crypto.c - the ChaCha20-Poly1305 build switch (tn_crypto.h).
 *
 * Runs tn_crypto_selftest (RFC 8439 A.5 through every path at four buffer
 * offsets, plus a sweep of lengths against the reference), then a longer
 * randomised cross-check of the build's paths against OpenBSD's C: random
 * lengths 0..2048, AD 0..64, unaligned buffers, in place and not, both NEON
 * settings, and XChaCha20-Poly1305. In the portable build the paths are the
 * reference itself, so this only shows that the harness runs.
 */
#include <stdlib.h>
#include <tn_core.h>
#include <tn_crypto.h>
#include "harness.h"

#define MAXLEN	2048
#define ITERS	20000

static uint8_t src[MAXLEN + 32], ref[MAXLEN + 32], out[MAXLEN + 32];

static uint32_t seed = 0xc0ffee11;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static void
fill(uint8_t *p, size_t n)
{
	while (n--)
		*p++ = (uint8_t)rnd();
}

static int
cross_random(int neon)
{
	uint8_t key[32], ad[64], xnonce[24];
	int i, bad = 0;

	tn_crypto_set_neon(neon);
	for (i = 0; i < ITERS; i++) {
		size_t len = rnd() % (MAXLEN + 1), adl = rnd() % 65;
		size_t so = rnd() & 7, oo = rnd() & 7;
		int inplace = rnd() & 1;
		uint64_t nonce = ((uint64_t)rnd() << 32) | rnd();
		uint8_t *in = src + so, *o = inplace ? src + so : out + oo;

		if (i < 64)		/* the corners first */
			len = (size_t)i * 16 + (i & 1) * 15;
		if (len > MAXLEN)
			len = MAXLEN;
		fill(key, sizeof(key));
		fill(ad, adl);
		fill(in, len);
		chacha20poly1305_encrypt_ref(ref, in, len, ad, adl, nonce, key);
		chacha20poly1305_encrypt(o, in, len, ad, adl, nonce, key);
		if (memcmp(o, ref, len + 16)) {
			bad++;
			continue;
		}
		if (!inplace)
			memcpy(in, o, len + 16);
		if (chacha20poly1305_decrypt(o, in, len + 16, ad, adl, nonce,
		    key) != 1 ||
		    chacha20poly1305_decrypt_ref(ref, ref, len + 16, ad, adl,
		    nonce, key) != 1 || memcmp(o, ref, len))
			bad++;

		if ((i & 15) == 0) {
			fill(xnonce, sizeof(xnonce));
			xchacha20poly1305_encrypt_ref(ref, src, len, ad, adl,
			    xnonce, key);
			xchacha20poly1305_encrypt(out, src, len, ad, adl, xnonce,
			    key);
			if (memcmp(out, ref, len + 16) ||
			    xchacha20poly1305_decrypt(out, ref, len + 16, ad,
			    adl, xnonce, key) != 1 || memcmp(out, src, len))
				bad++;
		}
	}
	return bad;
}

int
main(void)
{
	int f;

	if (tnshim_init() != 0)
		return 1;
	printf("crypto: %s\n", tn_crypto_impl());
	f = tn_crypto_selftest();
	if (f)
		fprintf(stderr, "tn_crypto_selftest: %d failures\n", f);
	CHECK_EQ(f, 0, "tn_crypto_selftest");
	CHECK_EQ(cross_random(1), 0, "random cross-check, NEON on");
	CHECK_EQ(cross_random(0), 0, "random cross-check, NEON off");
	tn_crypto_set_neon(1);
	tnshim_fini();
	return harness_report("crypto");
}
