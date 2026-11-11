/*
 * strand_hook.h - the hook of the stranded-connection reset (strand.c): the
 * whole body of the network stack's transmit dequeue routine (a driver calls
 * it to take the next frame to send; it returns the frame's packet or NULL).
 * Its object file is checked by tests/check_strand.sh together with
 * strand_core.o: no floating-point or vector instruction, and no call but the
 * core's own helpers.
 *
 * Rules the hook follows:
 *   - the original runs through a reference that is looked at before it is
 *     used: taiHEN makes a hook live a moment before it stores the reference,
 *     so a call in that gap spins on the reference with plain loads (a bounded
 *     count; it may not call the system to wait) and, if it is still missing,
 *     answers "no frame" (the driver asks again at its next kick);
 *   - the frame is looked at after the original returns: the driver owns it
 *     until it frees it, which is after the hook has returned. It is never
 *     changed, held or dropped;
 *   - not armed it does one load and nothing else;
 *   - once installed it is never taken out (a live hook released under
 *     traffic is dangerous), so the module refuses to stop for it.
 */
#ifndef VT_STRAND_HOOK_H
#define VT_STRAND_HOOK_H

#include <stdint.h>

#include <taihen.h>

extern volatile tai_hook_ref_t strand_ref;

uint32_t strand_hook(uint32_t desc);

#endif /* VT_STRAND_HOOK_H */
