/*
 * strand.c - the platform side of the stranded-connection reset (strand.h).
 *
 * What it does with the system: reads the network stack's code once, to match
 * it against the table, and puts one taiHEN offset hook on its transmit
 * dequeue routine the first time the reset is armed. The hook is never
 * released (a live hook released while its routine ran is dangerous), so it
 * stays a pass-through for the module's life and the module refuses to stop
 * (module.c). Arming and disarming only set flags; the interface thread does
 * the installing and the answering (iface.c), in thread context.
 */
#include "tn_kern.h"
#include "strand.h"
#include "strand_fw.h"
#include "strand_hook.h"

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include <string.h>

/* taihenModuleUtils (no SDK header). */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid, uint32_t funcnid, uintptr_t *func);


static volatile uint32_t g_until;   /* uptime (us, low 32 bits) the window ends at the latest */
static volatile int g_want;         /* armed and the hook not yet tried */
static volatile int g_inst;         /* 0 not installed, 1 installed, -1 off for this boot */
static int g_tried;                 /* install() has run (interface thread only) */
static SceUID g_hook = -1;

static uint32_t g_text, g_text_size;     /* the stack's code segment; interface thread only */
static SceKernelModuleInfo g_mi;         /* 440 bytes: the thread's stack is small */

#define IP4(a) (unsigned)((a) >> 24), (unsigned)(((a) >> 16) & 0xFF), \
	(unsigned)(((a) >> 8) & 0xFF), (unsigned)((a) & 0xFF)

static uint32_t
now_us(void)
{
	return (uint32_t)ksceKernelGetSystemTimeWide();
}

/* ---- arming (any thread) -------------------------------------------------- */

void
strand_arm(uint32_t addr, uint32_t wlan)
{
	if (!addr || g_inst < 0)
		return;                      /* off for this boot (already logged) */
	if (!strand_may_arm(addr, wlan)) {
		klog("strand: not armed: %u.%u.%u.%u is the Wi-Fi's own address", IP4(addr));
		return;
	}
	__atomic_store_n(&g_until, now_us() + STRAND_WINDOW_US, __ATOMIC_RELEASE);
	strand_core_arm(addr, wlan);
	g_want = 1;
	klog("strand: armed until the tunnel is back (10 min at most), tunnel address %u.%u.%u.%u", IP4(addr));
	iface_strand_wake();
}

void
strand_disarm(const char *why)
{
	if (!__atomic_load_n(&strand_st.addr, __ATOMIC_ACQUIRE))
		return;
	strand_core_disarm();
	klog("strand: disarmed (%s): caught %u dropped %u", why, (unsigned)strand_st.caught,
	    (unsigned)strand_st.dropped);
}

/* True from the start of the install until the boot ends (a failure turns it off). */
int strand_installed(void) { return g_tried && g_inst >= 0; }

void
strand_counts(uint32_t *caught, uint32_t *dropped, uint32_t *unreadable, uint32_t *lost)
{
	*caught = strand_st.caught;
	*dropped = strand_st.dropped;
	*unreadable = strand_st.unreadable;
	*lost = strand_st.lost;
}

/* ---- installing (interface thread) ---------------------------------------- */

typedef int (*modinfo_fn)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);

/* ksceKernelGetModuleInfo is in a library of the module manager that was
 * renumbered at 3.63: ask for it under both. */
static modinfo_fn
resolve_modinfo(void)
{
	static const struct { uint32_t lib, nid; } k[] = {
		{ 0xC445FA63u, 0xD269F915u },    /* 3.60, 3.61 */
		{ 0x92C9FFC2u, 0xB72C75A4u },    /* 3.63 and later */
	};
	for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		uintptr_t fn = 0;
		if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", k[i].lib, k[i].nid, &fn) >= 0 && fn)
			return (modinfo_fn)fn;
	}
	return NULL;
}

/* The stack's id and where its code segment is. */
static int
find_text(SceUID *modid)
{
	tai_module_info_t ti;
	modinfo_fn info;

	memset(&ti, 0, sizeof(ti));
	ti.size = sizeof(ti);
	if (taiGetModuleInfoForKernel(KERNEL_PID, STRAND_MODULE, &ti) < 0)
		return -1;
	info = resolve_modinfo();
	if (!info)
		return -2;
	memset(&g_mi, 0, sizeof(g_mi));
	g_mi.size = sizeof(g_mi);
	if (info(KERNEL_PID, ti.modid, &g_mi) < 0)
		return -3;
	if (strncmp(g_mi.module_name, STRAND_MODULE, sizeof(g_mi.module_name)) != 0 ||
	    !g_mi.segments[STRAND_SEG_CODE].vaddr || !g_mi.segments[STRAND_SEG_CODE].memsz)
		return -4;
	g_text = (uint32_t)(uintptr_t)g_mi.segments[STRAND_SEG_CODE].vaddr;
	g_text_size = g_mi.segments[STRAND_SEG_CODE].memsz;
	*modid = ti.modid;
	return 0;
}

static int
read_text(uint32_t off, uint8_t *out, uint32_t n)
{
	if (!g_text || (uint64_t)off + n > g_text_size)
		return -1;
	memcpy(out, (const void *)(uintptr_t)(g_text + off), n);
	return 0;
}

/* Once per boot, whatever the outcome. */
static void
install(void)
{
	const strand_fw_row *row;
	SceUID modid = -1, uid;
	int r;

	r = find_text(&modid);
	if (r < 0) {
		g_inst = -1;
		klog("strand: off: the network stack's code could not be read (%d); nothing hooked", r);
		return;
	}
	row = strand_fw_match(read_text);
	if (!row) {
		g_inst = -1;
		klog("strand: off: the network stack's code matches no known build; nothing hooked");
		return;
	}
	strand_ref = 0;                      /* before the hook can be live */
	uid = taiHookFunctionOffsetForKernel(KERNEL_PID, (tai_hook_ref_t *)&strand_ref, modid,
	    STRAND_SEG_CODE, row->tx_off, 1, strand_hook);
	if (uid < 0) {
		g_inst = -1;
		strand_ref = 0;
		klog("strand: off: the hook did not install (0x%08X, build %s)", (unsigned)uid, row->name);
		return;
	}
	g_hook = uid;
	g_inst = 1;
	klog("strand: hook installed (build %s, routine at 0x%X)", row->name, (unsigned)row->tx_off);
}

/* ---- the interface thread's side ------------------------------------------- */

int
strand_pending(void)
{
	return __atomic_load_n(&strand_st.addr, __ATOMIC_ACQUIRE) != 0 || strand_waiting();
}

void
strand_poll(int quiet)
{
	uint8_t drop[STRAND_IP_MAX];

	if (__atomic_load_n(&strand_st.addr, __ATOMIC_ACQUIRE) &&
	    !strand_window_open(__atomic_load_n(&g_until, __ATOMIC_ACQUIRE), now_us())) {
		strand_core_expire();            /* the ring is still answered */
		klog("strand: window closed: caught %u dropped %u unreadable %u", (unsigned)strand_st.caught,
		    (unsigned)strand_st.dropped, (unsigned)strand_st.unreadable);
	}
	if (quiet) {
		/* net_ps may not be touched (a suspend, a wait for the network):
		 * what was caught is old by the time it ends. */
		while (strand_pop(drop))
			;
		return;
	}
	if (g_want && !g_tried) {
		g_want = 0;
		g_tried = 1;
		install();
	}
}

uint32_t
strand_next(uint8_t ip[STRAND_IP_MAX])
{
	uint32_t n, ms = now_us() / 1000u;

	while ((n = strand_pop(ip)) != 0)
		if (!strand_recent(ip, ms))
			return n;
	return 0;
}
