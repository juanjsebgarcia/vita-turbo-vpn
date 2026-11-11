/*
 * tn_crypto_portable.c - tn_crypto.h for the TN_CRYPTO=portable build:
 * OpenBSD's C is the only ChaCha20-Poly1305 and has no switch.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <tn_crypto.h>

#ifdef TN_CRYPTO_CRYPTOGAMS
#error "tn_crypto_portable.c is the TN_CRYPTO=portable build"
#endif

const char *
tn_crypto_impl(void)
{
	return "portable";
}

int
tn_crypto_set_neon(int on)
{
	(void)on;
	return -1;
}
