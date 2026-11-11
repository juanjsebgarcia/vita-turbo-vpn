/*
 * bench_crypto.c - host front end of tn_crypto_bench: 1420-byte packets
 * sealed and opened in place through every ChaCha20-Poly1305 path built,
 * then BLAKE2s and X25519.
 *
 *   bench_crypto [packets [len]]     defaults 20000 and 1420
 */
#include <stdlib.h>
#include <tn_core.h>
#include <tn_crypto.h>

int
main(int argc, char **argv)
{
	struct tn_crypto_bench_row rows[8];
	uint32_t packets = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 20000;
	uint32_t len = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1420;
	int i, n, bad = 0;

	if (tnshim_init() != 0)
		return 1;
	if (tn_crypto_selftest() != 0) {
		fprintf(stderr, "self-test failed\n");
		return 1;
	}
	printf("build: %s, %u packets of %u bytes\n", tn_crypto_impl(),
	    packets, len);
	printf("%-18s %10s %10s %12s %12s\n", "path", "seal MB/s", "open MB/s",
	    "seal us/pkt", "open us/pkt");
	n = tn_crypto_bench(rows, 8, packets, len);
	for (i = 0; i < n; i++) {
		struct tn_crypto_bench_row *r = &rows[i];

		if (r->bytes == 0)
			printf("%-18s %10s %10s %12.1f %12s  (%u ops)\n",
			    r->name, "-", "-", r->seal_ns / 1e3 / r->ops, "-",
			    r->ops);
		else if (r->open_ns == 0)
			printf("%-18s %10.2f %10s %12.2f %12s\n", r->name,
			    r->kbps_seal / 1e3, "-", r->seal_ns / 1e3 / r->ops,
			    "-");
		else
			printf("%-18s %10.2f %10.2f %12.2f %12.2f%s\n",
			    r->name, r->kbps_seal / 1e3, r->kbps_open / 1e3,
			    r->seal_ns / 1e3 / r->ops,
			    r->open_ns / 1e3 / r->ops, r->ok ? "" : "  FAILED");
		bad |= !r->ok;
	}
	tnshim_fini();
	return bad;
}
