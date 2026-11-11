/*
 * strand.h - reset the connections stranded when the tunnel goes away
 * (strand_core.h says how; strand.c is the platform side).
 *
 * routes.c arms it when the tunnel routes are removed (pause, disconnect, any
 * transition away from UP), with the address the tunnel had; connecting,
 * resuming, the tunnel routes coming back or a change of the Wi-Fi's address
 * disarms it, and ten minutes end it at the latest. Armed, the stack's
 * transmit dequeue routine is watched for TCP segments leaving over the Wi-Fi
 * with that source address, and the interface thread answers each connection
 * with a reset up the console's own stack, so the application reconnects
 * directly at once.
 *
 * The hook is installed the first time it is armed in a boot, only on a build
 * of the stack whose code matches the table (strand_fw.h), and is never
 * released: while not armed it is a pass-through (one load).
 */
#ifndef VT_STRAND_H
#define VT_STRAND_H

#include <stdint.h>

#include "strand_core.h"

/* Any thread; no net_ps call. `addr` is the tunnel address, host order;
 * `wlan` the Wi-Fi's current address (0 = unknown): never armed for it. */
void strand_arm(uint32_t addr, uint32_t wlan);
void strand_disarm(const char *why);

/* The interface thread (the only reader), in this order each pass. */
int  strand_pending(void);   /* armed, or entries waiting: poll at 250 ms */
void strand_poll(int quiet); /* install on first arm, close the window, flush while quiet */
uint32_t strand_next(uint8_t ip[STRAND_IP_MAX]);   /* the next connection to answer, or 0 */

/* 1 once the hook is in: the module may not stop. */
int strand_installed(void);

void strand_counts(uint32_t *caught, uint32_t *dropped, uint32_t *unreadable, uint32_t *lost);

#endif /* VT_STRAND_H */
