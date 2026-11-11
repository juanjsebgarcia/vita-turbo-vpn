/*
 * tn_crypto_bench.c - ChaCha20-Poly1305 self-test and benchmark (tn_crypto.h)
 * for the host tests and for an on-device run from the kernel module.
 *
 * Integer only, no float: a VFP exception flag panics a kernel thread,
 * and vita.mk's fpcheck enforces it. Buffers are static
 * (kernel stacks are small), so neither function is reentrant: one caller
 * at a time.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <lib/libkern/libkern.h>

#include <crypto/chachapoly.h>
#include <crypto/blake2s.h>
#include <crypto/curve25519.h>
#include <tn_crypto.h>

typedef void	seal_fn(uint8_t *, const uint8_t *, const size_t,
		    const uint8_t *, const size_t, const uint64_t,
		    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);
typedef int	open_fn(uint8_t *, const uint8_t *, const size_t,
		    const uint8_t *, const size_t, const uint64_t,
		    const uint8_t[CHACHA20POLY1305_KEY_SIZE]);

/* A path under test: the reference, or the build's own with NEON on/off. */
struct path {
	const char	*name;
	seal_fn		*seal;
	open_fn		*open;
	int		 neon;		/* -1: leave the setting alone */
};

static const struct path paths[] = {
	{ "portable-c", chacha20poly1305_encrypt_ref,
	    chacha20poly1305_decrypt_ref, -1 },
#ifdef TN_CRYPTO_CRYPTOGAMS
	{ "cryptogams-scalar", chacha20poly1305_encrypt,
	    chacha20poly1305_decrypt, 0 },
	{ "cryptogams-neon", chacha20poly1305_encrypt,
	    chacha20poly1305_decrypt, 1 },
#endif
};
#define NPATHS	((int)(sizeof(paths) / sizeof(paths[0])))

/* RFC 8439 appendix A.5: 32 zero bits then a 64-bit counter, as the tunnel protocol uses. */
static const uint8_t a5_key[32] = {
	0x1c, 0x92, 0x40, 0xa5, 0xeb, 0x55, 0xd3, 0x8a, 0xf3, 0x33, 0x88, 0x86,
	0x04, 0xf6, 0xb5, 0xf0, 0x47, 0x39, 0x17, 0xc1, 0x40, 0x2b, 0x80, 0x09,
	0x9d, 0xca, 0x5c, 0xbc, 0x20, 0x70, 0x75, 0xc0,
};
static const uint64_t a5_nonce = 0x0807060504030201ULL;
static const uint8_t a5_aad[12] = {
	0xf3, 0x33, 0x88, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4e, 0x91,
};
static const uint8_t a5_sealed[265 + 16] = {
	0x64, 0xa0, 0x86, 0x15, 0x75, 0x86, 0x1a, 0xf4, 0x60, 0xf0, 0x62, 0xc7,
	0x9b, 0xe6, 0x43, 0xbd, 0x5e, 0x80, 0x5c, 0xfd, 0x34, 0x5c, 0xf3, 0x89,
	0xf1, 0x08, 0x67, 0x0a, 0xc7, 0x6c, 0x8c, 0xb2, 0x4c, 0x6c, 0xfc, 0x18,
	0x75, 0x5d, 0x43, 0xee, 0xa0, 0x9e, 0xe9, 0x4e, 0x38, 0x2d, 0x26, 0xb0,
	0xbd, 0xb7, 0xb7, 0x3c, 0x32, 0x1b, 0x01, 0x00, 0xd4, 0xf0, 0x3b, 0x7f,
	0x35, 0x58, 0x94, 0xcf, 0x33, 0x2f, 0x83, 0x0e, 0x71, 0x0b, 0x97, 0xce,
	0x98, 0xc8, 0xa8, 0x4a, 0xbd, 0x0b, 0x94, 0x81, 0x14, 0xad, 0x17, 0x6e,
	0x00, 0x8d, 0x33, 0xbd, 0x60, 0xf9, 0x82, 0xb1, 0xff, 0x37, 0xc8, 0x55,
	0x97, 0x97, 0xa0, 0x6e, 0xf4, 0xf0, 0xef, 0x61, 0xc1, 0x86, 0x32, 0x4e,
	0x2b, 0x35, 0x06, 0x38, 0x36, 0x06, 0x90, 0x7b, 0x6a, 0x7c, 0x02, 0xb0,
	0xf9, 0xf6, 0x15, 0x7b, 0x53, 0xc8, 0x67, 0xe4, 0xb9, 0x16, 0x6c, 0x76,
	0x7b, 0x80, 0x4d, 0x46, 0xa5, 0x9b, 0x52, 0x16, 0xcd, 0xe7, 0xa4, 0xe9,
	0x90, 0x40, 0xc5, 0xa4, 0x04, 0x33, 0x22, 0x5e, 0xe2, 0x82, 0xa1, 0xb0,
	0xa0, 0x6c, 0x52, 0x3e, 0xaf, 0x45, 0x34, 0xd7, 0xf8, 0x3f, 0xa1, 0x15,
	0x5b, 0x00, 0x47, 0x71, 0x8c, 0xbc, 0x54, 0x6a, 0x0d, 0x07, 0x2b, 0x04,
	0xb3, 0x56, 0x4e, 0xea, 0x1b, 0x42, 0x22, 0x73, 0xf5, 0x48, 0x27, 0x1a,
	0x0b, 0xb2, 0x31, 0x60, 0x53, 0xfa, 0x76, 0x99, 0x19, 0x55, 0xeb, 0xd6,
	0x31, 0x59, 0x43, 0x4e, 0xce, 0xbb, 0x4e, 0x46, 0x6d, 0xae, 0x5a, 0x10,
	0x73, 0xa6, 0x72, 0x76, 0x27, 0x09, 0x7a, 0x10, 0x49, 0xe6, 0x17, 0xd9,
	0x1d, 0x36, 0x10, 0x94, 0xfa, 0x68, 0xf0, 0xff, 0x77, 0x98, 0x71, 0x30,
	0x30, 0x5b, 0xea, 0xba, 0x2e, 0xda, 0x04, 0xdf, 0x99, 0x7b, 0x71, 0x4d,
	0x6c, 0x6f, 0x2c, 0x29, 0xa6, 0xad, 0x5c, 0xb4, 0x02, 0x2b, 0x02, 0x70,
	0x9b,
	/* tag */
	0xee, 0xad, 0x9d, 0x67, 0x89, 0x0c, 0xbb, 0x22, 0x39, 0x23, 0x36, 0xfe,
	0xa1, 0x85, 0x1f, 0x38,
};
/* First and last 16 bytes of the A.5 plaintext; the rest is checked by
 * re-encrypting to a5_sealed. */
static const uint8_t a5_pt_head[16] = {	/* "Internet-Drafts " */
	0x49, 0x6e, 0x74, 0x65, 0x72, 0x6e, 0x65, 0x74, 0x2d, 0x44, 0x72, 0x61,
	0x66, 0x74, 0x73, 0x20,
};
static const uint8_t a5_pt_tail[16] = {	/* "in progress./" and a quote */
	0x69, 0x6e, 0x20, 0x70, 0x72, 0x6f, 0x67, 0x72, 0x65, 0x73, 0x73, 0x2e,
	0x2f, 0xe2, 0x80, 0x9d,
};

#define BUFSZ	(TN_CRYPTO_BENCH_MAXLEN + CHACHA20POLY1305_AUTHTAG_SIZE + 8)
static uint8_t buf_src[BUFSZ], buf_ref[BUFSZ], buf_out[BUFSZ];
static uint8_t buf_ad[64];

static uint32_t
xs32(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static void
fill(uint8_t *p, size_t n, uint32_t *s)
{
	while (n--)
		*p++ = (uint8_t)xs32(s);
}

static void
path_select(const struct path *p)
{
	if (p->neon >= 0)
		tn_crypto_set_neon(p->neon);
}

/* RFC 8439 A.5 through one path, at a buffer offset. */
static int
kat_a5(const struct path *p, size_t off)
{
	enum { N = sizeof(a5_sealed) - 16 };
	uint8_t *in = buf_src + off, *out = buf_out + off;
	int fails = 0;

	memcpy(in, a5_sealed, sizeof(a5_sealed));
	memset(out, 0xaa, sizeof(a5_sealed));
	if (p->open(out, in, sizeof(a5_sealed), a5_aad, sizeof(a5_aad),
	    a5_nonce, a5_key) != 1)
		fails++;
	if (memcmp(out, a5_pt_head, 16) || memcmp(out + N - 16, a5_pt_tail, 16))
		fails++;
	memcpy(in, out, N);
	p->seal(out, in, N, a5_aad, sizeof(a5_aad), a5_nonce, a5_key);
	if (memcmp(out, a5_sealed, sizeof(a5_sealed)))
		fails++;
	/* in place, as wg_noise does */
	p->open(out, out, sizeof(a5_sealed), a5_aad, sizeof(a5_aad), a5_nonce,
	    a5_key);
	p->seal(out, out, N, a5_aad, sizeof(a5_aad), a5_nonce, a5_key);
	if (memcmp(out, a5_sealed, sizeof(a5_sealed)))
		fails++;
	/* one flipped tag bit */
	out[N + 3] ^= 0x10;
	memcpy(in, out, sizeof(a5_sealed));
	if (p->open(out, in, sizeof(a5_sealed), a5_aad, sizeof(a5_aad),
	    a5_nonce, a5_key) != 0)
		fails++;
	return fails;
}

/* One length through one path against the reference. */
static int
cross_one(const struct path *p, size_t len, size_t ad_len, size_t off,
    uint32_t *seed)
{
	uint8_t key[32];
	uint64_t nonce;
	uint8_t *in = buf_src + off, *out = buf_out + off;
	int fails = 0;

	fill(key, sizeof(key), seed);
	nonce = ((uint64_t)xs32(seed) << 32) | xs32(seed);
	fill(buf_ad, ad_len, seed);
	fill(in, len, seed);

	chacha20poly1305_encrypt_ref(buf_ref, in, len, buf_ad, ad_len, nonce,
	    key);
	path_select(p);
	p->seal(out, in, len, buf_ad, ad_len, nonce, key);
	if (memcmp(out, buf_ref, len + 16))
		fails++;
	/* open in place what the reference sealed */
	memcpy(out, buf_ref, len + 16);
	if (p->open(out, out, len + 16, buf_ad, ad_len, nonce, key) != 1 ||
	    memcmp(out, in, len))
		fails++;
	/* a flipped bit anywhere is rejected */
	if (len) {
		memcpy(out, buf_ref, len + 16);
		out[xs32(seed) % (len + 16)] ^= (uint8_t)(1u << (xs32(seed) & 7));
		if (p->open(out, out, len + 16, buf_ad, ad_len, nonce, key) != 0)
			fails++;
	}
	return fails;
}

int
tn_crypto_selftest(void)
{
	uint32_t seed = 0x9e3779b9;
	size_t len, off;
	int i, fails = 0, neon_was = tn_crypto_set_neon(1);

	for (i = 0; i < NPATHS; i++) {
		path_select(&paths[i]);
		for (off = 0; off < 4; off++)
			fails += kat_a5(&paths[i], off);
	}
	for (i = 1; i < NPATHS; i++) {
		for (len = 0; len <= TN_CRYPTO_SELFTEST_MAXLEN;
		    len += (len < 300 ? 1 : 7)) {
			off = len & 3;
			fails += cross_one(&paths[i], len, 0, off, &seed);
			fails += cross_one(&paths[i], len, len % 37, 3 - off,
			    &seed);
		}
		fails += cross_one(&paths[i], 1420, 0, 0, &seed);
		fails += cross_one(&paths[i], 1420, 32, 1, &seed);
	}
	if (neon_was >= 0)
		tn_crypto_set_neon(neon_was);
	return fails;
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	getnanouptime(&ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* kB/s (1000 B) = bytes * 1e6 / ns, in integers. */
static uint32_t
kbps(uint64_t bytes, uint64_t ns)
{
	if (ns == 0)
		return 0;
	return (uint32_t)(bytes * 1000000ULL / ns);
}

int
tn_crypto_bench(struct tn_crypto_bench_row *rows, int maxrows,
    uint32_t packets, uint32_t len)
{
	uint8_t key[32];
	uint32_t seed = 0x12345678, n;
	uint64_t t0, t1, t2, total, seal_ns, open_ns;
	struct blake2s_state bs;
	uint8_t h[32], pt[32], sk[32];
	int i, r = 0, neon_was;

	if (len > TN_CRYPTO_BENCH_MAXLEN)
		len = TN_CRYPTO_BENCH_MAXLEN;
	if (packets == 0)
		packets = 1;
	neon_was = tn_crypto_set_neon(1);
	fill(key, sizeof(key), &seed);
	fill(buf_src, len, &seed);

	for (i = 0; i < NPATHS && r < maxrows; i++, r++) {
		struct tn_crypto_bench_row *row = &rows[r];
		int ok = 1;

		path_select(&paths[i]);
		/* warm the caches and the branch predictors */
		memcpy(buf_out, buf_src, len);
		paths[i].seal(buf_out, buf_out, len, NULL, 0, 0, key);
		paths[i].open(buf_out, buf_out, len + 16, NULL, 0, 0, key);

		/* seal in place, a new nonce per packet, as the data path */
		t0 = now_ns();
		for (n = 1; n <= packets; n++)
			paths[i].seal(buf_out, buf_out, len, NULL, 0, n, key);
		t1 = now_ns();
		seal_ns = t1 - t0;

		/* open one sealed packet `packets` times; src != dst keeps the
		 * input intact, and the work is the same as in place */
		paths[i].seal(buf_ref, buf_src, len, NULL, 0, 7, key);
		t1 = now_ns();
		for (n = 0; n < packets; n++)
			if (paths[i].open(buf_out, buf_ref, len + 16, NULL, 0,
			    7, key) != 1)
				ok = 0;
		t2 = now_ns();
		open_ns = t2 - t1;
		if (memcmp(buf_out, buf_src, len))
			ok = 0;

		total = (uint64_t)packets * len;
		row->name = paths[i].name;
		row->ops = packets;
		row->bytes = len;
		row->seal_ns = seal_ns;
		row->open_ns = open_ns;
		row->kbps_seal = kbps(total, row->seal_ns);
		row->kbps_open = kbps(total, row->open_ns);
		row->ok = ok;
	}

	if (r < maxrows) {
		struct tn_crypto_bench_row *row = &rows[r++];

		t0 = now_ns();
		for (n = 0; n < packets; n++) {
			blake2s_init(&bs, sizeof(h));
			blake2s_update(&bs, buf_src, len);
			blake2s_final(&bs, h);
		}
		t1 = now_ns();
		row->name = "blake2s";
		row->ops = packets;
		row->bytes = len;
		row->seal_ns = t1 - t0;
		row->open_ns = 0;
		row->kbps_seal = kbps((uint64_t)packets * len, row->seal_ns);
		row->kbps_open = 0;
		row->ok = 1;
	}

	if (r < maxrows) {
		struct tn_crypto_bench_row *row = &rows[r++];
		uint32_t ops = packets / 64 + 1;

		fill(sk, sizeof(sk), &seed);
		curve25519_clamp_secret(sk);
		memset(pt, 0, sizeof(pt));
		pt[0] = 9;
		t0 = now_ns();
		for (n = 0; n < ops; n++)
			curve25519(pt, sk, pt);
		t1 = now_ns();
		row->name = "x25519";
		row->ops = ops;
		row->bytes = 0;
		row->seal_ns = t1 - t0;
		row->open_ns = 0;
		row->kbps_seal = 0;
		row->kbps_open = 0;
		row->ok = 1;
	}

	if (neon_was >= 0)
		tn_crypto_set_neon(neon_was);
	explicit_bzero(key, sizeof(key));
	explicit_bzero(sk, sizeof(sk));
	return r;
}
