/*
 * Host test for strand_core.c and strand_fw.c, run with tests/run.sh. The
 * stack's memory is a mock laid out as strand_core.h describes: a descriptor
 * (interface type at +0x58), a packet (first segment at +0x20) and a segment
 * (data start +8, data end +0xC), at kernel-window addresses.
 */
#include <stdio.h>
#include <string.h>

#define STRAND_HOST 1
#include "../strand_core.h"
#include "../strand_fw.h"
#include "../tcp_rst.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- mock memory ---------------------------------------------------------- */

#define MBASE 0x81000000u
#define MSIZE 0x6000u
static uint8_t  mem[MSIZE];
static unsigned reads, stray, faulted_reads;
static uint32_t fault_page = 0xFFFFFFFFu;   /* the translation check reports this page unmapped */

uint8_t strand_host_rd8(uint32_t a)
{
	reads++;
	if ((a & ~0xFFFu) == fault_page)
		faulted_reads++;
	if (a < MBASE || a >= MBASE + MSIZE) {
		stray++;
		return 0xFF;
	}
	return mem[a - MBASE];
}

uint32_t strand_host_rd32(uint32_t a)
{
	uint32_t v = 0;
	int i;

	reads++;
	if ((a & ~0xFFFu) == fault_page)
		faulted_reads++;
	if (a < MBASE || a + 4 > MBASE + MSIZE) {
		stray++;
		return 0xFFFFFFFFu;
	}
	for (i = 3; i >= 0; i--)
		v = v << 8 | mem[a - MBASE + (uint32_t)i];
	return v;
}

/* The translation check: every page is mapped unless a test names it. */
static unsigned map_calls;

int strand_host_mapped(uint32_t va)
{
	map_calls++;
	return (va & ~0xFFFu) != fault_page;
}

static void w32(uint32_t a, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		mem[a - MBASE + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

/* Each object in a page of its own, so a fault in one page is a fault at one step. */
#define DESC (MBASE + 0x0100u)
#define PKT  (MBASE + 0x1200u)
#define SEG  (MBASE + 0x2300u)
static uint32_t data_at = MBASE + 0x3400u;
#define DATA data_at
#define TUN  0x0A420002u          /* the former tunnel address 10.66.0.2 */

/* A frame in the mock: Ethernet + IPv4 (ihl words) + TCP, from `src`. Returns the frame length. */
static uint32_t frame(uint32_t src, int ihl, int proto, int flags, int payload)
{
	uint8_t *f = mem + (DATA - MBASE);
	int thl = 20, ipl = ihl * 4, tot = ipl + thl + payload;
	uint32_t len = 14u + (uint32_t)tot;
	uint8_t *ip = f + 14, *t = ip + ipl;

	memset(f, 0, 256);
	f[12] = 0x08; f[13] = 0x00;
	ip[0] = (uint8_t)(0x40 | ihl); ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
	ip[8] = 64; ip[9] = (uint8_t)proto;
	ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16); ip[14] = (uint8_t)(src >> 8); ip[15] = (uint8_t)src;
	ip[16] = 142; ip[17] = 250; ip[18] = 74; ip[19] = 46;
	for (int i = 20; i < ipl; i++)
		ip[i] = 0x01;                       /* options: NOPs */
	t[0] = 0xC8; t[1] = 0x1B;               /* 51227 */
	t[2] = 0x01; t[3] = 0xBB;               /* 443 */
	t[4] = 0x11; t[5] = 0x22; t[6] = 0x33; t[7] = 0x44;       /* seq */
	t[8] = 0xAA; t[9] = 0xBB; t[10] = 0xCC; t[11] = 0xDD;     /* ack */
	t[12] = (uint8_t)((thl / 4) << 4); t[13] = (uint8_t)flags;
	return len;
}

/* Descriptor, packet and segment around the frame at DATA. */
static void layout(uint32_t type, uint32_t len)
{
	memset(mem + (DESC - MBASE), 0, 0x100);
	memset(mem + (PKT - MBASE), 0, 0x100);
	memset(mem + (SEG - MBASE), 0, 0x100);
	w32(DESC + 0x58, type);
	w32(PKT + 8, len);
	w32(PKT + 0x20, SEG);
	w32(SEG + 8, DATA);
	w32(SEG + 0xC, DATA + len);
	w32(SEG + 0x10, 0);
}

static uint32_t headers(uint32_t addr, uint8_t out[STRAND_IP_MAX], uint32_t *bad)
{
	return strand_frame_headers(DESC, PKT, addr, out, bad);
}

static void test_matcher(void)
{
	uint8_t out[STRAND_IP_MAX], rst[TCP_RST_LEN];
	uint32_t bad, n, len;

	/* A Wi-Fi TCP data segment from the former tunnel address: copied. */
	len = frame(TUN, 5, 6, 0x18, 100);
	layout(2, len);
	n = headers(TUN, out, &bad);
	CHECK(n == 40 && !bad, "match: %u bytes (bad %u)", n, bad);
	CHECK(out[0] == 0x45 && out[9] == 6 && out[12] == 10 && out[15] == 2 && out[16] == 142, "ip header copied");
	CHECK(out[2] == 0 && out[3] == 40, "total length set to the copy, got %u", out[3]);
	CHECK(out[20] == 0xC8 && out[23] == 0xBB && out[24] == 0x11 && out[28] == 0xAA && out[33] == 0x18, "tcp header copied");
	CHECK(tcp_rst_for(out, n, rst) == 1, "tcp_rst_for accepts the copy");
	CHECK(rst[12] == 142 && rst[16] == 10 && rst[19] == 2, "reset goes back to the tunnel address");
	CHECK(rst[33] == 0x04 && rst[24] == 0xAA && rst[27] == 0xDD, "reset takes its sequence from their ack");

	/* A SYN with no ACK: the reset acknowledges seq + 1 (the copy has no payload to count). */
	len = frame(TUN, 5, 6, 0x02, 0);
	layout(2, len);
	n = headers(TUN, out, &bad);
	CHECK(n == 40, "syn matched");
	CHECK(tcp_rst_for(out, n, rst) == 1 && rst[33] == 0x14 && rst[31] == 0x45, "syn answered with RST|ACK seq+1, ack low byte 0x%02X", rst[31]);

	/* Not a Wi-Fi interface: every other type is left alone, and nothing past the type is read. */
	for (uint32_t type = 0; type < 8; type++) {
		if (type == 2)
			continue;
		layout(type, len);
		reads = 0;
		CHECK(headers(TUN, out, &bad) == 0 && !bad, "type %u not matched", type);
		CHECK(reads == 1, "type %u: one read (the type), got %u", type, reads);
	}

	/* Wrong source (the Wi-Fi's own address, another host). */
	len = frame(0xC0A80064u, 5, 6, 0x18, 10);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 0 && !bad, "wrong source not matched");

	/* A reset is never answered; a FIN or an ACK is. */
	len = frame(TUN, 5, 6, 0x04, 0);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 0, "RST not matched");
	len = frame(TUN, 5, 6, 0x14, 0);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 0, "RST|ACK not matched");
	len = frame(TUN, 5, 6, 0x11, 0);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 40, "FIN|ACK matched");

	/* UDP from the tunnel address is not a connection. */
	len = frame(TUN, 5, 17, 0, 40);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 0, "UDP not matched");

	/* Not IPv4 (ARP, IPv6 ethertype) and not IP version 4. */
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);
	mem[DATA - MBASE + 12] = 0x08; mem[DATA - MBASE + 13] = 0x06;
	CHECK(headers(TUN, out, &bad) == 0, "ARP ethertype not matched");
	len = frame(TUN, 5, 6, 0x18, 10);
	mem[DATA - MBASE + 14] = 0x65;
	CHECK(headers(TUN, out, &bad) == 0, "IP version 6 not matched");

	/* A later fragment has no TCP header. */
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);
	mem[DATA - MBASE + 14 + 7] = 0x10;
	CHECK(headers(TUN, out, &bad) == 0, "later fragment not matched");

	/* Truncated: the first segment ends inside the headers. */
	len = frame(TUN, 5, 6, 0x18, 100);
	layout(2, len);
	w32(SEG + 0xC, DATA + 14 + 20 + 19);
	CHECK(headers(TUN, out, &bad) == 0 && !bad, "truncated in the TCP header not matched");
	w32(SEG + 0xC, DATA + 30);
	CHECK(headers(TUN, out, &bad) == 0 && !bad, "truncated in the IP header not matched");
	w32(SEG + 0xC, DATA + 14 + 20 + 20);
	CHECK(headers(TUN, out, &bad) == 40, "exactly the headers is enough");

	/* A TCP data offset below 5 or an IP total length below the headers is garbage. */
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);
	mem[DATA - MBASE + 14 + 20 + 12] = 0x40;
	CHECK(headers(TUN, out, &bad) == 0, "tcp header length 16 not matched");
	len = frame(TUN, 5, 6, 0x18, 10);
	mem[DATA - MBASE + 14 + 3] = 30;
	CHECK(headers(TUN, out, &bad) == 0, "ip total length below the headers not matched");

	/* IP options: IHL 6 and 15 (the copy is the IP header and 20 bytes of TCP). */
	for (int ihl = 6; ihl <= 15; ihl += 9) {
		len = frame(TUN, ihl, 6, 0x18, 50);
		layout(2, len);
		n = headers(TUN, out, &bad);
		CHECK(n == (uint32_t)(ihl * 4 + 20) && n <= STRAND_IP_MAX, "IHL %d: %u bytes", ihl, n);
		CHECK(out[0] == (0x40 | ihl) && out[ihl * 4 + 1] == 0x1B, "IHL %d: ports at the right place", ihl);
		CHECK(tcp_rst_for(out, n, rst) == 1 && rst[33] == 0x04, "IHL %d: reset built", ihl);
	}
	/* IHL 15 with a segment that ends inside the TCP header. */
	len = frame(TUN, 15, 6, 0x18, 50);
	layout(2, len);
	w32(SEG + 0xC, DATA + 14 + 60 + 19);
	CHECK(headers(TUN, out, &bad) == 0, "IHL 15 truncated not matched");

	/* The translation check: a page that is not mapped is never read, wherever
	 * in the chain it is (the descriptor, the packet, the segment, the data,
	 * or the second page of a frame that straddles two). */
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);
	map_calls = 0;
	CHECK(headers(TUN, out, &bad) == 40 && !bad, "all pages mapped: matched");
	CHECK(map_calls >= 4, "the chain was checked, %u translations", map_calls);
	{
		uint32_t pages[] = { DESC, PKT, SEG, DATA };
		const char *names[] = { "descriptor", "packet", "segment", "data" };
		for (int k = 0; k < 4; k++) {
			layout(2, len);
			fault_page = pages[k] & ~0xFFFu;
			reads = stray = faulted_reads = 0;
			CHECK(headers(TUN, out, &bad) == 0 && bad, "unmapped %s page refused", names[k]);
			CHECK(faulted_reads == 0, "unmapped %s page: never read, got %u reads", names[k], faulted_reads);
			CHECK(reads == (unsigned)(k < 2 ? 0 : k == 2 ? 2 : 4),
			      "unmapped %s page: reads stop there, got %u", names[k], reads);
			fault_page = 0xFFFFFFFFu;
		}
	}
	/* A frame that straddles two pages, the second unmapped. */
	data_at = MBASE + 0x4000u - 0x20u;
	len = frame(TUN, 5, 6, 0x18, 100);
	layout(2, len);
	CHECK(headers(TUN, out, &bad) == 40 && !bad, "straddling frame, both pages mapped: matched");
	fault_page = MBASE + 0x4000u;
	CHECK(headers(TUN, out, &bad) == 0 && bad, "second page unmapped: refused");
	fault_page = MBASE + 0x3000u;            /* the frame's first page */
	CHECK(headers(TUN, out, &bad) == 0 && bad, "first page unmapped: refused");
	fault_page = 0xFFFFFFFFu;
	/* A short frame never asks about bytes it will not read (page after its end). */
	data_at = MBASE + 0x4000u - 0x40u;
	len = frame(TUN, 5, 6, 0x18, 0);         /* 54 bytes: ends inside the first page */
	layout(2, len);
	fault_page = MBASE + 0x4000u;
	CHECK(headers(TUN, out, &bad) == 40 && !bad, "a frame ending in page one never touches page two");
	fault_page = 0xFFFFFFFFu;
	data_at = MBASE + 0x3400u;
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);

	/* Pointers outside the window are never followed. */
	len = frame(TUN, 5, 6, 0x18, 10);
	layout(2, len);
	reads = stray = 0;
	CHECK(strand_frame_headers(0x00000000u, PKT, TUN, out, &bad) == 0 && bad, "null descriptor refused");
	CHECK(strand_frame_headers(DESC + 1, PKT, TUN, out, &bad) == 0 && bad, "unaligned descriptor refused");
	CHECK(strand_frame_headers(DESC, 0xD0000000u, TUN, out, &bad) == 0 && bad, "packet above the window refused");
	CHECK(strand_frame_headers(0x00000800u, PKT, TUN, out, &bad) == 0 && bad, "small descriptor refused");
	CHECK(reads == 0, "refused pointers cost no read, got %u", reads);
	w32(PKT + 0x20, 0x00000010u);
	CHECK(headers(TUN, out, &bad) == 0 && bad, "bad segment pointer refused");
	layout(2, len);
	w32(SEG + 8, 0xFFFFFFF0u);
	w32(SEG + 0xC, 0xFFFFFFF8u);
	CHECK(headers(TUN, out, &bad) == 0 && bad, "data pointer above the window refused");
	layout(2, len);
	w32(SEG + 0xC, DATA - 4);
	CHECK(headers(TUN, out, &bad) == 0 && bad, "data end before start refused");
	layout(2, len);
	w32(SEG + 0xC, DATA + 0x20000);
	CHECK(headers(TUN, out, &bad) == 0 && bad, "implausible segment length refused");
	CHECK(stray == 0, "no read outside the mock memory, got %u", stray);
}

/* ---- capture, ring ---------------------------------------------------------- */

static void push_frame(uint32_t src)
{
	uint32_t len = frame(src, 5, 6, 0x18, 10);

	layout(2, len);
	strand_capture(DESC, PKT);
}

static void test_ring(void)
{
	uint8_t out[STRAND_IP_MAX];
	uint32_t n;

	/* Not armed: nothing is read, nothing is kept. */
	strand_core_reset();
	reads = 0;
	push_frame(TUN);
	reads = 0;
	strand_capture(DESC, PKT);
	CHECK(reads == 0, "not armed: no memory read, got %u", reads);
	CHECK(!strand_waiting() && strand_st.caught == 0, "not armed: nothing kept");

	/* Armed: a matching frame is kept and popped in order; others are not. */
	strand_core_arm(TUN, 0);
	push_frame(0xC0A80064u);
	CHECK(!strand_waiting(), "other source not kept");
	push_frame(TUN);
	CHECK(strand_waiting() && strand_st.caught == 1, "kept");
	n = strand_pop(out);
	CHECK(n == 40 && out[12] == 10 && out[15] == 2, "popped %u", n);
	CHECK(strand_pop(out) == 0 && !strand_waiting(), "then empty");

	/* Wrap: three laps of the ring, read in order each lap. */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	for (uint32_t lap = 0; lap < 3; lap++) {
		for (uint32_t i = 0; i < STRAND_SLOTS; i++) {
			uint32_t len = frame(TUN, 5, 6, 0x18, 10);
			mem[DATA - MBASE + 14 + 20 + 1] = (uint8_t)(lap * 32 + i);   /* source port, low byte */
			layout(2, len);
			strand_capture(DESC, PKT);
		}
		for (uint32_t i = 0; i < STRAND_SLOTS; i++) {
			n = strand_pop(out);
			CHECK(n == 40 && out[21] == (uint8_t)(lap * 32 + i), "lap %u entry %u: got %u port byte %u", lap, i, n, out[21]);
		}
		CHECK(strand_pop(out) == 0, "lap %u: empty", lap);
	}
	CHECK(strand_st.dropped == 0 && strand_st.caught == 3 * STRAND_SLOTS, "no drops in the laps, caught %u", strand_st.caught);

	/* Overflow: the oldest stay, the rest are counted, and the ring recovers. */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	for (uint32_t i = 0; i < STRAND_SLOTS + 5; i++) {
		uint32_t len = frame(TUN, 5, 6, 0x18, 10);
		mem[DATA - MBASE + 14 + 20 + 1] = (uint8_t)i;
		layout(2, len);
		strand_capture(DESC, PKT);
	}
	CHECK(strand_st.caught == STRAND_SLOTS && strand_st.dropped == 5, "overflow: caught %u dropped %u", strand_st.caught, strand_st.dropped);
	for (uint32_t i = 0; i < STRAND_SLOTS; i++) {
		n = strand_pop(out);
		CHECK(n == 40 && out[21] == (uint8_t)i, "overflow keeps the oldest: entry %u got %u", i, out[21]);
	}
	CHECK(strand_pop(out) == 0, "empty after overflow");
	push_frame(TUN);
	CHECK(strand_pop(out) == 40, "ring works again after overflow");

	/* Entries caught under an earlier arming are not acted on (a reconnect in between). */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	push_frame(TUN);
	push_frame(TUN);
	strand_core_disarm();
	CHECK(strand_pop(out) == 0 && !strand_waiting(), "disarm: stale entries discarded");
	strand_core_arm(TUN, 0);
	push_frame(TUN);
	CHECK(strand_pop(out) == 40, "new arming: new entry served");

	/* The window closing keeps what the ring holds, and stops catching. */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	push_frame(TUN);
	strand_core_expire();
	push_frame(TUN);
	CHECK(strand_st.caught == 1, "expired: no more caught");
	CHECK(strand_pop(out) == 40 && strand_pop(out) == 0, "expired: what was caught is served");

	/* A producer that reserved a slot and never published it: the reader waits, then gives it up. */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	strand_st.head = 1;                     /* reserved, never published */
	push_frame(TUN);                        /* index 1, published */
	for (uint32_t i = 0; i < STRAND_STUCK - 1; i++)
		CHECK(strand_pop(out) == 0, "stuck: wait %u", i);
	CHECK(strand_pop(out) == 40, "stuck: given up, the next entry served");
	CHECK(strand_st.lost == 1, "stuck: counted, got %u", strand_st.lost);

	/* Unreadable pointers are counted, not kept. */
	strand_core_reset();
	strand_core_arm(TUN, 0);
	strand_capture(0, PKT);
	CHECK(strand_st.unreadable == 1 && !strand_waiting(), "unreadable counted");
}

static void test_window_and_arm(void)
{
	uint32_t t0, i;

	/* The window: open until ten minutes, whatever the clock's wrap. */
	CHECK(STRAND_WINDOW_US == 10u * 60u * 1000000u, "ten minutes");
	for (i = 0; i < 2; i++) {
		t0 = i ? 0xFFFF0000u : 1000u;          /* the second start straddles the clock's wrap */
		CHECK(strand_window_open(t0 + STRAND_WINDOW_US, t0), "open at once");
		CHECK(strand_window_open(t0 + STRAND_WINDOW_US, t0 + 61000000u), "still open after 61 s (it used to close at 60)");
		CHECK(strand_window_open(t0 + STRAND_WINDOW_US, t0 + 5u * 60u * 1000000u), "open after 5 minutes");
		CHECK(strand_window_open(t0 + STRAND_WINDOW_US, t0 + STRAND_WINDOW_US - 1u), "open a microsecond before the end");
		CHECK(!strand_window_open(t0 + STRAND_WINDOW_US, t0 + STRAND_WINDOW_US), "closed at ten minutes");
		CHECK(!strand_window_open(t0 + STRAND_WINDOW_US, t0 + STRAND_WINDOW_US + 90000000u), "closed after eleven");
	}

	/* Never armed for the Wi-Fi's own address, or for none. */
	strand_core_reset();
	CHECK(strand_core_arm(TUN, TUN) == 0 && strand_st.addr == 0, "the Wi-Fi's own address: refused");
	CHECK(strand_core_arm(0, 0) == 0 && strand_st.addr == 0, "address 0: refused");
	CHECK(strand_core_arm(TUN, 0xC0A80064u) == 1 && strand_st.addr == TUN, "another address: armed");
	CHECK(strand_core_arm(TUN, 0) == 1, "Wi-Fi address unknown: armed");
	CHECK(strand_may_arm(TUN, 0xC0A80064u) && !strand_may_arm(TUN, TUN) && !strand_may_arm(0, 5), "predicate");
	/* A refused arm catches nothing. */
	strand_core_reset();
	strand_core_arm(TUN, TUN);
	push_frame(TUN);
	CHECK(!strand_waiting() && strand_st.caught == 0, "refused arm: nothing caught");
}

static void test_recent(void)
{
	uint8_t a[STRAND_IP_MAX], b[STRAND_IP_MAX];
	uint32_t bad;

	strand_core_reset();
	frame(TUN, 5, 6, 0x18, 10);
	layout(2, 14 + 40 + 10);
	headers(TUN, a, &bad);
	memcpy(b, a, sizeof(a));
	b[21] ^= 1;                              /* another source port */
	CHECK(strand_recent(a, 1000) == 0, "first reset sent");
	CHECK(strand_recent(a, 1500) == 1, "same connection within a second: skipped");
	CHECK(strand_recent(b, 1500) == 0, "another connection: sent");
	CHECK(strand_recent(a, 2000) == 0, "a second later: sent again");
	CHECK(strand_recent(a, 2500) == 1, "and again limited");
	for (int i = 0; i < 40; i++) {           /* more connections than the table holds: still safe */
		b[21] = (uint8_t)(0x40 + i);          /* never a's port */
		strand_recent(b, 2600);
	}
	CHECK(strand_recent(a, 2600) == 0, "an evicted entry is simply sent again");
}

/* ---- the build table -------------------------------------------------------- */

#define IMG 0x3000u
static uint8_t img[IMG];

static int rd_img(uint32_t off, uint8_t *out, uint32_t n)
{
	if ((uint64_t)off + n > IMG)
		return -1;
	memcpy(out, img + off, n);
	return 0;
}

static int rd_fail(uint32_t off, uint8_t *out, uint32_t n)
{
	(void)off; (void)out; (void)n;
	return -1;
}

/* Fill a code image whose bytes at the row's two offsets are the pattern `seed` makes. */
static void make_img(const strand_fw_row *r, unsigned seed, int rx, int tx)
{
	memset(img, 0, sizeof(img));
	for (uint32_t i = 0; i < STRAND_SIG_BYTES; i++) {
		if (rx)
			img[r->rx_off + i] = (uint8_t)(seed * 7 + i);
		if (tx)
			img[r->tx_off + i] = (uint8_t)(seed * 13 + i * 3);
	}
}

static uint64_t hash_at(uint32_t off)
{
	return strand_fnv64(img + off, STRAND_SIG_BYTES);
}

static void test_table(void)
{
	/* The real table: offsets and hashes per build. */
	static const struct { const char *name; uint32_t rx, tx; uint64_t rxh, txh; } want[] = {
		{ "3.60",      0x1A20, 0x1504, 0x3ACD91AE52FF6C7Eull, 0xBBCEE31F2E915156ull },
		{ "3.61",      0x1A20, 0x1504, 0x589F18405C975016ull, 0xEE2814A2CFC34CAEull },
		{ "3.63-3.70", 0x1B88, 0x166C, 0xC184209A1832A857ull, 0xD35F59DCB051D8ECull },
		{ "3.71-3.74", 0x1B88, 0x166C, 0x7BBD5017AC4AB9CFull, 0x4FB30B58DE445E72ull },
	};
	strand_fw_row rows[4];
	size_t i;

	strand_fw_override(NULL, 0);
	CHECK(strand_fw_count() == 4, "four builds, got %zu", strand_fw_count());
	for (i = 0; i < 4; i++) {
		const strand_fw_row *r = strand_fw_row_at(i);
		CHECK(r && !strcmp(r->name, want[i].name) && r->rx_off == want[i].rx && r->tx_off == want[i].tx &&
		      r->rx_hash == want[i].rxh && r->tx_hash == want[i].txh, "row %zu matches the expected build", i);
	}
	CHECK(strand_fw_row_at(4) == NULL, "no fifth row");
	CHECK(strand_fnv64((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a 64 of \"a\"");
	CHECK(strand_fnv64((const uint8_t *)"", 0) == 0xcbf29ce484222325ull, "FNV-1a 64 of nothing");

	/* An image that is not any build's code: refused. A read that fails: refused. */
	memset(img, 0x5A, sizeof(img));
	CHECK(strand_fw_match(rd_img) == NULL, "unknown code refused");
	CHECK(strand_fw_match(rd_fail) == NULL, "unreadable code refused");

	/* Rows built from images: each is found by its hash, and only by both. */
	for (i = 0; i < 4; i++) {
		const strand_fw_row *r = strand_fw_row_at(i);
		make_img(r, (unsigned)i + 1, 1, 1);
		rows[i].name = want[i].name;
		rows[i].rx_off = r->rx_off;
		rows[i].tx_off = r->tx_off;
		rows[i].rx_hash = hash_at(r->rx_off);
		rows[i].tx_hash = hash_at(r->tx_off);
	}
	/* Same offsets in rows 0 and 1 (and 2 and 3), different bytes: the hash picks the row. */
	strand_fw_override(rows, 4);
	for (i = 0; i < 4; i++) {
		const strand_fw_row *got;
		/* Rows 0 and 1 (and 2 and 3) share offsets: only the bytes tell them apart. */
		make_img(strand_fw_row_at(i), (unsigned)i + 1, 1, 1);
		got = strand_fw_match(rd_img);
		CHECK(got == &rows[i], "row %zu found by its hash (got %s)", i, got ? got->name : "none");
	}
	/* The receive routine's bytes do not count (relocated in memory);
	 * the transmit routine's must match. */
	make_img(&rows[2], 3, 1, 0);
	CHECK(strand_fw_match(rd_img) == NULL, "receive bytes alone refused");
	make_img(&rows[2], 3, 0, 1);
	CHECK(strand_fw_match(rd_img) == &rows[2], "transmit bytes alone accepted (receive bytes relocated)");
	/* One changed byte in the hooked routine: refused. */
	make_img(&rows[2], 3, 1, 1);
	img[rows[2].tx_off + 63] ^= 1;
	CHECK(strand_fw_match(rd_img) == NULL, "one changed byte refused");
	strand_fw_override(NULL, 0);
}

int main(void)
{
	test_matcher();
	test_ring();
	test_window_and_arm();
	test_recent();
	test_table();
	printf("%s (%d checks, %d failure%s)\n", fails ? "FAILED" : "strand: all passed", checks, fails, fails == 1 ? "" : "s");
	return fails != 0;
}
