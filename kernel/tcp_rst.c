/*
 * tcp_rst.c - the TCP reset that answers a stranded connection.
 *
 * A TCP connection opened before the tunnel came up keeps the Wi-Fi address
 * as its source. Once the tunnel's routes are in place its packets reach the
 * tunnel interface, where they cannot be sent (the server only accepts the
 * tunnel address), so they are dropped. Dropped silently, the application
 * waits for its own timeout (a browser sits on its old page with the
 * progress bar stuck). Answered with a reset, as if from the far end, the
 * application learns at once that the connection is gone and opens a new
 * one, which gets the tunnel address. Nothing is sent anywhere: the reset
 * goes back up the console's own stack.
 *
 * Pure C, no system calls: tested on the host (kernel/tests/test_tcp_rst.c).
 */
#include "tcp_rst.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t sum16(uint32_t s, const uint8_t *p, uint32_t n)
{
	for (; n > 1; p += 2, n -= 2)
		s += rd16(p);
	if (n)
		s += (uint32_t)p[0] << 8;
	return s;
}

static uint16_t fold(uint32_t s)
{
	while (s >> 16)
		s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_ACK 0x10

int
tcp_rst_for(const uint8_t *ip, uint32_t len, uint8_t out[TCP_RST_LEN])
{
	uint32_t ihl, tot, thl, seglen, seq, ack;
	const uint8_t *t;
	uint8_t flags, *o = out, *ot = out + 20;
	uint8_t pseudo[12];
	uint32_t s;

	if (len < 20 || (ip[0] >> 4) != 4 || ip[9] != 6)
		return 0;                           /* IPv4 TCP only */
	ihl = (uint32_t)(ip[0] & 0x0F) * 4;
	tot = rd16(ip + 2);
	if (ihl < 20 || tot > len || tot < ihl + 20)
		return 0;
	if (rd16(ip + 6) & 0x1FFF)
		return 0;                           /* not the first fragment */
	t = ip + ihl;
	thl = (uint32_t)(t[12] >> 4) * 4;
	if (thl < 20 || ihl + thl > tot)
		return 0;
	flags = t[13];
	if (flags & TCP_RST)
		return 0;                           /* never answer a reset */
	seglen = tot - ihl - thl + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);

	memset(out, 0, TCP_RST_LEN);
	/* IP: from the packet's destination back to its source. */
	o[0] = 0x45;
	wr16(o + 2, TCP_RST_LEN);
	o[8] = 64;                              /* TTL */
	o[9] = 6;
	memcpy(o + 12, ip + 16, 4);
	memcpy(o + 16, ip + 12, 4);
	wr16(o + 10, fold(sum16(0, o, 20)));

	/* TCP (RFC 793, "Reset Generation"): if the segment carries an ACK,
	 * the reset takes its sequence number from that ACK; otherwise it
	 * has sequence 0 and acknowledges the segment. */
	memcpy(ot, t + 2, 2);                   /* source port = their destination */
	memcpy(ot + 2, t, 2);                   /* destination port = their source */
	if (flags & TCP_ACK) {
		seq = rd32(t + 8);
		ack = 0;
		ot[13] = TCP_RST;
	} else {
		seq = 0;
		ack = rd32(t + 4) + seglen;
		ot[13] = TCP_RST | TCP_ACK;
	}
	wr32(ot + 4, seq);
	wr32(ot + 8, ack);
	ot[12] = 5 << 4;                        /* 20-byte header */

	memcpy(pseudo, o + 12, 8);              /* source, destination */
	pseudo[8] = 0;
	pseudo[9] = 6;
	wr16(pseudo + 10, 20);
	s = sum16(0, pseudo, 12);
	s = sum16(s, ot, 20);
	wr16(ot + 16, fold(s));
	return 1;
}
