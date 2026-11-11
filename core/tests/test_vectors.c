/*
 * test_vectors.c - the vendored primitives against published vectors
 * (see vectors.h for the sources).
 */
#include <crypto/blake2s.h>
#include <crypto/chachapoly.h>
#include <crypto/chacha_private.h>
#include <crypto/curve25519.h>
#include <crypto/poly1305.h>
#include <crypto/siphash.h>

#include "harness.h"
#include "vectors.h"

/* ---- ChaCha20 and Poly1305 (RFC 8439) ----------------------------------- */

/*
 * The RFC uses a 32-bit counter and a 96-bit nonce; chacha_private.h a
 * 64-bit counter and a 64-bit IV. The same 16 bytes either way: counter
 * word, then nonce[0..3] as the top counter word, then nonce[4..11].
 */
static void
rfc_chacha_setup(chacha_ctx *x, const uint8_t key[32], const uint8_t nonce[12],
    uint32_t counter)
{
	uint8_t ctr[8];

	ctr[0] = counter;
	ctr[1] = counter >> 8;
	ctr[2] = counter >> 16;
	ctr[3] = counter >> 24;
	memcpy(ctr + 4, nonce, 4);
	chacha_keysetup(x, key, 256);
	chacha_ivsetup(x, nonce + 4, ctr);
}

static void
test_chacha20(void)
{
	chacha_ctx x;
	uint8_t out[sizeof(rfc8439_242_pt)];
	uint8_t otk[32] = { 0 };

	/* 2.4.2: encryption, counter 1 */
	rfc_chacha_setup(&x, rfc8439_242_key, rfc8439_242_nonce, 1);
	chacha_encrypt_bytes(&x, rfc8439_242_pt, out, sizeof(out));
	CHECK_MEM(out, rfc8439_242_ct, sizeof(out), "rfc8439 2.4.2 chacha20");

	/* The same, fed in uneven pieces that are multiples of the block. */
	rfc_chacha_setup(&x, rfc8439_242_key, rfc8439_242_nonce, 1);
	chacha_encrypt_bytes(&x, rfc8439_242_pt, out, 64);
	chacha_encrypt_bytes(&x, rfc8439_242_pt + 64, out + 64,
	    sizeof(out) - 64);
	CHECK_MEM(out, rfc8439_242_ct, sizeof(out), "rfc8439 2.4.2 split");

	/* 2.6.2: Poly1305 one-time key is block 0's first 32 bytes */
	rfc_chacha_setup(&x, rfc8439_262_key, rfc8439_262_nonce, 0);
	chacha_encrypt_bytes(&x, otk, otk, sizeof(otk));
	CHECK_MEM(otk, rfc8439_262_otk, 32, "rfc8439 2.6.2 poly1305 key");
}

static const struct {
	const uint8_t *key, *msg;
	size_t len;
	const uint8_t *tag;
} poly_vec[] = {
	{ rfc8439_252_key, rfc8439_252_msg, sizeof(rfc8439_252_msg),
	  rfc8439_252_tag },
	RFC8439_A3_VECTOR(1), RFC8439_A3_VECTOR(2), RFC8439_A3_VECTOR(3),
	RFC8439_A3_VECTOR(4), RFC8439_A3_VECTOR(5), RFC8439_A3_VECTOR(6),
	RFC8439_A3_VECTOR(7), RFC8439_A3_VECTOR(8), RFC8439_A3_VECTOR(9),
	RFC8439_A3_VECTOR(10), RFC8439_A3_VECTOR(11),
};

static void
test_poly1305(void)
{
	poly1305_state st;
	uint8_t tag[16];
	size_t i, k;
	char name[64];

	for (i = 0; i < nitems(poly_vec); i++) {
		poly1305_init(&st, poly_vec[i].key);
		poly1305_update(&st, poly_vec[i].msg, poly_vec[i].len);
		poly1305_finish(&st, tag);
		snprintf(name, sizeof(name), "poly1305 vector %zu", i);
		CHECK_MEM(tag, poly_vec[i].tag, 16, name);

		/* Byte at a time: exercises the leftover buffer. */
		poly1305_init(&st, poly_vec[i].key);
		for (k = 0; k < poly_vec[i].len; k++)
			poly1305_update(&st, poly_vec[i].msg + k, 1);
		poly1305_finish(&st, tag);
		snprintf(name, sizeof(name), "poly1305 vector %zu bytewise", i);
		CHECK_MEM(tag, poly_vec[i].tag, 16, name);
	}
}

static void
poly_pad(poly1305_state *st, size_t len)
{
	static const uint8_t zero[16];

	if (len & 15)
		poly1305_update(st, zero, 16 - (len & 15));
}

/* 2.8.2 has a non-zero 32-bit fixed part, which the tunnel AEAD API
 * cannot express, so the construction is rebuilt from the primitives. */
static void
test_aead_282(void)
{
	chacha_ctx x;
	poly1305_state st;
	uint8_t nonce[12], otk[32] = { 0 }, ct[sizeof(rfc8439_282_pt)];
	uint8_t tag[16], lens[16];
	uint64_t al = sizeof(rfc8439_282_aad), cl = sizeof(ct);

	memcpy(nonce, rfc8439_282_fixed, 4);
	memcpy(nonce + 4, rfc8439_282_iv, 8);
	rfc_chacha_setup(&x, rfc8439_282_key, nonce, 0);
	chacha_encrypt_bytes(&x, otk, otk, 32);
	rfc_chacha_setup(&x, rfc8439_282_key, nonce, 1);
	chacha_encrypt_bytes(&x, rfc8439_282_pt, ct, sizeof(ct));
	CHECK_MEM(ct, rfc8439_282_ct, sizeof(ct), "rfc8439 2.8.2 ciphertext");

	poly1305_init(&st, otk);
	poly1305_update(&st, rfc8439_282_aad, al);
	poly_pad(&st, al);
	poly1305_update(&st, ct, cl);
	poly_pad(&st, cl);
	tnshim_htolem64(lens, al);
	tnshim_htolem64(lens + 8, cl);
	poly1305_update(&st, lens, 16);
	poly1305_finish(&st, tag);
	CHECK_MEM(tag, rfc8439_282_tag, 16, "rfc8439 2.8.2 tag");
}

/* A.5 has a zero fixed part: exactly the protocol's nonce layout. */
static void
test_aead_a5(void)
{
	enum { N = sizeof(rfc8439_a5_pt) };
	uint8_t sealed[N + 16], out[N + 16];
	uint64_t nonce = tnshim_lemtoh64(rfc8439_a5_nonce + 4);
	size_t i;
	int bad;

	CHECK(memcmp(rfc8439_a5_nonce, "\0\0\0\0", 4) == 0, "a5 fixed part");
	memcpy(sealed, rfc8439_a5_ct, N);
	memcpy(sealed + N, rfc8439_a5_tag, 16);

	memset(out, 0xaa, sizeof(out));
	CHECK(chacha20poly1305_decrypt(out, sealed, sizeof(sealed),
	    rfc8439_a5_aad, sizeof(rfc8439_a5_aad), nonce, rfc8439_a5_key) == 1,
	    "rfc8439 A.5 decrypt accepts");
	CHECK_MEM(out, rfc8439_a5_pt, N, "rfc8439 A.5 plaintext");

	chacha20poly1305_encrypt(out, rfc8439_a5_pt, N,
	    rfc8439_a5_aad, sizeof(rfc8439_a5_aad), nonce, rfc8439_a5_key);
	CHECK_MEM(out, sealed, sizeof(sealed), "rfc8439 A.5 encrypt");

	/* Flip each bit of ciphertext, tag and AAD in turn: all rejected,
	 * and the output buffer is left alone. */
	bad = 0;
	for (i = 0; i < sizeof(sealed) * 8; i++) {
		sealed[i / 8] ^= 1 << (i % 8);
		memset(out, 0x55, sizeof(out));
		if (chacha20poly1305_decrypt(out, sealed, sizeof(sealed),
		    rfc8439_a5_aad, sizeof(rfc8439_a5_aad), nonce,
		    rfc8439_a5_key) != 0 || out[0] != 0x55)
			bad++;
		sealed[i / 8] ^= 1 << (i % 8);
	}
	CHECK_EQ(bad, 0, "rfc8439 A.5 every ciphertext/tag bit flip rejected");
	bad = 0;
	for (i = 0; i < sizeof(rfc8439_a5_aad) * 8; i++) {
		uint8_t aad[sizeof(rfc8439_a5_aad)];
		memcpy(aad, rfc8439_a5_aad, sizeof(aad));
		aad[i / 8] ^= 1 << (i % 8);
		if (chacha20poly1305_decrypt(out, sealed, sizeof(sealed),
		    aad, sizeof(aad), nonce, rfc8439_a5_key) != 0)
			bad++;
	}
	CHECK_EQ(bad, 0, "rfc8439 A.5 every AAD bit flip rejected");
	CHECK(chacha20poly1305_decrypt(out, sealed, sizeof(sealed),
	    rfc8439_a5_aad, sizeof(rfc8439_a5_aad), nonce + 1,
	    rfc8439_a5_key) == 0, "rfc8439 A.5 wrong nonce rejected");
	CHECK(chacha20poly1305_decrypt(out, sealed, 15, NULL, 0, nonce,
	    rfc8439_a5_key) == 0, "aead shorter than a tag rejected");
}

/* ---- XChaCha20-Poly1305 (cookie replies) -------------------------------- */

static void
test_xchacha(void)
{
	enum { N = sizeof(xchacha_a31_pt) };
	uint32_t sub[8];
	uint8_t out[N + 16], back[N];

	hchacha20(sub, xchacha_221_nonce, xchacha_221_key);
	CHECK_MEM(sub, xchacha_221_out, 32, "xchacha 2.2.1 hchacha20");

	xchacha20poly1305_encrypt(out, xchacha_a31_pt, N, xchacha_a31_aad,
	    sizeof(xchacha_a31_aad), xchacha_a31_iv, xchacha_a31_key);
	CHECK_MEM(out, xchacha_a31_ct, N, "xchacha A.3.1 ciphertext");
	CHECK_MEM(out + N, xchacha_a31_tag, 16, "xchacha A.3.1 tag");

	CHECK(xchacha20poly1305_decrypt(back, out, sizeof(out),
	    xchacha_a31_aad, sizeof(xchacha_a31_aad), xchacha_a31_iv,
	    xchacha_a31_key) == 1, "xchacha A.3.1 decrypt accepts");
	CHECK_MEM(back, xchacha_a31_pt, N, "xchacha A.3.1 round trip");
	out[N + 15] ^= 0x80;
	CHECK(xchacha20poly1305_decrypt(back, out, sizeof(out),
	    xchacha_a31_aad, sizeof(xchacha_a31_aad), xchacha_a31_iv,
	    xchacha_a31_key) == 0, "xchacha tampered tag rejected");
}

/* ---- X25519 (RFC 7748) -------------------------------------------------- */

static void
test_x25519(void)
{
	uint8_t out[32], k[32], u[32], r[32], pub[32], s1[32], s2[32];
	uint8_t priv[32];
	int i;

	CHECK(curve25519(out, rfc7748_52_1_scalar, rfc7748_52_1_u) == 1 &&
	    memcmp(out, rfc7748_52_1_out, 32) == 0, "rfc7748 5.2 vector 1");
	CHECK(curve25519(out, rfc7748_52_2_scalar, rfc7748_52_2_u) == 1 &&
	    memcmp(out, rfc7748_52_2_out, 32) == 0, "rfc7748 5.2 vector 2");

	/* Iterated: k = u = 9; then r = X25519(k, u), u = k, k = r. */
	memset(k, 0, 32);
	k[0] = 9;
	memcpy(u, k, 32);
	for (i = 1; i <= 1000; i++) {
		curve25519(r, k, u);
		memcpy(u, k, 32);
		memcpy(k, r, 32);
		if (i == 1)
			CHECK_MEM(k, rfc7748_iter1, 32, "rfc7748 1 iteration");
	}
	CHECK_MEM(k, rfc7748_iter1000, 32, "rfc7748 1,000 iterations");

	/* 6.1 Diffie-Hellman */
	CHECK(curve25519_generate_public(pub, rfc7748_61_a) == 1 &&
	    memcmp(pub, rfc7748_61_ka, 32) == 0, "rfc7748 6.1 alice public");
	CHECK(curve25519_generate_public(pub, rfc7748_61_b) == 1 &&
	    memcmp(pub, rfc7748_61_kb, 32) == 0, "rfc7748 6.1 bob public");
	CHECK(curve25519(s1, rfc7748_61_a, rfc7748_61_kb) == 1 &&
	    memcmp(s1, rfc7748_61_k, 32) == 0, "rfc7748 6.1 alice shared");
	CHECK(curve25519(s2, rfc7748_61_b, rfc7748_61_ka) == 1 &&
	    memcmp(s2, rfc7748_61_k, 32) == 0, "rfc7748 6.1 bob shared");

	/* Small-order points give the all-zero secret, which is refused. */
	for (i = 0; i < 4; i++)
		CHECK(curve25519(out, rfc7748_61_a, x25519_low_order[i]) == 0,
		    "x25519 small-order point refused");

	/* Generated secrets are clamped. */
	curve25519_generate_secret(priv);
	CHECK((priv[0] & 7) == 0 && (priv[31] & 0xc0) == 0x40,
	    "generated secret clamped");
}

/* ---- BLAKE2s (RFC 7693, reference KAT) and HMAC-BLAKE2s ----------------- */

/* RFC 7693 appendix E: deterministic input generator. */
static void
selftest_seq(uint8_t *out, size_t len, uint32_t seed)
{
	uint32_t t, a = 0xDEAD4BAD * seed, b = 1;
	size_t i;

	for (i = 0; i < len; i++) {
		t = a + b;
		a = b;
		b = t;
		out[i] = (t >> 24) & 0xff;
	}
}

static void
seq(uint8_t *out, size_t n, unsigned m)
{
	size_t k;

	for (k = 0; k < n; k++)
		out[k] = (uint8_t)(7 * k + m);
}

static void
test_blake2s(void)
{
	static const size_t md_len[4] = { 16, 20, 28, 32 };
	static const size_t in_len[6] = { 0, 3, 64, 65, 255, 1024 };
	struct blake2s_state st;
	uint8_t in[1024], md[32], key[100];
	size_t i, j, n;
	int bad;

	blake2s(md, (const uint8_t *)"abc", NULL, 32, 3, 0);
	CHECK_MEM(md, rfc7693_abc, 32, "rfc7693 B blake2s-256(abc)");

	/* Appendix E grand hash over 48 unkeyed and keyed hashes. */
	blake2s_init(&st, 32);
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 6; j++) {
			selftest_seq(in, in_len[j], in_len[j]);
			blake2s(md, in, NULL, md_len[i], in_len[j], 0);
			blake2s_update(&st, md, md_len[i]);
			selftest_seq(key, md_len[i], md_len[i]);
			blake2s(md, in, key, md_len[i], in_len[j], md_len[i]);
			blake2s_update(&st, md, md_len[i]);
		}
	}
	blake2s_final(&st, md);
	CHECK_MEM(md, rfc7693_selftest_res, 32, "rfc7693 E self-test");

	/* Reference keyed KAT, all 256 lengths, one call each. */
	for (i = 0; i < 256; i++)
		in[i] = i;
	for (i = 0; i < 32; i++)
		key[i] = i;
	bad = 0;
	for (n = 0; n < 256; n++) {
		blake2s(md, in, key, 32, n, 32);
		bad += memcmp(md, blake2s_kat[n], 32) != 0;
	}
	CHECK_EQ(bad, 0, "blake2s keyed KAT 0-255");

	/* And through update() in 1-, 7- and 64-byte pieces. */
	bad = 0;
	for (n = 0; n < 256; n++) {
		static const size_t step[3] = { 1, 7, 64 };
		for (j = 0; j < 3; j++) {
			blake2s_init_key(&st, 32, key, 32);
			for (i = 0; i < n; i += step[j])
				blake2s_update(&st, in + i,
				    MIN(step[j], n - i));
			blake2s_final(&st, md);
			bad += memcmp(md, blake2s_kat[n], 32) != 0;
		}
	}
	CHECK_EQ(bad, 0, "blake2s keyed KAT, incremental");

	for (i = 0; i < nitems(blake2s_param_vec); i++) {
		seq(key, blake2s_param_vec[i].klen, 3);
		seq(in, blake2s_param_vec[i].mlen, 4);
		blake2s(md, in, blake2s_param_vec[i].klen ? key : NULL,
		    blake2s_param_vec[i].outlen, blake2s_param_vec[i].mlen,
		    blake2s_param_vec[i].klen);
		CHECK_MEM(md, blake2s_param_vec[i].out,
		    blake2s_param_vec[i].outlen, "blake2s outlen/keylen");
	}

	/* HMAC-BLAKE2s, the KDF under every Noise step. */
	for (i = 0; i < nitems(hmac_blake2s_vec); i++) {
		seq(key, hmac_blake2s_vec[i].klen, 1);
		seq(in, hmac_blake2s_vec[i].mlen, 2);
		blake2s_hmac(md, in, key, 32, hmac_blake2s_vec[i].mlen,
		    hmac_blake2s_vec[i].klen);
		CHECK_MEM(md, hmac_blake2s_vec[i].mac, 32, "hmac-blake2s");
	}
}

/* ---- SipHash-2-4 (reference vectors) ------------------------------------ */

static void
test_siphash(void)
{
	SIPHASH_KEY k;
	SIPHASH_CTX ctx;
	uint8_t key[16], in[64], out[8];
	size_t i;
	int bad = 0, bad2 = 0;

	for (i = 0; i < 16; i++)
		key[i] = i;
	for (i = 0; i < 64; i++)
		in[i] = i;
	memcpy(&k, key, sizeof(k));
	for (i = 0; i < 64; i++) {
		tnshim_htolem64(out, SipHash24(&k, in, i));
		bad += memcmp(out, siphash24_vec[i], 8) != 0;

		SipHash24_Init(&ctx, &k);
		SipHash24_Update(&ctx, in, i / 3);
		SipHash24_Update(&ctx, in + i / 3, i - i / 3);
		SipHash24_Final(out, &ctx);
		bad2 += memcmp(out, siphash24_vec[i], 8) != 0;
	}
	CHECK_EQ(bad, 0, "siphash-2-4 reference vectors 0-63");
	CHECK_EQ(bad2, 0, "siphash-2-4 reference vectors, split updates");
}

int
main(void)
{
	if (tnshim_init() != 0)
		return 1;
	test_chacha20();
	test_poly1305();
	test_aead_282();
	test_aead_a5();
	test_xchacha();
	test_x25519();
	test_blake2s();
	test_siphash();
	tnshim_fini();
	return harness_report("vectors");
}
