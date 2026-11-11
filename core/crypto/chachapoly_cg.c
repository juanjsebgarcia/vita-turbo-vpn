/*
 * chachapoly_cg.c - OpenBSD's four tunnel AEAD entry points
 * (chacha20poly1305_encrypt/decrypt, xchacha20poly1305_encrypt/decrypt,
 * crypto/chachapoly.h) on the CRYPTOGAMS ARMv4/NEON assembly. Built only
 * with TN_CRYPTO=cryptogams; OpenBSD's chachapoly.c is then
 * compiled with those four renamed to *_ref and stays as the reference.
 *
 * Same construction as OpenBSD's (RFC 8439 section 2.8 with the protocol's
 * nonce: 32 zero bits, then the 64-bit counter little-endian): block 0 of the
 * key stream is the one-time Poly1305 key, data starts at block 1, the tag
 * covers AD || pad16 || ciphertext || pad16 || le64(adlen) || le64(ctlen),
 * and decrypt checks the tag in constant time before it writes anything.
 *
 * The assembly calls nothing outside itself and keeps no live data in
 * caller-saved VFP/NEON registers between calls (it saves d8-d15 as AAPCS
 * asks), and it is integer-only, so it raises no VFP exception flags
 * (a raised VFP exception flag panics a kernel thread).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <lib/libkern/libkern.h>

#include <crypto/chachapoly.h>
#include <tn_crypto.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <crypto/chacha_private.h>	/* hchacha20, for XChaCha20 */
#pragma GCC diagnostic pop

#ifndef TN_CRYPTO_CRYPTOGAMS
#error "chachapoly_cg.c is the TN_CRYPTO=cryptogams build"
#endif

#ifndef TN_CRYPTO_NEON
#define TN_CRYPTO_NEON 1
#endif

/* 1: the NEON entry points; 0: integer only. Read once per message. */
static volatile int tncg_neon = TN_CRYPTO_NEON ? 1 : 0;

/*
 * void chacha20_{arm,neon}(out, inp, len, const u32 key[8],
 *     const u32 counter[4])
 * counter[0] is the 32-bit block counter, counter[1..3] the 96-bit nonce.
 * key and counter are read with ldm: they must be word aligned. in and out
 * may be unaligned and may be equal.
 */
void	tncg_chacha20_arm(uint8_t *, const uint8_t *, size_t,
	    const uint32_t[8], const uint32_t[4]);
void	tncg_chacha20_neon(uint8_t *, const uint8_t *, size_t,
	    const uint32_t[8], const uint32_t[4]);

/*
 * int  poly1305_init(void *ctx, const u8 key[16])      clamps r, zeroes h
 * void poly1305_blocks[_neon](void *ctx, const u8 *inp, size_t len,
 *     u32 padbit)                                      whole 16-byte blocks
 * void poly1305_emit(void *ctx, u8 mac[16], const u32 nonce[4])
 */
int	tncg_poly1305_init(void *, const uint8_t[16]);
void	tncg_poly1305_blocks(void *, const uint8_t *, size_t, uint32_t);
void	tncg_poly1305_blocks_neon(void *, const uint8_t *, size_t, uint32_t);
void	tncg_poly1305_emit(void *, uint8_t[16], const uint32_t[4]);

typedef void	cg_chacha_fn(uint8_t *, const uint8_t *, size_t,
		    const uint32_t[8], const uint32_t[4]);
typedef void	cg_blocks_fn(void *, const uint8_t *, size_t, uint32_t);

/* The NEON ChaCha20 pays off from four blocks; below that, integer. */
#define CG_CHACHA_NEON_MIN	193

struct cg_poly {
	/* the assembly's state: hash, r, and the NEON key powers (192 bytes). */
	uint64_t	 opaque[24] __attribute__((aligned(16)));
	uint32_t	 s[4];			/* key bytes 16..31 */
	cg_blocks_fn	*blocks;
};

const char *
tn_crypto_impl(void)
{
	return tncg_neon ? "cryptogams-neon" : "cryptogams-scalar";
}

int
tn_crypto_set_neon(int on)
{
	int was = tncg_neon;

	tncg_neon = on ? 1 : 0;
	return was;
}

static void
cg_poly_init(struct cg_poly *p, const uint8_t key[32], int neon)
{
	tncg_poly1305_init(p->opaque, key);
	p->blocks = neon ? tncg_poly1305_blocks_neon : tncg_poly1305_blocks;
	memcpy(p->s, key + 16, sizeof(p->s));
}

/* Absorb len bytes, then zero padding to a 16-byte boundary. */
static void
cg_poly_padded(struct cg_poly *p, const uint8_t *in, size_t len)
{
	cg_blocks_fn *blocks = p->blocks;
	size_t full = len & ~(size_t)15, rem = len & 15;
	uint8_t last[16];

	if (full)
		blocks(p->opaque, in, full, 1);
	if (rem) {
		memset(last, 0, sizeof(last));
		memcpy(last, in + full, rem);
		blocks(p->opaque, last, sizeof(last), 1);
	}
}

static void
cg_poly_final(struct cg_poly *p, size_t ad_len, size_t ct_len,
    uint8_t tag[16])
{
	uint64_t lens[2];

	lens[0] = htole64((uint64_t)ad_len);
	lens[1] = htole64((uint64_t)ct_len);
	p->blocks(p->opaque, (const uint8_t *)lens, sizeof(lens), 1);
	tncg_poly1305_emit(p->opaque, tag, p->s);
}

/*
 * Key words, nonce words and the one-time Poly1305 key (block 0) for a
 * message; data then runs from block 1.
 */
struct cg_msg {
	uint32_t	key[8];
	uint32_t	ctr[4];
	union {
		uint8_t		b[32];
		uint32_t	w[8];
	} otk;
	struct cg_poly	poly;
	cg_chacha_fn	*chacha_long;	/* for CG_CHACHA_NEON_MIN bytes and up */
};

static void
cg_chacha(struct cg_msg *m, uint8_t *out, const uint8_t *in, size_t len)
{
	cg_chacha_fn *f = len >= CG_CHACHA_NEON_MIN ? m->chacha_long :
	    tncg_chacha20_arm;

	f(out, in, len, m->key, m->ctr);
}

static void
cg_msg_init(struct cg_msg *m, uint64_t nonce,
    const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	int neon = tncg_neon;

	m->chacha_long = neon ? tncg_chacha20_neon : tncg_chacha20_arm;
	memcpy(m->key, key, sizeof(m->key));
	m->ctr[0] = 0;
	m->ctr[1] = 0;
	m->ctr[2] = htole32((uint32_t)nonce);
	m->ctr[3] = htole32((uint32_t)(nonce >> 32));
	memset(m->otk.b, 0, sizeof(m->otk.b));
	tncg_chacha20_arm(m->otk.b, m->otk.b, sizeof(m->otk.b), m->key,
	    m->ctr);
	cg_poly_init(&m->poly, m->otk.b, neon);
	m->ctr[0] = 1;
}

void
chacha20poly1305_encrypt(uint8_t *dst, const uint8_t *src,
    const size_t src_len, const uint8_t *ad, const size_t ad_len,
    const uint64_t nonce, const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	struct cg_msg m;

	cg_msg_init(&m, nonce, key);
	cg_poly_padded(&m.poly, ad, ad_len);
	cg_chacha(&m, dst, src, src_len);
	cg_poly_padded(&m.poly, dst, src_len);
	cg_poly_final(&m.poly, ad_len, src_len, dst + src_len);
	explicit_bzero(&m, sizeof(m));
}

int
chacha20poly1305_decrypt(uint8_t *dst, const uint8_t *src,
    const size_t src_len, const uint8_t *ad, const size_t ad_len,
    const uint64_t nonce, const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	struct cg_msg m;
	uint8_t tag[CHACHA20POLY1305_AUTHTAG_SIZE];
	size_t dst_len;
	int bad;

	if (src_len < CHACHA20POLY1305_AUTHTAG_SIZE)
		return 0;
	dst_len = src_len - CHACHA20POLY1305_AUTHTAG_SIZE;

	cg_msg_init(&m, nonce, key);
	cg_poly_padded(&m.poly, ad, ad_len);
	cg_poly_padded(&m.poly, src, dst_len);
	cg_poly_final(&m.poly, ad_len, dst_len, tag);

	bad = timingsafe_bcmp(tag, src + dst_len, sizeof(tag));
	if (!bad)
		cg_chacha(&m, dst, src, dst_len);

	explicit_bzero(&m, sizeof(m));
	explicit_bzero(tag, sizeof(tag));
	return !bad;
}

/* XChaCha20-Poly1305 (cookie replies): HChaCha20 subkey, then the above. */
static void
cg_xkey(uint8_t subkey[CHACHA20POLY1305_KEY_SIZE], uint64_t *h_nonce,
    const uint8_t nonce[XCHACHA20POLY1305_NONCE_SIZE],
    const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	uint32_t derived[8];
	int i;

	memcpy(h_nonce, nonce + 16, sizeof(*h_nonce));
	*h_nonce = le64toh(*h_nonce);
	hchacha20(derived, nonce, key);
	for (i = 0; i < 8; i++)
		derived[i] = htole32(derived[i]);
	memcpy(subkey, derived, sizeof(derived));
	explicit_bzero(derived, sizeof(derived));
}

void
xchacha20poly1305_encrypt(uint8_t *dst, const uint8_t *src,
    const size_t src_len, const uint8_t *ad, const size_t ad_len,
    const uint8_t nonce[XCHACHA20POLY1305_NONCE_SIZE],
    const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	uint8_t subkey[CHACHA20POLY1305_KEY_SIZE];
	uint64_t h_nonce;

	cg_xkey(subkey, &h_nonce, nonce, key);
	chacha20poly1305_encrypt(dst, src, src_len, ad, ad_len, h_nonce,
	    subkey);
	explicit_bzero(subkey, sizeof(subkey));
}

int
xchacha20poly1305_decrypt(uint8_t *dst, const uint8_t *src,
    const size_t src_len, const uint8_t *ad, const size_t ad_len,
    const uint8_t nonce[XCHACHA20POLY1305_NONCE_SIZE],
    const uint8_t key[CHACHA20POLY1305_KEY_SIZE])
{
	uint8_t subkey[CHACHA20POLY1305_KEY_SIZE];
	uint64_t h_nonce;
	int ret;

	cg_xkey(subkey, &h_nonce, nonce, key);
	ret = chacha20poly1305_decrypt(dst, src, src_len, ad, ad_len, h_nonce,
	    subkey);
	explicit_bzero(subkey, sizeof(subkey));
	return ret;
}
