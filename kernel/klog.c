/*
 * klog.c - the kernel log ring, flushed to ur0:data/vpnturbo/kernel.log
 * by its own thread. Lines are
 * appended from any context (callbacks included); the thread opens, appends
 * and closes the file each pass so whatever was written survives a hang. It
 * records state changes, handshakes, errors and route changes only - never
 * key material and never packet contents. proto.c logs through tnp_log, which
 * forwards here.
 */
#include "tn_kern.h"

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "vt_paths.h"

#define KLOG_DIR   VT_DATA_DIR
#define KLOG_PATH  KLOG_DIR "/kernel.log"
#define KLOG_LINES 128
#define KLOG_LEN   112

/*
 * The ring. A writer reserves a sequence number with an
 * atomic add, then claims its slot (seq % KLOG_LINES) with a busy flag, so
 * two writers a whole lap apart can never format into the same line at
 * once (the later one drops its line and counts it). When the line is
 * complete the slot's sequence is published (release). The flusher writes
 * line `t` only if the slot is not busy and holds sequence t + 1, both
 * before and after copying it out (acquire): a line still being written
 * waits for the next pass (bounded: after a lap it is skipped), a line
 * already overwritten by a newer one is skipped. Lock-free on both sides,
 * so any context may log, net_ps callbacks included.
 */
static char g_klog[KLOG_LINES][KLOG_LEN];
static volatile uint32_t g_seq[KLOG_LINES];   /* sequence + 1 of the line in the slot, 0 = none */
static volatile uint8_t g_busy[KLOG_LINES];
static volatile uint32_t g_head, g_tail, g_dropped;
static SceUID g_thread = -1;

static void
vlog(const char *tag, const char *fmt, va_list ap)
{
	uint32_t i = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
	uint32_t slot = i % KLOG_LINES;
	char *line = g_klog[slot];
	uint32_t ms = (uint32_t)(ksceKernelGetSystemTimeWide() / 1000);
	int n;

	if (__atomic_exchange_n(&g_busy[slot], 1, __ATOMIC_ACQUIRE)) {
		__atomic_add_fetch(&g_dropped, 1, __ATOMIC_RELAXED);
		return;                         /* a writer a lap behind is still in it */
	}
	__atomic_store_n(&g_seq[slot], 0, __ATOMIC_RELAXED);
	n = snprintf(line, KLOG_LEN, "%8u %s", (unsigned)ms, tag);
	if (n < 0 || n >= KLOG_LEN)
		n = 0;
	vsnprintf(line + n, (size_t)(KLOG_LEN - n - 1), fmt, ap);
	line[KLOG_LEN - 1] = 0;
	__atomic_store_n(&g_seq[slot], i + 1, __ATOMIC_RELEASE);
	__atomic_store_n(&g_busy[slot], 0, __ATOMIC_RELEASE);
}

void
klog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog("", fmt, ap);
	va_end(ap);
}

/* proto.c's logging hook. */
void
tnp_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog("[tunnel] ", fmt, ap);
	va_end(ap);
}

/* Copy line t out if it is complete and still t: 1 = copied, 0 = not yet
 * written, -1 = gone (overwritten, or dropped). */
static int
take_line(uint32_t t, char *out)
{
	uint32_t slot = t % KLOG_LINES, s1, s2;

	if (__atomic_load_n(&g_busy[slot], __ATOMIC_ACQUIRE))
		return 0;
	s1 = __atomic_load_n(&g_seq[slot], __ATOMIC_ACQUIRE);
	if (s1 != t + 1)
		return (int32_t)(s1 - (t + 1)) > 0 ? -1 : 0;
	memcpy(out, g_klog[slot], KLOG_LEN);
	out[KLOG_LEN - 1] = 0;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	s2 = __atomic_load_n(&g_seq[slot], __ATOMIC_RELAXED);
	if (s2 != s1 || __atomic_load_n(&g_busy[slot], __ATOMIC_RELAXED))
		return -1;
	return 1;
}

static int
klog_thread(SceSize args, void *argp)
{
	static char line[KLOG_LEN];
	static uint32_t told_dropped, stuck_t = 0xFFFFFFFFu, stuck_n;

	(void)args; (void)argp;
	for (;;) {
		ksceKernelDelayThread(50 * 1000);   /* short: the last lines before a crash matter */
		/* No file I/O from the first suspend event to the wake (the
		 * storage stack suspends with the rest). A user pause, and the
		 * post-wake network wait, are not a reason to stop logging. */
		if (g_st.suspended)
			continue;
		uint32_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
		if (g_tail == head)
			continue;
		SceUID fd = ksceIoOpen(KLOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
		if (fd < 0)
			continue;
		if (head - g_tail > KLOG_LINES)
			g_tail = head - KLOG_LINES;
		while (g_tail != head) {
			int r = take_line(g_tail, line);
			if (r == 0 && head - g_tail < KLOG_LINES) {
				/* Still being written: next pass. A line whose
				 * writer dropped it (slot busy) never comes:
				 * skipped after three passes (150 ms). */
				if (stuck_t != g_tail) {
					stuck_t = g_tail;
					stuck_n = 0;
				}
				if (++stuck_n < 3)
					break;
			}
			if (r == 1) {
				ksceIoWrite(fd, line, strnlen(line, KLOG_LEN));
				ksceIoWrite(fd, "\n", 1);
			}
			g_tail++;
		}
		if (g_dropped != told_dropped) {
			char b[48];
			int n;
			told_dropped = g_dropped;
			n = snprintf(b, sizeof(b), "klog: %u line(s) dropped (ring busy)\n", (unsigned)told_dropped);
			if (n > 0 && n < (int)sizeof(b))
				ksceIoWrite(fd, b, (SceSize)n);
		}
		ksceIoClose(fd);
	}
	return 0;
}

void
klog_init(void)
{
	ksceIoMkdir(KLOG_DIR, 0777);
	g_thread = ksceKernelCreateThread("vpnturbo_log", klog_thread, 0x60, 0x2000, 0, 0, NULL);
	if (g_thread >= 0)
		ksceKernelStartThread(g_thread, 0, NULL);
}
