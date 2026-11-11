/* Host test for tcp_rst.c: cc -I.. test_tcp_rst.c ../tcp_rst.c && ./a.out */
#include <stdio.h>
#include <string.h>
#include "../tcp_rst.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const uint8_t *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }

/* One's-complement sum; a correct header including its checksum gives 0xFFFF. */
static unsigned csum(const uint8_t *a, int n, const uint8_t *b, int m)
{
	unsigned long s = 0;
	int i;
	for (i = 0; i + 1 < n; i += 2) s += be16(a + i);
	for (i = 0; i + 1 < m; i += 2) s += be16(b + i);
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return (unsigned)s;
}

/* An IPv4/TCP packet 192.0.2.10:50000 -> 198.51.100.145:80 with `payload`
 * bytes, the given flags, seq and ack, and a 12-byte option block. */
static int pkt(uint8_t *p, int flags, unsigned long seq, unsigned long ack, int payload)
{
	int thl = 32, tot = 20 + thl + payload;
	memset(p, 0, 200);
	p[0] = 0x45; p[2] = (uint8_t)(tot >> 8); p[3] = (uint8_t)tot; p[8] = 64; p[9] = 6;
	p[12] = 192; p[13] = 0; p[14] = 2; p[15] = 10;
	p[16] = 198; p[17] = 51; p[18] = 100; p[19] = 145;
	uint8_t *t = p + 20;
	t[0] = 0xC3; t[1] = 0x50;            /* 50000 */
	t[2] = 0; t[3] = 80;
	t[4] = (uint8_t)(seq >> 24); t[5] = (uint8_t)(seq >> 16); t[6] = (uint8_t)(seq >> 8); t[7] = (uint8_t)seq;
	t[8] = (uint8_t)(ack >> 24); t[9] = (uint8_t)(ack >> 16); t[10] = (uint8_t)(ack >> 8); t[11] = (uint8_t)ack;
	t[12] = (uint8_t)((thl / 4) << 4); t[13] = (uint8_t)flags;
	return tot;
}

static void common(const uint8_t *r)
{
	uint8_t pseudo[12];
	CHECK(r[0] == 0x45 && be16(r + 2) == 40 && r[9] == 6, "ip header");
	CHECK(csum(r, 20, NULL, 0) == 0xFFFF, "ip checksum");
	CHECK(r[12] == 198 && r[15] == 145 && r[16] == 192 && r[19] == 10, "addresses swapped");
	CHECK(be16(r + 20) == 80 && be16(r + 22) == 50000, "ports swapped");
	CHECK((r[32] >> 4) == 5, "tcp header length");
	memcpy(pseudo, r + 12, 8); pseudo[8] = 0; pseudo[9] = 6; pseudo[10] = 0; pseudo[11] = 20;
	CHECK(csum(pseudo, 12, r + 20, 20) == 0xFFFF, "tcp checksum");
}

int main(void)
{
	uint8_t p[200], r[TCP_RST_LEN];
	int n;

	/* An established connection's data segment (ACK set): RST, seq = their ack. */
	n = pkt(p, 0x18 /* PSH|ACK */, 1000, 0xDEADBEEFul, 100);
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 1, "data segment answered");
	common(r);
	CHECK(r[33] == 0x04, "flags RST only, got 0x%02X", r[33]);
	CHECK(be32(r + 24) == 0xDEADBEEFul, "seq = their ack");
	CHECK(be32(r + 28) == 0, "ack 0");

	/* A SYN (no ACK): RST|ACK, seq 0, ack = their seq + 1. */
	n = pkt(p, 0x02, 0xFFFFFFFFul, 0, 0);
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 1, "syn answered");
	common(r);
	CHECK(r[33] == 0x14, "flags RST|ACK, got 0x%02X", r[33]);
	CHECK(be32(r + 24) == 0, "seq 0");
	CHECK(be32(r + 28) == 0, "ack wraps: 0xFFFFFFFF + 1 = 0, got %lu", be32(r + 28));

	/* FIN without ACK and with 5 bytes: ack = seq + 5 + 1. */
	n = pkt(p, 0x01, 500, 0, 5);
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 1, "fin answered");
	CHECK(be32(r + 28) == 506, "ack = seq + len + fin, got %lu", be32(r + 28));

	/* Never answered: a reset, UDP, a later fragment, a truncated packet. */
	n = pkt(p, 0x14, 1, 1, 0);
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 0, "reset not answered");
	n = pkt(p, 0x18, 1, 1, 0); p[9] = 17;
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 0, "udp not answered");
	n = pkt(p, 0x18, 1, 1, 0); p[7] = 0x10;
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 0, "later fragment not answered");
	n = pkt(p, 0x18, 1, 1, 0);
	CHECK(tcp_rst_for(p, 30, r) == 0, "truncated not answered");
	p[32] = 0x20; /* header length 8 bytes */
	CHECK(tcp_rst_for(p, (uint32_t)n, r) == 0, "bad tcp header length not answered");

	printf("%s (%d failure%s)\n", fails ? "FAILED" : "tcp_rst: all passed", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
