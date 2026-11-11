/*
 * strand_fw.h - where the network stack's transmit dequeue routine is, per
 * build of that stack, and a hash of the code bytes there (strand.c hooks it).
 * Platform-free data and pure functions, so the rule "no matching row, no
 * hook" is tested on the host (tests/test_strand.c); strand.c reads the
 * module's code and installs the hook.
 *
 * The gate is the code, never the version: a row matches when the first
 * STRAND_SIG_BYTES bytes at its transmit offset hash (FNV-1a, 64 bits) to the row's transmit hash. Offsets
 * are from the start of the module's first (code) segment, as taiHEN's
 * offset hooks take them. The receive routine's hash is kept in the table
 * but not compared: that routine holds absolute addresses that the loader
 * rewrites when the module is placed.
 *
 * Four builds of the stack exist on firmware 3.60-3.74: 3.60, 3.61, 3.63-3.70
 * and 3.71-3.74. The table holds offsets and hashes only, never code.
 */
#ifndef VT_STRAND_FW_H
#define VT_STRAND_FW_H

#include <stddef.h>
#include <stdint.h>

#define STRAND_MODULE     "SceNetPs"
#define STRAND_SEG_CODE   0u
#define STRAND_SIG_BYTES  64u

typedef struct {
	const char *name;      /* the firmware range, for the log                       */
	uint32_t    rx_off;    /* the receive indicate routine (only in the signature)  */
	uint32_t    tx_off;    /* the transmit dequeue routine (the one hooked)         */
	uint64_t    rx_hash;   /* FNV-1a of STRAND_SIG_BYTES bytes at rx_off            */
	uint64_t    tx_hash;
} strand_fw_row;

size_t               strand_fw_count(void);
const strand_fw_row *strand_fw_row_at(size_t i);

uint64_t strand_fnv64(const uint8_t *p, uint32_t n);

/* Reads `n` bytes at `off` of the code segment into out: 0, or negative. */
typedef int (*strand_read_fn)(uint32_t off, uint8_t *out, uint32_t n);

/* The row whose code the stack has, or NULL (no row, a read that failed, any doubt). */
const strand_fw_row *strand_fw_match(strand_read_fn rd);

#ifdef STRAND_HOST
/* Host tests: use this table instead (NULL, 0 puts the real one back). */
void strand_fw_override(const strand_fw_row *rows, size_t n);
#endif

#endif /* VT_STRAND_FW_H */
