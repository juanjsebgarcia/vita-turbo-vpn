/*
 * strand_core.c - see strand_core.h. Compiled with general registers only and
 * without loop-to-library rewriting (CMakeLists.txt); tests/check_strand.sh
 * checks the object file.
 */
#include "strand_core.h"

strand_state strand_st;

#ifdef STRAND_HOST
#define RD32(a) strand_host_rd32(a)
#define RD8(a)  strand_host_rd8(a)
#define PAGE_MAPPED(va) strand_host_mapped(va)
#else
/*
 * Is the page of `va` readable now? The processor's own address translation
 * (privileged read, ATS1CPR) answers in PAR without touching the page and
 * without a system call; bit 0 of PAR is the fault flag. The pair must not be
 * split by an interrupt (its handler may translate too and overwrite PAR), so
 * interrupts are masked across the three instructions and restored to what
 * they were. Nothing else runs in between: no abort is possible.
 */
static inline int mapped_page(uint32_t va)
{
	uint32_t par, psr;

	__asm__ volatile(
		"mrs   %0, cpsr\n\t"
		"cpsid i\n\t"
		"mcr   p15, 0, %2, c7, c8, 0\n\t"
		"isb\n\t"
		"mrc   p15, 0, %1, c7, c4, 0\n\t"
		"tst   %0, #0x80\n\t"
		"bne   1f\n\t"
		"cpsie i\n\t"
		"1:"
		: "=&r"(psr), "=&r"(par)
		: "r"(va)
		: "memory", "cc");
	return !(par & 1u);
}
#define PAGE_MAPPED(va) mapped_page(va)
#define RD32(a) (*(const volatile uint32_t *)(uintptr_t)(a))
#define RD8(a)  (*(const volatile uint8_t *)(uintptr_t)(a))
#endif

#define LD(x)        __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define ST_REL(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define RESERVE_TRIES 4u
#define MASK          (STRAND_SLOTS - 1u)

#define ETH_LEN     14u
#define TCP_MIN     20u
#define TCP_RST_BIT 0x04u

static inline void count(volatile uint32_t *c)
{
	__atomic_fetch_add(c, 1u, __ATOMIC_RELAXED);
}

/* A word-aligned address with room for the fields read behind it. */
static inline int kptr_ok(uint32_t p)
{
	return !(p & 3u) && p >= STRAND_K_LO && p <= STRAND_K_HI - 0x60u;
}

/* Every page of [va, va + n) is mapped (n > 0). */
static inline int range_mapped(uint32_t va, uint32_t n)
{
	uint32_t page = va & ~0xFFFu, last = (va + n - 1u) & ~0xFFFu;

	for (;;) {
		if (!PAGE_MAPPED(page))
			return 0;
		if (page == last)
			return 1;
		page += 0x1000u;
	}
}

static inline uint32_t be16(uint32_t a)
{
	return (uint32_t)RD8(a) << 8 | RD8(a + 1);
}

static inline uint32_t be32(uint32_t a)
{
	return (uint32_t)RD8(a) << 24 | (uint32_t)RD8(a + 1) << 16 | (uint32_t)RD8(a + 2) << 8 | RD8(a + 3);
}

uint32_t strand_frame_headers(uint32_t desc, uint32_t pkt, uint32_t addr,
                              uint8_t out[STRAND_IP_MAX], uint32_t *bad)
{
	uint32_t seg, s, e, len, ip, t, ihl, n, i;

	*bad = 0;
	if (!kptr_ok(desc) || !kptr_ok(pkt) || !range_mapped(desc + STRAND_DESC_TYPE, 4u) ||
	    !range_mapped(pkt + STRAND_PKT_SEG, 4u)) {
		*bad = 1;
		return 0;
	}
	if (RD32(desc + STRAND_DESC_TYPE) != STRAND_TYPE_WLAN)
		return 0;
	seg = RD32(pkt + STRAND_PKT_SEG);
	if (!kptr_ok(seg) || !range_mapped(seg + STRAND_SEG_START, 8u)) {
		*bad = 1;
		return 0;
	}
	s = RD32(seg + STRAND_SEG_START);
	e = RD32(seg + STRAND_SEG_END);
	if (e < s || e - s > 0xFFFFu || s < STRAND_K_LO || s >= STRAND_K_HI ||
	    e - s > STRAND_K_HI - s) {
		*bad = 1;
		return 0;
	}
	len = e - s;
	if (len < ETH_LEN + 20u + TCP_MIN)
		return 0;                           /* too short to hold the headers */
	/* Everything read below is within the first 14 + 60 + 20 bytes. */
	if (!range_mapped(s, len < ETH_LEN + 60u + TCP_MIN ? len : ETH_LEN + 60u + TCP_MIN)) {
		*bad = 1;
		return 0;
	}

	if (be16(s + 12) != 0x0800u)
		return 0;                           /* not IPv4 */
	ip = s + ETH_LEN;
	if ((RD8(ip) >> 4) != 4u || RD8(ip + 9) != 6u)
		return 0;                           /* IPv4 TCP only */
	ihl = (uint32_t)(RD8(ip) & 0x0Fu) * 4u;
	if (ihl < 20u || len < ETH_LEN + ihl + TCP_MIN)
		return 0;
	if (be16(ip + 6) & 0x1FFFu)
		return 0;                           /* not the first fragment */
	if (be32(ip + 12) != addr)
		return 0;                           /* not from the former tunnel address */
	if (be16(ip + 2) < ihl + TCP_MIN)
		return 0;
	t = ip + ihl;
	if ((RD8(t + 12) >> 4) < 5u || (RD8(t + 13) & TCP_RST_BIT))
		return 0;                           /* a bad header, or a reset: never answered */

	n = ihl + TCP_MIN;
	for (i = 0; i < n; i++)
		out[i] = RD8(ip + i);
	/* The copy is a packet of its own: header only, so tcp_rst_for accepts it. */
	out[2] = (uint8_t)(n >> 8);
	out[3] = (uint8_t)n;
	out[ihl + 12] = 5u << 4;
	return n;
}

void strand_capture(uint32_t desc, uint32_t pkt)
{
	uint8_t hdr[STRAND_IP_MAX];
	uint32_t addr, gen, n, bad, h, tries, i;
	strand_slot *sl;

	/* The generation first: a disarm between the two loads then leaves the
	 * entry stamped with the old one, which the reader skips. */
	gen = LD(strand_st.gen);
	addr = LD(strand_st.addr);
	if (!addr)
		return;                             /* the common case: nothing else is touched */
	n = strand_frame_headers(desc, pkt, addr, hdr, &bad);
	if (bad)
		count(&strand_st.unreadable);
	if (!n)
		return;
	for (tries = 0; tries < RESERVE_TRIES; tries++) {
		h = LD(strand_st.head);
		if (h - LD(strand_st.tail) >= STRAND_SLOTS) {   /* full: the oldest stay, this one is lost */
			count(&strand_st.dropped);
			return;
		}
		if (__atomic_compare_exchange_n(&strand_st.head, &h, h + 1u, 0, __ATOMIC_ACQ_REL,
		                                __ATOMIC_ACQUIRE))
			break;
	}
	if (tries == RESERVE_TRIES) {           /* the other producers kept winning: never wait */
		count(&strand_st.dropped);
		return;
	}
	sl = &strand_st.slot[h & MASK];
	for (i = 0; i < n; i++)
		sl->data[i] = hdr[i];
	sl->len = n;
	sl->gen = gen;
	count(&strand_st.caught);
	ST_REL(sl->seq, h + 1u);                /* published */
}

/* ---- the reader's side (thread context) ------------------------------------ */

uint32_t strand_pop(uint8_t out[STRAND_IP_MAX])
{
	static uint32_t stuck;

	for (;;) {
		uint32_t i = strand_st.tail, n, live;
		strand_slot *sl;

		if (i == LD(strand_st.head)) {
			stuck = 0;
			return 0;
		}
		sl = &strand_st.slot[i & MASK];
		if (LD(sl->seq) != i + 1u) {        /* reserved, not yet published */
			if (++stuck < STRAND_STUCK)
				return 0;
			stuck = 0;
			count(&strand_st.lost);
			ST_REL(strand_st.tail, i + 1u);
			continue;
		}
		stuck = 0;
		n = sl->len;
		live = sl->gen == LD(strand_st.gen) && n >= 40u && n <= STRAND_IP_MAX;
		if (live)
			for (uint32_t k = 0; k < n; k++)
				out[k] = sl->data[k];
		ST_REL(strand_st.tail, i + 1u);     /* gives the slot back */
		if (live)
			return n;
	}
}

int strand_waiting(void)
{
	return strand_st.tail != LD(strand_st.head);
}

int strand_core_arm(uint32_t addr, uint32_t wlan)
{
	if (!strand_may_arm(addr, wlan))
		return 0;
	__atomic_fetch_add(&strand_st.gen, 1u, __ATOMIC_ACQ_REL);
	ST_REL(strand_st.addr, addr);
	return 1;
}

void strand_core_disarm(void)
{
	ST_REL(strand_st.addr, 0u);
	__atomic_fetch_add(&strand_st.gen, 1u, __ATOMIC_ACQ_REL);
}

void strand_core_expire(void)
{
	ST_REL(strand_st.addr, 0u);
}

/* ---- one reset per connection per second ------------------------------------ */

#define RECENT_N 16u

static struct {
	uint32_t src, dst, ports, t;
	uint8_t  used;
} g_recent[RECENT_N];
static uint32_t g_recent_w;

int strand_recent(const uint8_t *ip, uint32_t now_ms)
{
	uint32_t ihl = (uint32_t)(ip[0] & 0x0Fu) * 4u, src, dst, ports, i;
	const uint8_t *t = ip + ihl;

	src = (uint32_t)ip[12] << 24 | (uint32_t)ip[13] << 16 | (uint32_t)ip[14] << 8 | ip[15];
	dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
	ports = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
	for (i = 0; i < RECENT_N; i++) {
		if (g_recent[i].used && g_recent[i].src == src && g_recent[i].dst == dst &&
		    g_recent[i].ports == ports) {
			if (now_ms - g_recent[i].t < 1000u)
				return 1;
			g_recent[i].t = now_ms;
			return 0;
		}
	}
	i = g_recent_w++ % RECENT_N;
	g_recent[i].used = 1;
	g_recent[i].src = src;
	g_recent[i].dst = dst;
	g_recent[i].ports = ports;
	g_recent[i].t = now_ms;
	return 0;
}

#ifdef STRAND_HOST
void strand_core_reset(void)
{
	uint32_t i;

	strand_st.addr = strand_st.gen = strand_st.head = strand_st.tail = 0;
	strand_st.caught = strand_st.dropped = strand_st.unreadable = strand_st.lost = 0;
	for (i = 0; i < STRAND_SLOTS; i++)
		strand_st.slot[i].seq = 0;
	for (i = 0; i < RECENT_N; i++)
		g_recent[i].used = 0;
	g_recent_w = 0;
}
#endif
