/*
 * tn_shim_vita.c - Vita kernel back end of the shim.
 *
 *   locks      two recursive kernel mutexes (SceThreadmgrForDriver)
 *   genuine    ksceSblRngGenuineRandomNumber, 64 bytes a call
 *   pseudo     ksceSblRngPseudoRandomNumber (SceSblSsMgrForDriver)
 *   uptime     ksceKernelGetSystemTimeWide, microseconds since boot
 *   wall time  ksceRtcGetCurrentTick, microseconds since 0001-01-01 UTC
 *
 * Link with SceThreadmgrForDriver_stub, SceSblSsMgrForDriver_stub,
 * SceRtcForDriver_stub and SceSysclibForDriver_stub (memcpy, memset, the
 * 64-bit division helpers). None of these may be called from an interrupt
 * handler, and neither may the core.
 */
#include "tn_shim.h"

#include <psp2kern/types.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/rtc.h>
#include <psp2kern/kernel/debug.h>

/*
 * SceSblSsMgrForDriver 0x61E9428D. The SDK stubs export both; its header
 * only declares the second.
 */
int ksceSblRngGenuineRandomNumber(void *dst);			/* 0xAC57F4F0 */
int ksceSblRngPseudoRandomNumber(void *dst, SceSize size);	/* 0x4F9BFBE5 */

#define GENUINE_CHUNK	0x40

/* SceRtcTick of 1970-01-01 00:00:00 UTC. */
#define RTC_UNIX_EPOCH	62135596800000000ULL

static SceUID g_mtx[TNSHIM_NLOCKS] = { -1, -1 };

int
tnshim_be_init(void)
{
	static const char *const names[TNSHIM_NLOCKS] = {
		"tncore_mtx", "tncore_rw",
	};
	int i;

	for (i = 0; i < TNSHIM_NLOCKS; i++) {
		g_mtx[i] = ksceKernelCreateMutex(names[i],
		    SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
		if (g_mtx[i] < 0) {
			int err = g_mtx[i];
			tnshim_be_fini();
			return err;
		}
	}
	return 0;
}

void
tnshim_be_fini(void)
{
	int i;

	for (i = 0; i < TNSHIM_NLOCKS; i++) {
		if (g_mtx[i] >= 0)
			ksceKernelDeleteMutex(g_mtx[i]);
		g_mtx[i] = -1;
	}
}

void
tnshim_be_lock(int id)
{
	ksceKernelLockMutex(g_mtx[id], 1, NULL);
}

void
tnshim_be_unlock(int id)
{
	ksceKernelUnlockMutex(g_mtx[id], 1);
}

uintptr_t
tnshim_be_self(void)
{
	return (uintptr_t)ksceKernelGetThreadId();
}

int
tnshim_be_random_genuine(void *buf, size_t n)
{
	unsigned char chunk[GENUINE_CHUNK] __attribute__((aligned(4)));
	unsigned char *p = buf;
	int ret = 0;

	while (n > 0) {
		size_t k = n > GENUINE_CHUNK ? GENUINE_CHUNK : n;
		if (ksceSblRngGenuineRandomNumber(chunk) < 0) {
			ret = -1;
			break;
		}
		memcpy(p, chunk, k);
		p += k;
		n -= k;
	}
	tnshim_explicit_bzero(chunk, sizeof(chunk));
	return ret;
}

int
tnshim_be_random_pseudo(void *buf, size_t n)
{
	return ksceSblRngPseudoRandomNumber(buf, n) < 0 ? -1 : 0;
}

void
tnshim_be_uptime_ns(uint64_t *ns)
{
	*ns = (uint64_t)ksceKernelGetSystemTimeWide() * 1000ULL;
}

/*
 * Handshake TAI64N timestamps must never go backwards, or every server
 * the console has used would reject it after a clock jump (flat battery,
 * manual date change). The module seeds g_wall_floor from the value it
 * persisted to ur0:data/vpnturbo/state at the previous run, and
 * tnshim_be_walltime_ns returns max(realtime, floor + 1ns), bumping the
 * floor each time. The module reads g_wall_floor back out on a timer and on
 * suspend and writes it to the state file; no file I/O happens here, under
 * the handshake lock. One floor is enough: v1 has a single private key.
 */
static uint64_t g_wall_floor;	/* ns; written only through tnshim_vita_wall_floor_* */

void
tnshim_vita_wall_floor_set(uint64_t ns)
{
	__atomic_store_n(&g_wall_floor, ns, __ATOMIC_RELAXED);
}

uint64_t
tnshim_vita_wall_floor_get(void)
{
	return __atomic_load_n(&g_wall_floor, __ATOMIC_RELAXED);
}

void
tnshim_be_walltime_ns(uint64_t *ns)
{
	SceRtcTick tick;
	uint64_t now, floor;

	/*
	 * Handshake timestamps only need to grow from one handshake to the
	 * next; if the clock cannot be read, uptime is the least bad stand-in.
	 */
	if (ksceRtcGetCurrentTick(&tick) < 0 || tick.tick < RTC_UNIX_EPOCH)
		tnshim_be_uptime_ns(&now);
	else
		now = (tick.tick - RTC_UNIX_EPOCH) * 1000ULL;

	/* max(now, floor + 1), then raise the floor. */
	do {
		floor = __atomic_load_n(&g_wall_floor, __ATOMIC_RELAXED);
		if (now <= floor)
			now = floor + 1;
	} while (!__atomic_compare_exchange_n(&g_wall_floor, &floor, now,
	    0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));

	*ns = now;
}

void
tnshim_be_sleep_ns(uint64_t ns)
{
	uint64_t us = ns / 1000ULL;

	ksceKernelDelayThread(us > 0xffffffffULL ? 0xffffffffU : (SceUInt)us);
}

void
tnshim_be_panic(const char *what, const char *file, int line)
{
	ksceDebugPrintf("tunnel core: %s (%s:%d)\n", what, file, line);
	/* Park this thread rather than take the whole console down. */
	for (;;)
		ksceKernelDelayThread(1000000);
}
