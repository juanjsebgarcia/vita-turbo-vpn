/*
 * strand_core.h - the part of the stranded-connection reset (strand.c) that
 * runs inside the network stack's transmit dequeue routine: the frame test
 * and the ring it writes into. Platform-free, so the host tests run the code
 * the console runs (tests/test_strand.c).
 *
 * When the tunnel goes away (pause, disconnect, a drop with the kill switch
 * off) a connection that was opened through it keeps the tunnel address as
 * its source and keeps sending over the Wi-Fi, where the router cannot answer
 * a private source: the application waits for its own timeout. This code
 * spots such a frame as it leaves, copies its IP and TCP headers into the
 * ring, and the interface thread answers each with a reset up the console's
 * own stack (tcp_rst.c), so the application reconnects at once. The frame
 * itself is not touched.
 *
 * What runs here, rules every line follows:
 *   - it runs in the thread of whoever calls the stack's driver routine (the
 *     Wi-Fi driver's send path), with the stack's locks not held by us;
 *   - no call but its own helpers: no system call, no wait, no lock of ours,
 *     no allocation, no log, no library call (the copies are loops the
 *     compiler may not turn into one: see CMakeLists.txt). tests/check_strand.sh
 *     checks the object file;
 *   - it reads the stack's memory through pointers the stack handed over: the
 *     driver descriptor (argument) and the packet (the routine's result), then
 *     the first segment of the packet, after range and alignment checks, with
 *     every length bounded, and after asking the processor whether each page
 *     it is about to read is mapped (an address-translation operation, no
 *     system call: see mapped_page in strand_core.c). A pointer outside the
 *     window, or into a page that is not mapped, is never followed; the frame
 *     is counted as unreadable;
 *   - not armed (addr == 0) it returns at once, before anything else is read.
 *
 * Stack layout, the same on every build of the stack on 3.60-3.74: the
 * descriptor holds the interface type at +0x58 (2 = Wi-Fi); the packet holds
 * its first segment at +0x20; a segment holds its data start at +8 and data
 * end at +0xC. The frame starts with an Ethernet header.
 *
 * Ring: many producers (the routine may be entered from more than one thread)
 * and one reader (the interface thread). A producer reserves the next index
 * with a compare-and-swap that fails when the ring is full (the frame is
 * dropped and counted, never overwritten), fills the slot, and publishes it by
 * storing the slot's sequence number (index + 1) last with release ordering.
 * The reader takes a slot only when its sequence number says it is published
 * (acquire), copies it out, and then moves the tail, which gives the slot
 * back. A producer that stalls inside its slot holds the reader at that slot;
 * the reader gives it up after STRAND_STUCK polls rather than wait forever.
 */
#ifndef VT_STRAND_CORE_H
#define VT_STRAND_CORE_H

#include <stdint.h>

#define STRAND_IP_MAX   80u      /* IP header (up to 60 bytes) + 20 bytes of TCP */
#define STRAND_SLOTS    16u      /* a power of two */
#define STRAND_STUCK    8u       /* polls an unpublished slot is waited for */

/* Layout of the stack's objects (see above). */
#define STRAND_DESC_TYPE  0x58u
#define STRAND_TYPE_WLAN  2u
#define STRAND_PKT_SEG    0x20u
#define STRAND_SEG_START  0x08u
#define STRAND_SEG_END    0x0Cu

/* Addresses followed: not null or small, below the top of the kernel's range. */
#define STRAND_K_LO       0x00100000u
#define STRAND_K_HI       0xC0000000u

/* Armed until the tunnel comes back (connect, resume, the end of a pause, the
 * tunnel routes added), but never longer than this: a connection that is
 * going to send again does so well within minutes, and nothing is caught
 * after that. */
#define STRAND_WINDOW_US  600000000u         /* 10 minutes */

/* 1 while the window is open: `until` and `now` are low words of an
 * microsecond clock (wrap-safe for windows under half the range). */
static inline int strand_window_open(uint32_t until, uint32_t now)
{
	return (int32_t)(until - now) > 0;
}

/* Never arm for the Wi-Fi's own address: its connections are alive, and the
 * reset would kill them. */
static inline int strand_may_arm(uint32_t addr, uint32_t wlan)
{
	return addr != 0 && addr != wlan;
}

typedef struct {
	volatile uint32_t seq;        /* index + 1 once the slot is published */
	uint32_t          gen;        /* the arming it was caught under       */
	uint32_t          len;        /* bytes in data                        */
	uint8_t           data[STRAND_IP_MAX];
} strand_slot;

typedef struct {
	volatile uint32_t addr;       /* the former tunnel address (host order); 0 = not armed */
	volatile uint32_t gen;        /* bumped at every arm and disarm                         */
	volatile uint32_t head;       /* next index to reserve                                  */
	volatile uint32_t tail;       /* next index to read (reader only)                       */
	volatile uint32_t caught;     /* frames put in the ring                                 */
	volatile uint32_t dropped;    /* frames lost: ring full, or producers kept colliding    */
	volatile uint32_t unreadable; /* frames whose pointers were outside the window          */
	volatile uint32_t lost;       /* slots the reader gave up on                            */
	strand_slot       slot[STRAND_SLOTS];
} strand_state;

extern strand_state strand_st;

/*
 * The test and the copy, split from the ring for the tests: if the frame the
 * packet `pkt` holds (sent through the interface `desc`) is a Wi-Fi TCP
 * segment of IPv4 whose source is `addr` and that is not a reset, copy its
 * IP and TCP headers into out (the IP header, then 20 bytes of TCP, with the
 * IP total length and the TCP data offset set to match, which keeps the
 * sequence numbers, ports, addresses and flags) and return the length;
 * otherwise 0. *bad is set when a pointer was outside the window.
 */
uint32_t strand_frame_headers(uint32_t desc, uint32_t pkt, uint32_t addr,
                              uint8_t out[STRAND_IP_MAX], uint32_t *bad);

/* The hook's call, after the routine returned `pkt` for `desc`. */
void strand_capture(uint32_t desc, uint32_t pkt);

/* Reader side (the interface thread only). Copies the next live entry out;
 * returns its length, or 0 when there is none. Entries caught under another
 * arming than the current one are skipped. */
uint32_t strand_pop(uint8_t out[STRAND_IP_MAX]);

/* Entries waiting (reader side). */
int strand_waiting(void);

/* Arm for `addr` (refused, 0 returned, when strand_may_arm says no; `wlan` is
 * the Wi-Fi's current address, 0 = unknown) / disarm (any thread). Each
 * starts a new generation, so what the ring still holds from before is not
 * acted on. */
int  strand_core_arm(uint32_t addr, uint32_t wlan);
void strand_core_disarm(void);
/* The window ran out: frames are no longer caught, but what the ring holds
 * is still answered (same generation). */
void strand_core_expire(void);

/* 1 if a reset for this connection was already sent in the last second
 * (headers as strand_pop gives them); otherwise records it and returns 0.
 * `now_ms` is any millisecond clock. Reader side. */
int strand_recent(const uint8_t *ip, uint32_t now_ms);

#ifdef STRAND_HOST
uint32_t strand_host_rd32(uint32_t a);
uint8_t  strand_host_rd8(uint32_t a);
int      strand_host_mapped(uint32_t va);   /* the translation check: 1 mapped, 0 fault */
void     strand_core_reset(void);   /* tests: back to the start state */
#endif

#endif /* VT_STRAND_CORE_H */
