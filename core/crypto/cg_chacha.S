/*
 * cg_chacha.S - CRYPTOGAMS ChaCha20 (ARMv4 integer + NEON) under the core's
 * own names. Assembled with -I../third_party/cryptogams/generated (core.mk).
 *
 * Built in the generator's __KERNEL__ configuration: no run-time CPU probe
 * (OPENSSL_armcap_P) inside the assembly, so the code has no PC-relative
 * reference to data in another segment, and both entry points are exported.
 * chachapoly_cg.c picks one per call:
 *   tncg_chacha20_arm   integer only, any length
 *   tncg_chacha20_neon  3xNEON + 1xIALU; CRYPTOGAMS suggests len >= 193
 */
#define __KERNEL__		1
#define __LINUX_ARM_ARCH__	7
#define chacha20_arm		tncg_chacha20_arm
#define chacha20_neon		tncg_chacha20_neon
#include "chacha-armv4.S"
