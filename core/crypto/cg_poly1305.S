/*
 * cg_poly1305.S - CRYPTOGAMS Poly1305 (ARMv4 integer + NEON) under the
 * core's own names; OpenBSD's poly1305_init/update/finish stay as they are.
 * The generator's __KERNEL__ configuration, as in cg_chacha.S:
 *   tncg_poly1305_init         clamp r, zero h (returns 0: no function table)
 *   tncg_poly1305_blocks       integer only
 *   tncg_poly1305_blocks_neon  NEON for 64 bytes and up, else integer
 *   tncg_poly1305_emit         either representation
 */
#define __KERNEL__		1
#define __LINUX_ARM_ARCH__	7
#define poly1305_init_arm	tncg_poly1305_init
#define poly1305_blocks_arm	tncg_poly1305_blocks
#define poly1305_emit_arm	tncg_poly1305_emit
#define poly1305_blocks_neon	tncg_poly1305_blocks_neon
#include "poly1305-armv4.S"
