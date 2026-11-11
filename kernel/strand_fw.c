/*
 * strand_fw.c - see strand_fw.h.
 */
#include "strand_fw.h"

static const strand_fw_row g_rows[] = {
	{ "3.60",      0x1A20u, 0x1504u, 0x3ACD91AE52FF6C7Eull, 0xBBCEE31F2E915156ull },
	{ "3.61",      0x1A20u, 0x1504u, 0x589F18405C975016ull, 0xEE2814A2CFC34CAEull },
	{ "3.63-3.70", 0x1B88u, 0x166Cu, 0xC184209A1832A857ull, 0xD35F59DCB051D8ECull },
	{ "3.71-3.74", 0x1B88u, 0x166Cu, 0x7BBD5017AC4AB9CFull, 0x4FB30B58DE445E72ull },
};

#ifdef STRAND_HOST
static const strand_fw_row *g_over;
static size_t               g_over_n;

void strand_fw_override(const strand_fw_row *rows, size_t n)
{
	g_over = rows;
	g_over_n = n;
}
#endif

size_t strand_fw_count(void)
{
#ifdef STRAND_HOST
	if (g_over)
		return g_over_n;
#endif
	return sizeof(g_rows) / sizeof(g_rows[0]);
}

const strand_fw_row *strand_fw_row_at(size_t i)
{
#ifdef STRAND_HOST
	if (g_over)
		return i < g_over_n ? &g_over[i] : NULL;
#endif
	return i < sizeof(g_rows) / sizeof(g_rows[0]) ? &g_rows[i] : NULL;
}

uint64_t strand_fnv64(const uint8_t *p, uint32_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;

	for (uint32_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

const strand_fw_row *strand_fw_match(strand_read_fn rd)
{
	uint8_t b[STRAND_SIG_BYTES];

	for (size_t i = 0; i < strand_fw_count(); i++) {
		const strand_fw_row *r = strand_fw_row_at(i);

		/* Only the routine that is hooked. The receive routine holds
		 * absolute addresses (movw/movt pairs) that the loader rewrites
		 * when the module is placed, so it is not compared. The transmit
		 * routine's first 64 bytes hold none on any build 3.60-3.74. */
		if (rd(r->tx_off, b, sizeof(b)) < 0 || strand_fnv64(b, sizeof(b)) != r->tx_hash)
			continue;
		return r;
	}
	return NULL;
}
