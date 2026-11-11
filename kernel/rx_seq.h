/*
 * rx_seq.h - loss and reordering seen on the received data messages.
 * Header-only, so host_test/tn_pair_test.c
 * runs the same code (pair.sh).
 *
 * A data message carries the receiver index (bytes 4-7) and a
 * 64-bit counter (bytes 8-15, little-endian) that the sender bumps for every
 * message of a session. A jump in the counter means messages that never
 * reached our socket: lost on the way, or dropped by net_ps because the
 * socket's receive buffer was full. A counter at or below the highest seen
 * is a late (reordered or duplicated) one. A new receiver index (a rekey)
 * starts over without counting. Read before decryption, so it is a
 * diagnostic only: a forged message can move the counts, never the data
 * path (the core's replay window decides what is accepted).
 *
 * "lost" counts every gap at the moment it opens; a late message that fills
 * part of a gap later is counted in "late" too, so the real loss is about
 * lost - late. Jumps of RX_SEQ_JUMP or more are not counted as loss (a
 * forged or garbled counter), only noted in "jumps".
 *
 * A device test showed more "late" (2.7k) than "lost" (2.2k)
 * per 30 s, which lost - late cannot explain. A window of the last
 * RX_SEQ_WIN counters (one bit each) now tells the three apart:
 *   gone     a counter that left the window never seen: really lost
 *            (before our socket, or by a full receive buffer);
 *   filled   a late one that filled a gap: reordered on the way;
 *   dup      one seen already (a duplicate on the way);
 *   too_late older than the window (counted in "gone" already);
 * and how far behind the newest a reordered one came (depth: 1, 2-3,
 * 4-15, 16 or more; depth_max). "lost" and "late" keep their meaning.
 */
#ifndef RX_SEQ_H
#define RX_SEQ_H

#include <stdint.h>
#include <string.h>

#define RX_SEQ_JUMP 8192u
#define RX_SEQ_WIN  256u

struct rx_seq {
	uint32_t idx;          /* receiver index of the current run; 0 = none */
	uint64_t top;          /* highest counter seen in the run */
	uint32_t lost, late, jumps, restarts;
	uint32_t gone, filled, dup, too_late, depth_max;
	uint32_t depth[4];     /* filled ones late by 1, 2-3, 4-15, 16+ */
	uint32_t win[RX_SEQ_WIN / 32];   /* bit c % WIN: counter c seen (top-WIN < c <= top) */
};

static inline int
rx_seq_bit(const struct rx_seq *s, uint64_t c)
{
	uint32_t i = (uint32_t)(c % RX_SEQ_WIN);
	return (int)((s->win[i >> 5] >> (i & 31)) & 1u);
}

static inline void
rx_seq_put(struct rx_seq *s, uint64_t c, int on)
{
	uint32_t i = (uint32_t)(c % RX_SEQ_WIN);
	if (on)
		s->win[i >> 5] |= 1u << (i & 31);
	else
		s->win[i >> 5] &= ~(1u << (i & 31));
}

/* A new run: everything at or below the first counter counts as seen. */
static inline void
rx_seq_reset_win(struct rx_seq *s)
{
	memset(s->win, 0xff, sizeof(s->win));
}

static inline void
rx_seq_note(struct rx_seq *s, const uint8_t *m, uint32_t len)
{
	uint32_t idx;
	uint64_t ctr = 0, d, k, n;
	int i;

	if (len < 32 || m[0] != 4 || m[1] || m[2] || m[3])
		return;                 /* not a data message (header + tag) */
	idx = (uint32_t)m[4] | (uint32_t)m[5] << 8 | (uint32_t)m[6] << 16 | (uint32_t)m[7] << 24;
	for (i = 7; i >= 0; i--)
		ctr = ctr << 8 | m[8 + i];
	if (idx != s->idx || s->idx == 0) {
		s->idx = idx ? idx : 1;
		s->top = ctr;
		s->restarts++;
		rx_seq_reset_win(s);
		return;
	}
	if (ctr <= s->top) {
		d = s->top - ctr;
		s->late++;
		if (d >= RX_SEQ_WIN) {
			s->too_late++;
		} else if (rx_seq_bit(s, ctr)) {
			s->dup++;
		} else {
			rx_seq_put(s, ctr, 1);
			s->filled++;
			s->depth[d <= 1 ? 0 : d <= 3 ? 1 : d <= 15 ? 2 : 3]++;
			if (d > s->depth_max)
				s->depth_max = (uint32_t)d;
		}
		return;
	}
	d = ctr - s->top;
	if (d >= RX_SEQ_JUMP) {
		s->jumps++;
		s->top = ctr;
		rx_seq_reset_win(s);
		return;
	}
	s->lost += (uint32_t)(d - 1);
	/* The counters top+1 .. ctr take the places of top-WIN+1 .. ctr-WIN:
	 * any of those never seen is gone for good. */
	n = d < RX_SEQ_WIN ? d : RX_SEQ_WIN;
	for (k = 1; k <= n; k++) {
		if (!rx_seq_bit(s, s->top + k))
			s->gone++;
		rx_seq_put(s, s->top + k, 0);
	}
	if (d > RX_SEQ_WIN)
		s->gone += (uint32_t)(d - RX_SEQ_WIN);   /* fell through the window at once */
	rx_seq_put(s, ctr, 1);
	s->top = ctr;
}

#endif
