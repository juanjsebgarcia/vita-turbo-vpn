/*
 * overlay.c - the framebuffer overlay: the status-bar padlock
 * and the HUD, on one hook.
 *
 * Technique (Wi-Fi Turbo's HUD, proven on the console): hook
 * ksceDisplaySetFrameBufInternal
 * (SceDisplayForDriver 0x9FED47AC, NID 0x16466675; exported on every
 * firmware 3.60-3.74), let the original run, then draw into the buffer the
 * process just handed over, with the DACR opened around the writes because
 * the buffer is a user-space CDRAM mapping. taiHEN chains hooks on one
 * export, so this sits next to Wi-Fi Turbo's own without either knowing.
 *
 * What is drawn:
 *   - padlock: kernel/icons/vpn_icons.h `padlock_bold`, alpha-blended at 1x,
 *     white while the tunnel is UP, Signal red (#dc1c2c) while traffic is
 *     held (LOCKED, or Lockdown mode with the tunnel not up, not while
 *     paused), nothing in any other state; centred in the status bar. Only into frames from
 *     SceShell (home screen, LiveArea, quick menu) and Settings, never a
 *     game's, and only for a 960x544 buffer, where the status-bar layout is
 *     known (the padlock is centred; its position is
 *     adjustable in the settings file for the device check);
 *   - HUD (hud.c): Compact is the "VPN <STATE>" line; Extended adds TUNN:
 *     (not while idle) and EXIT:, in the chosen corner, on the same
 *     screens, and over games and apps too when "in games" is on.
 *
 * The hook only loads, stores and blits: the HUD's lines are built outside
 * it (overlay_hud_sync, from the timer tick and from the calls that change
 * what they show), into the back copy, and published by flipping the front
 * index. Cost per frame when nothing is to be drawn: a few loads and
 * compares; the padlock is 16x22 pixels, of which only the anti-aliased
 * edge is read back.
 *
 * Guards (any failing one means: draw nothing this frame):
 *   - nothing before the first vtSetOverlay call (the SceShell plugin makes
 *     it about 10 s after boot), so a cold boot never meets an unproven
 *     blit, as Wi-Fi Turbo's draw-arm gate; "draw=0" in the settings file
 *     turns every pixel write off;
 *   - the original call succeeded (a refused frame is not drawn into);
 *   - head 0 only; a base, pitch and size that make sense (Wi-Fi Turbo's
 *     check), a 4-byte aligned base, and pixel format A8B8G8R8 exactly:
 *     any other format (10-bit, or bits this code does not know) is skipped.
 *
 * Settings (ur0:data/vpnturbo/overlay.txt, key=value lines, read at
 * module start and rewritten when a client changes them):
 *   padlock=1 hud=0 corner=2 games=0 lang=1   (the defaults; hud is
 *   0 Off, 1 Compact, 2 Extended)
 *   padlock_x=41 padlock_y=5 draw=1           (calibration, file only)
 */
#include "tn_kern.h"
#include "overlay.h"
#include "hud.h"
#include "hud_i18n.h"
#include "icons/vpn_icons.h"

#include <psp2kern/display.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>
#include "vt_paths.h"

/* SceSysrootForKernel: the caller's title id (as control.c). */
int ksceKernelSysrootGetProcessTitleId(SceUID pid, char *titleid, SceSize len);

#define DISPLAY_DRIVER_LIB    0x9FED47AC   /* SceDisplayForDriver */
#define NID_SET_FRAMEBUF_INT  0x16466675   /* ksceDisplaySetFrameBufInternal */

#define OV_DIR   VT_DATA_DIR
#define OV_PATH  OV_DIR "/overlay.txt"

/* Centred in the status bar (960 wide), which is rarely occupied there;
 * left of centre other status icons appeared behind it. */
#define PADLOCK_X   ((960 - VT_ICON_PADLOCK_BOLD_W) / 2)
#define PADLOCK_Y   5
#define COL_WHITE   0x00FFFFFFu
#define COL_RED     0x002C1CDCu  /* #dc1c2c as A8B8G8R8 (R in the low byte) */

/* ---- settings ------------------------------------------------------------ */

static volatile int g_padlock = 1;
static volatile int g_hud = 0;
static volatile int g_corner = VT_CORNER_BOTTOM_LEFT;
static volatile int g_games = 0;
static volatile int g_lang = 1;               /* English (US) */
static volatile int g_px = PADLOCK_X, g_py = PADLOCK_Y;
static volatile int g_draw = 1;

static volatile int g_armed;                  /* a client has called vtSetOverlay */
static volatile SceUID g_pid_shell = -1, g_pid_settings = -1;
static volatile uint32_t g_exit_ip;           /* from vtSetExitIp; cleared while the tunnel is idle */

static SceUID g_hook = -1;
/* taiHEN makes the hook live a moment before it stores the ref, and calls
 * keep coming from other threads meanwhile: wait for the ref (bounded; a
 * single load once it is set) and never continue through 0. */
static volatile tai_hook_ref_t g_ref;
#define HOOK_NO_REF  ((int)0x80010005)

static tai_hook_ref_t ref_wait(void)
{
	for (int i = 0; i < 2000 && g_ref == 0; i++)
		ksceKernelDelayThread(10);   /* at most ~20 ms, only in the gap */
	return g_ref;
}
static SceUID g_lock = -1;                    /* settings writers and the file */
static int g_loaded;                          /* the file was read (or is absent) */

static volatile SceUID g_lock_holder = -1;
static void lock(void)   { tn_mutex_lock(g_lock, "overlay settings", &g_lock_holder); }
static void unlock(void) { if (g_lock >= 0) ksceKernelUnlockMutex(g_lock, 1); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void parse_line(const char *k, int klen, int v)
{
#define KEY(name) (klen == (int)sizeof(name) - 1 && memcmp(k, name, sizeof(name) - 1) == 0)
	if (KEY("padlock"))        g_padlock = v != 0;
	else if (KEY("hud"))       g_hud = clampi(v, VT_HUD_OFF, VT_HUD_EXTENDED);
	else if (KEY("corner"))    g_corner = clampi(v, VT_CORNER_TOP_LEFT, VT_CORNER_BOTTOM_RIGHT);
	else if (KEY("games"))     g_games = v != 0;
	else if (KEY("lang"))      g_lang = clampi(v, 0, 19);
	else if (KEY("padlock_x")) g_px = clampi(v, 0, 960 - VT_ICON_PADLOCK_BOLD_W);
	else if (KEY("padlock_y")) g_py = clampi(v, 0, 544 - VT_ICON_H);
	else if (KEY("draw"))      g_draw = v != 0;
#undef KEY
}

/* Under g_lock. */
static void settings_load(void)
{
	char buf[256];
	SceUID fd = ksceIoOpen(OV_PATH, SCE_O_RDONLY, 0);
	int n, i = 0;

	if (fd < 0) {
		/* Absent: the defaults stand. A missing mount at early boot is
		 * retried at the first vtSetOverlay call. */
		g_loaded = (fd == (SceUID)0x80010002);   /* SCE_ERROR_ERRNO_ENOENT */
		return;
	}
	n = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	g_loaded = 1;
	if (n <= 0)
		return;
	buf[n] = 0;
	while (i < n) {
		int ks = i, ke, v = 0, neg = 0;
		while (i < n && buf[i] != '=' && buf[i] != '\n') i++;
		ke = i;
		if (i < n && buf[i] == '=') {
			i++;
			if (i < n && buf[i] == '-') { neg = 1; i++; }
			while (i < n && buf[i] >= '0' && buf[i] <= '9' && v < 100000)
				v = v * 10 + (buf[i++] - '0');
			parse_line(buf + ks, ke - ks, neg ? -v : v);
		}
		while (i < n && buf[i] != '\n') i++;
		i++;
	}
	klog("overlay: settings padlock=%d hud=%d corner=%d games=%d lang=%d",
	    g_padlock, g_hud, g_corner, g_games, g_lang);
}

/* Under g_lock. */
static void settings_save(void)
{
	char buf[160];
	int n = snprintf(buf, sizeof(buf),
	    "padlock=%d\nhud=%d\ncorner=%d\ngames=%d\nlang=%d\n",
	    g_padlock, g_hud, g_corner, g_games, g_lang);
	SceUID fd;

	if (n <= 0 || n >= (int)sizeof(buf))
		return;
	/* Calibration keys are kept only when they differ from the defaults. */
	if (g_px != PADLOCK_X || g_py != PADLOCK_Y)
		n += snprintf(buf + n, sizeof(buf) - n, "padlock_x=%d\npadlock_y=%d\n", g_px, g_py);
	if (!g_draw && n < (int)sizeof(buf) - 8)
		n += snprintf(buf + n, sizeof(buf) - n, "draw=0\n");
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;
	ksceIoMkdir(OV_DIR, 0777);
	fd = ksceIoOpen(OV_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) {
		klog("overlay: settings not saved (0x%08X)", (unsigned)fd);
		return;
	}
	ksceIoWrite(fd, buf, n);
	ksceIoClose(fd);
}

/* ---- drawing ------------------------------------------------------------- */

/*
 * The framebuffer is a user-space mapping
 * (CDRAM or main memory), which the kernel's own domain setting does not
 * let it touch. All domains are opened as manager (0xFFFFFFFF), as in Wi-Fi
 * Turbo's HUD, for the blit only; the previous value is put back after.
 */
static inline uint32_t dacr_open(void)
{
	uint32_t prev;
	__asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(prev));
	__asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(0xFFFFFFFFu));
	__asm__ volatile("isb");
	return prev;
}
static inline void dacr_restore(uint32_t prev)
{
	__asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(prev));
	__asm__ volatile("isb");
}

/* x * a / 255 rounded, without a division (no hardware divide). */
static inline uint32_t mul255(uint32_t x)
{
	x += 128;
	return (x + (x >> 8)) >> 8;
}

/* Blend `col` (0x00BBGGRR) through the padlock's alpha mask at (ox, oy). */
static void padlock_draw(uint32_t *fb, int pitch, int fw, int fh,
                         int ox, int oy, uint32_t col)
{
	const uint8_t *alpha = vt_icon_padlock_bold_alpha;
	const int w = VT_ICON_PADLOCK_BOLD_W;

	for (int y = 0; y < VT_ICON_H; y++) {
		int py = oy + y;
		if (py < 0 || py >= fh)
			continue;
		for (int x = 0; x < w; x++) {
			int px = ox + x;
			uint32_t a = alpha[y * w + x];
			if (!a || px < 0 || px >= fw)
				continue;
			uint32_t *p = &fb[py * pitch + px];
			if (a == 255) {
				*p = 0xFF000000u | col;
				continue;
			}
			uint32_t d = *p, out = 0xFF000000u;
			for (int s = 0; s < 24; s += 8) {
				uint32_t c = (col >> s) & 0xFF, b = (d >> s) & 0xFF;
				out |= mul255(c * a + b * (255u - a)) << s;
			}
			*p = out;
		}
	}
}

/* ksceDisplaySetFrameBufInternal(head, index, pParam, sync): the kernel
 * funnel every set-framebuffer call passes through, in the context of the
 * process setting the frame. */
static int hook_set_framebuf(int head, int index,
                             const SceDisplayFrameBuf *pParam, int sync)
{
	tai_hook_ref_t ref = ref_wait();
	if (!ref)
		return HOOK_NO_REF;   /* the frame is refused (SCE_ERROR_ERRNO_EIO), no work of ours */
	int ret = TAI_CONTINUE(int, ref, head, index, pParam, sync);

	g_wakes.fb++;           /* "idle:" line: hook calls per second */
	/* The display refused the frame: it is not shown, so nothing is
	 * drawn into it (and its base was not accepted by anyone). */
	if (ret < 0 || !g_armed || !g_draw || head != 0 || !pParam)
		return ret;

	int state = g_st.state;
	/* Red whenever traffic is being held: the tunnel's own lockdown, or
	 * Lockdown mode while the tunnel is not up (not while paused: a pause
	 * lets traffic go direct). So a console with Lockdown on and the VPN
	 * off says why nothing reaches the network. */
	int ld = g_st.lockdown && !g_st.paused && state != VT_STATE_UP;
	int red = state == VT_STATE_LOCKED || ld;
	int lockdraw = g_padlock && (state == VT_STATE_UP || red);
	int huddraw = g_hud != VT_HUD_OFF;
	int banner = options_restart_pending();
	if (!lockdraw && !huddraw && !banner)
		return ret;                      /* the usual case: nothing to do */

	/* Plausibility guard: only a sane linear 32 bpp A8B8G8R8 buffer. */
	int w = (int)pParam->width, h = (int)pParam->height,
	    pitch = (int)pParam->pitch;
	uintptr_t base = (uintptr_t)pParam->base;
	if (!base || (base & 3) || pParam->pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 ||
	    w <= 0 || h <= 0 || pitch < w || w > 1920 || h > 1088 || pitch > 4096)
		return ret;

	/* Called in the context of the process setting the frame. */
	SceUID pid = ksceKernelGetProcessId();
	int sys = pid == g_pid_shell || pid == g_pid_settings;
	if (!sys) {
		lockdraw = 0;                    /* never over a game */
		if (!g_games)
			huddraw = 0;
	}
	if (lockdraw && (w != 960 || h != 544))
		lockdraw = 0;                    /* status-bar layout known at 960x544 only */
	if (!sys)
		banner = 0;                      /* Settings or the home screen: where it was changed */
	if (!lockdraw && !huddraw && !banner)
		return ret;

	uint32_t prev = dacr_open();
	if (lockdraw)
		padlock_draw((uint32_t *)base, pitch, w, h, g_px, g_py,
		    red ? COL_RED : COL_WHITE);
	if (huddraw)
		hud_render((uint32_t *)base, pitch, w, h, g_corner);
	if (banner)
		hud_banner((uint32_t *)base, pitch, w, h, hud_tr(g_lang, HTR_RESTARTING));
	dacr_restore(prev);
	return ret;
}

/* ---- the HUD's text ------------------------------------------------------ */

/* Rebuild the HUD lines if what they show has changed, into the back copy
 * (hud_update publishes it by flipping the front index). Never called from
 * the hook: the timer tick, the control calls that change the tunnel's
 * state and the calls that change the settings or the exit address do. A
 * call that finds a build running skips; the next tick catches up.
 *
 * The exit address belongs to one connection: it is dropped here as soon
 * as the tunnel is seen idle, whatever the HUD mode, so a new connection
 * never shows (or later shows) the last one's. */
void
overlay_hud_sync(void)
{
	HudInfo in;
	int state = g_st.state;

	if (state == VT_STATE_IDLE)
		g_exit_ip = 0;
	in.state = state;
	in.lang = g_lang;
	in.extended = g_hud == VT_HUD_EXTENDED;
	in.tunnel_address = in.extended && state != VT_STATE_IDLE ? g_st.tunnel_address : 0;
	in.exit_ip = in.extended ? g_exit_ip : 0;
	hud_update(&in);
}

/* A new connection starts without the last one's exit address, whatever
 * state the tunnel was in (Lockdown leaves it LOCKED after a disconnect, so
 * it is never seen idle). */
void
overlay_exit_ip_reset(void)
{
	g_exit_ip = 0;
}

/* ---- module -------------------------------------------------------------- */

void
overlay_init(void)
{
	g_lock = ksceKernelCreateMutex("vpnturbo_ovl", 0, 0, NULL);
	lock();
	settings_load();
	unlock();

	overlay_hud_sync();                  /* the first lines, before any frame can want them */

	g_ref = 0;
	g_hook = taiHookFunctionExportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_ref, "SceDisplay",
	    DISPLAY_DRIVER_LIB, NID_SET_FRAMEBUF_INT, hook_set_framebuf);
	if (g_hook < 0)
		klog("overlay: framebuffer hook failed (0x%08X): no padlock or HUD", (unsigned)g_hook);
	else
		klog("overlay: framebuffer hook in place (drawing waits for the plugin)");
}

void
overlay_fini(void)
{
	if (g_hook >= 0) {
		taiHookReleaseForKernel(g_hook, g_ref);
		g_hook = -1;
		g_ref = 0;
	}
}

/* ---- syscalls ------------------------------------------------------------ */

enum { ROLE_NONE = -1, ROLE_APP, ROLE_SHELL, ROLE_SETTINGS };

static int caller_role(SceUID pid)
{
	char tid[32];

	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid)) < 0)
		return ROLE_NONE;
	if (strncmp(tid, "VPNT00001", sizeof(tid)) == 0) return ROLE_APP;
	if (strncmp(tid, "main", sizeof(tid)) == 0)      return ROLE_SHELL;
	if (strncmp(tid, "NPXS10015", sizeof(tid)) == 0) return ROLE_SETTINGS;
	return ROLE_NONE;
}

int
vtSetOverlay(const VtOverlay *uo)
{
	SceUID pid = ksceKernelGetProcessId();
	int role = caller_role(pid);
	VtOverlay k;
	int changed = 0, loaded = 0;

	if (role == ROLE_NONE)
		return -VT_ERR_DENIED;
	if (role == ROLE_SHELL && g_pid_shell != pid) {
		g_pid_shell = pid;
		klog("overlay: SceShell is pid 0x%08X", (unsigned)pid);
	} else if (role == ROLE_SETTINGS && g_pid_settings != pid) {
		g_pid_settings = pid;
		klog("overlay: Settings is pid 0x%08X", (unsigned)pid);
	}

	lock();
	if (!g_loaded) {
		settings_load();                 /* ur0 was not there at module start */
		loaded = 1;
	}
	if (!g_armed) {
		g_armed = 1;
		klog("overlay: drawing armed");
	}
	if (!uo) {
		unlock();
		if (loaded)
			overlay_hud_sync();
		return 0;
	}
	if (ksceKernelMemcpyUserToKernel(&k, uo, sizeof(k)) < 0 || k.size != sizeof(VtOverlay)) {
		unlock();
		if (loaded)
			overlay_hud_sync();
		return -VT_ERR_BAD_PROFILE;
	}
#define SET(var, val) do { int v_ = (val); if (var != v_) { var = v_; changed = 1; } } while (0)
	SET(g_padlock, k.padlock_on != 0);
	SET(g_hud, clampi(k.hud_on, VT_HUD_OFF, VT_HUD_EXTENDED));
	if (k.corner <= VT_CORNER_BOTTOM_RIGHT)
		SET(g_corner, k.corner);
	SET(g_games, k.in_games != 0);
	if (k.lang != VT_LANG_KEEP && k.lang < 20)
		SET(g_lang, k.lang);
#undef SET
	if (changed) {
		settings_save();
		klog("overlay: padlock=%d hud=%d corner=%d games=%d lang=%d",
		    g_padlock, g_hud, g_corner, g_games, g_lang);
	}
	unlock();
	if (changed || loaded)
		overlay_hud_sync();              /* the lines follow the mode and the language */
	return 0;
}

int
vtGetOverlay(VtOverlay *uo)
{
	VtOverlay k;

	memset(&k, 0, sizeof(k));
	k.size = sizeof(k);
	k.padlock_on = (uint8_t)g_padlock;
	k.hud_on = (uint8_t)g_hud;
	k.corner = (uint8_t)g_corner;
	k.in_games = (uint8_t)g_games;
	k.lang = (uint8_t)g_lang;
	return ksceKernelMemcpyKernelToUser(uo, &k, sizeof(k));
}

int
vtSetExitIp(uint32_t ip)
{
	int role = caller_role(ksceKernelGetProcessId());

	if (role != ROLE_APP && role != ROLE_SHELL)
		return -VT_ERR_DENIED;
	g_exit_ip = ip;
	overlay_hud_sync();
	return 0;
}
