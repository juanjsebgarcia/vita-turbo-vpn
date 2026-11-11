/*
 * quickmenu.c - vpnturbo_qm.suprx, the VPN rows in the PS
 * button quick menu.
 *
 * A small client module for QuickMenuReborn, built and installed exactly as
 * Wi-Fi Turbo's wifiturbo_qm.suprx: it is not listed in config.txt;
 * QuickMenuReborn (under *main) loads every .suprx in ux0:QuickMenuReborn/
 * or ur0:QuickMenuReborn/ into SceShell a few seconds after boot, and builds
 * the widgets registered here each time the quick menu opens. Rows:
 *   - "VPN - VPN" with a check box: connect or disconnect the
 *     last-used profile;
 *   - "Status: <state>", with the short state words (QM_ST_*, at most 14
 *     columns, checked by tools/gen_i18n.py);
 *   - "Profile: <nickname>";
 *   - "VPN - HUD" with a check box: the on-screen HUD off, or on in the
 *     last mode used (Compact if none yet; the mode, corner and
 *     in-game are on the Settings page);
 *   - one button: "Pause VPN 5 min", or "Resume VPN" while paused;
 *     dimmed and inert while the tunnel is off.
 * The VPN, status and profile rows carry icons (a padlock, a pulse, a
 * server), written out and loaded as Wi-Fi Turbo's icon is.
 *
 * Like wifiturbo_qm, it talks to the kernel module directly through its
 * syscall library, which it imports strongly: without the kernel module this
 * module fails to load and the quick menu simply shows nothing extra.
 *
 * Rules carried over from Wi-Fi Turbo's quick menu:
 *   - a label is set only from that widget's own OnLoad, or from an event
 *     handler while the menu is open: while the menu is open QuickMenuReborn
 *     applies a label to the live widget at once, and touching a widget that
 *     is not built yet crashed SceShell;
 *   - the check box is never set directly (QuickMenuRebornSetCheckBoxState
 *     always unticks it); the state is saved before the box is built, from
 *     its parent plane's OnLoad, and the default recall applies it.
 * Registration happens in module_start (QuickMenuReborn's loader thread),
 * everything else in QuickMenuReborn's callbacks.
 *
 * Live refresh while the menu stays open (a known issue), compiled in only
 * with VTW_QM_LIVE (CMake option, off by default until checked on the
 * console):
 *   - when our last widget is built (the pause button's OnLoad), the rows
 *     are marked open, a small worker thread is woken and a task is added
 *     to SceShell's PAF main-thread loop (paf_task.h, the same call
 *     QuickMenuReborn uses for its own open/close checks);
 *   - the worker reads the status twice a second (the syscall, and the test
 *     build's file read, stay off the UI thread), formats the three texts
 *     and publishes them;
 *   - the task, on the UI thread, applies only what changed, so a label is
 *     touched a few times a minute at most (the pause countdown, state
 *     changes), and only while the menu is open;
 *   - closed is noticed two ways, either stops the updates: our OnDelete
 *     handler (QuickMenuReborn calls it at close), or the system no longer
 *     reporting the overlay as shown (checked every frame before anything
 *     is touched); the task then removes itself.
 * QuickMenuReborn applies a label to the live PAF widget at once while the
 * menu is open and PAF widgets are not thread-safe, which is why every
 * QuickMenuReborn call stays on the UI thread.
 */
#ifndef VTW_QM_LIVE
#define VTW_QM_LIVE 0
#endif

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <string.h>
#if VTW_QM_LIVE
#include <psp2/appmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "paf_task.h"
#endif

#include "qmr.h"
#include "status.h"
#include "i18n.h"
#include "log.h"
#include "icons/qm_vpn_icon.h"
#include "icons/qm_status_icon.h"
#include "icons/qm_profile_icon.h"
#include "vt_paths.h"

/* QuickMenuReborn keeps every client's widgets in one list keyed by id. */
#define ID_SEP         "vpnturbo_qm_sep"
#define ID_PROF_PLANE  "vpnturbo_qm_prof_plane"
#define ID_PROF_TEXT   "vpnturbo_qm_prof_text"
#define ID_STAT_PLANE  "vpnturbo_qm_stat_plane"
#define ID_STATUS      "vpnturbo_qm_status"
#define ID_TOG_PLANE   "vpnturbo_qm_tog_plane"
#define ID_TOG_TEXT    "vpnturbo_qm_tog_text"
#define ID_TOG_BOX     "vpnturbo_qm_tog_box"
#define ID_HUD_PLANE   "vpnturbo_qm_hud_plane"
#define ID_HUD_TEXT    "vpnturbo_qm_hud_text"
#define ID_HUD_BOX     "vpnturbo_qm_hud_box"
#define ID_PAUSE_PLANE "vpnturbo_qm_pause_plane"
#define ID_PAUSE_BTN   "vpnturbo_qm_pause_btn"

#define ID_TOG_ICON    "vpnturbo_qm_tog_icon"
#define ID_STAT_ICON   "vpnturbo_qm_stat_icon"
#define ID_PROF_ICON   "vpnturbo_qm_prof_icon"

/* QuickMenuReborn loads textures from files, so the icons built into this
 * module are written out once, as Wi-Fi Turbo does with its own. */
#define ICON_DIR       VT_DATA_DIR

typedef struct {
	const char *tex_id, *path;
	const unsigned char *png;
	unsigned size;
} QmIcon;

static const QmIcon k_icons[3] = {
	{ "vpnturbo_qm_tex_vpn",     ICON_DIR "/qm_vpn.png",     k_qm_vpn_icon_png,     sizeof(k_qm_vpn_icon_png) },
	{ "vpnturbo_qm_tex_status",  ICON_DIR "/qm_status.png",  k_qm_status_icon_png,  sizeof(k_qm_status_icon_png) },
	{ "vpnturbo_qm_tex_profile", ICON_DIR "/qm_profile.png", k_qm_profile_icon_png, sizeof(k_qm_profile_icon_png) },
};
enum { ICON_VPN, ICON_STATUS, ICON_PROFILE };

#define PAUSE_SECONDS  300u
#define PAUSE_BTN_W    400.0f
#define PAUSE_BTN_H    60.0f

/* "VPN - <text>" (QM_PREFIX) */
static void prefixed(char *buf, int cap, const char *text)
{
	vtw_format(buf, cap, vtw_i18n(VTW_I18N_QM_FMT_ROW), vtw_i18n(VTW_I18N_QM_PREFIX), text);
}

/* "<label>: <value>" */
/* Cuts a UTF-8 name to max code points, ending it with an ellipsis; the
 * row is 640 px (see gen_i18n.py's width rules). */
#define QM_NAME_MAX 18
static void clip_name(char *s, int max)
{
	int n = 0, i = 0, cut = -1;
	for (; s[i]; i++) {
		if (((unsigned char)s[i] & 0xC0) != 0x80) {   /* a code point starts */
			if (n == max - 1)
				cut = i;
			if (n++ == max) {
				s[cut] = 0;
				strcat(s, "\xE2\x80\xA6");   /* the caller leaves 4 bytes spare */
				return;
			}
		}
	}
}

static void labelled(char *buf, int cap, int label_id, const char *value)
{
	vtw_format(buf, cap, vtw_i18n(VTW_I18N_QM_FMT_LABEL), vtw_i18n(label_id), value);
}

/* Everything the rows show that can change while the menu is open, built
 * from one status read. */
typedef struct {
	char  status[192];   /* "Status: <state>" */
	char  profile[128];  /* "Profile: <nickname>" */
	int   pause_id;      /* VTW_I18N_QM_PAUSE or _RESUME */
	float pause_alpha;   /* 1 = usable, 0.45 = dimmed */
	int   tone;          /* VTW_TONE_*: the status icon's tint */
} QmView;

static void build_view(QmView *v)
{
	VtStatus st;
	char word[128], name[VT_NAME_MAX + 4];   /* + room for the ellipsis */
	int rc = vtc_get_status(&st);

	/* The short words (QM_ST_*): the row is narrower than the Settings page. */
	int word_id = vtw_state_short_text_of(rc, &st, word, sizeof(word));
	labelled(v->status, sizeof(v->status), VTW_I18N_QM_LBL_STATUS, word);

	vtw_profile_text_of(rc, &st, name, VT_NAME_MAX);   /* 4 bytes stay spare */
	clip_name(name, QM_NAME_MAX);
	labelled(v->profile, sizeof(v->profile), VTW_I18N_QM_LBL_PROFILE,
		name[0] ? name : vtw_i18n(VTW_I18N_QM_NO_PROFILE));

	int paused = rc == 0 && st.state == VT_STATE_PAUSED;
	v->pause_id = paused ? VTW_I18N_QM_RESUME : VTW_I18N_QM_PAUSE;
	/* Lockdown refuses a pause: dimmed, as when off. */
	v->pause_alpha = rc == 0 && vtc_wanted(st.state) &&
	                 (!VT_ST_LOCKDOWN(&st) || paused) ? 1.0f : 0.45f;
	v->tone = vtw_state_tone(word_id);
}

/* What the widgets show now, so the live refresh touches only what
 * changed. Written on the UI thread only. */
static QmView g_shown;

/* Each applies one part of a view to widgets that must exist: called from
 * their own OnLoad, from an event handler or from the live task while the
 * menu is open. */
static void apply_status(const QmView *v)
{
	QuickMenuRebornSetWidgetLabel(ID_STATUS, v->status);
	memcpy(g_shown.status, v->status, sizeof(g_shown.status));
#if VTW_QM_LIVE
	/* The pulse icon is built before the status text (same plane), so it
	 * exists here. Signal red for trouble, dimmed while idle. */
	if (v->tone == VTW_TONE_TROUBLE)
		QuickMenuRebornSetWidgetColor(ID_STAT_ICON, 0.863f, 0.110f, 0.173f, 1.0f);
	else
		QuickMenuRebornSetWidgetColor(ID_STAT_ICON, 1.0f, 1.0f, 1.0f,
			v->tone == VTW_TONE_IDLE ? 0.45f : 1.0f);
	g_shown.tone = v->tone;
#endif
}

static void apply_profile(const QmView *v)
{
	QuickMenuRebornSetWidgetLabel(ID_PROF_TEXT, v->profile);
	memcpy(g_shown.profile, v->profile, sizeof(g_shown.profile));
}

static void apply_pause(const QmView *v)
{
	QuickMenuRebornSetWidgetLabel(ID_PAUSE_BTN, vtw_i18n(v->pause_id));
	QuickMenuRebornSetWidgetColor(ID_PAUSE_BTN, 1.0f, 1.0f, 1.0f, v->pause_alpha);
	g_shown.pause_id = v->pause_id;
	g_shown.pause_alpha = v->pause_alpha;
}

static void show_status(void)  { QmView v; build_view(&v); apply_status(&v); }
static void show_profile(void) { QmView v; build_view(&v); apply_profile(&v); }
static void show_pause(void)   { QmView v; build_view(&v); apply_pause(&v); }

#if VTW_QM_LIVE
/* ---- live refresh (VTW_QM_LIVE) ------------------------------------------- */

#define LIVE_PERIOD_US   500000      /* ~2 Hz */

static volatile int g_open;          /* our widgets exist (UI thread writes) */
static volatile int g_gen;           /* bumped by every press and open */
static int g_task_added;             /* UI thread only */
static SceUID g_worker = -1;
static SceUID g_wake = -1;
static volatile int g_quit;

/* The worker's last view, published under a sequence lock (odd while
 * being written). */
static volatile unsigned g_seq;
static QmView g_pub;
static volatile int g_pub_gen;
static unsigned g_seq_seen;          /* UI thread only */

static int overlay_shown(void)
{
	SceAppMgrAppState as;
	memset(&as, 0, sizeof(as));
	/* The size and version QuickMenuReborn passes (0x80, 0x03570000). */
	if (_sceAppMgrGetAppState(&as, sizeof(as), 0x03570000) < 0)
		return 0;
	return as.isSystemUiOverlaid != 0;
}

static int live_worker(SceSize argc, void *argv)
{
	(void)argc; (void)argv;
	while (!g_quit) {
		if (!g_open) {
			/* Asleep while the menu is closed; the timeout is only a
			 * backstop. */
			sceKernelWaitSema(g_wake, 1, &(SceUInt){ 2000000 });
			continue;
		}
		int gen = g_gen;
		QmView v;
		build_view(&v);
		g_seq++;
		__sync_synchronize();
		g_pub = v;
		g_pub_gen = gen;
		__sync_synchronize();
		g_seq++;
		sceKernelDelayThread(LIVE_PERIOD_US);
	}
	return sceKernelExitDeleteThread(0);
}

static void live_task(void *arg);

static void live_stop_task(void)
{
	g_open = 0;
	if (g_task_added) {
		scePafRemoveMainThreadTask(live_task, NULL);
		g_task_added = 0;
	}
}

/* UI thread, every frame while the menu is open. */
static void live_task(void *arg)
{
	(void)arg;
	if (!g_open || !overlay_shown()) {
		live_stop_task();    /* removing itself, as QuickMenuReborn's task does */
		return;
	}
	unsigned s1 = g_seq;
	if (s1 == g_seq_seen || (s1 & 1))
		return;
	__sync_synchronize();
	QmView v = g_pub;
	int gen = g_pub_gen;
	__sync_synchronize();
	if (g_seq != s1)
		return;              /* written meanwhile: next frame */
	g_seq_seen = s1;

	if (gen != g_gen)
		return;              /* read before a press: the next one counts */

	if (strcmp(v.status, g_shown.status) || v.tone != g_shown.tone)
		apply_status(&v);
	if (strcmp(v.profile, g_shown.profile))
		apply_profile(&v);
	if (v.pause_id != g_shown.pause_id || v.pause_alpha != g_shown.pause_alpha)
		apply_pause(&v);
}

/* Our last widget has been built: the rows exist until the menu closes. */
static void live_open(void)
{
	if (g_wake < 0)
		g_wake = sceKernelCreateSema("VtwQmWake", 0, 0, 1, NULL);
	if (g_worker < 0 && g_wake >= 0) {
		g_worker = sceKernelCreateThread("VtwQmLive", live_worker, 0x60, 0x2000, 0, 0, NULL);
		if (g_worker >= 0 && sceKernelStartThread(g_worker, 0, NULL) < 0) {
			sceKernelDeleteThread(g_worker);
			g_worker = -1;
		}
		VTW_LOG("quick menu: live refresh %s", g_worker >= 0 ? "ready" : "NOT available");
	}
	if (g_worker < 0)
		return;              /* rows still refresh on open and after a press */

	g_gen++;
	g_seq_seen = g_seq;      /* ignore anything published before this open */
	g_open = 1;
	sceKernelSignalSema(g_wake, 1);
	if (!g_task_added) {
		/* The return value is not relied on (its meaning is unknown);
		 * adding twice must never happen, so it counts as added. */
		scePafAddMainThreadTask(live_task, NULL);
		g_task_added = 1;
	}
}

/* QuickMenuReborn: the menu has closed and our widgets are gone. Only the
 * flag here; the task removes itself on its next frame. */
static void on_rows_deleted(const char *id)
{
	(void)id;
	g_open = 0;
}
#endif /* VTW_QM_LIVE */

/* ---- OnLoad handlers ------------------------------------------------------ */

static void on_status_load(const char *id)
{
	(void)id;
	show_status();
}

static void on_profile_load(const char *id)
{
	(void)id;
	show_profile();
}

static void on_tog_text_load(const char *id)
{
	char label[96];
	prefixed(label, sizeof(label), vtw_i18n(VTW_I18N_QM_ROW_TOGGLE));
	QuickMenuRebornSetWidgetLabel(id, label);
}

/* The plane is built before its check box: store the current state for the
 * default recall to apply. */
static void on_tog_plane_load(const char *id)
{
	(void)id;
	VtStatus st;
	int on = vtc_get_status(&st) == 0 && vtc_wanted(st.state);
	QuickMenuRebornSaveCheckBoxState(ID_TOG_BOX, on);
}

static void on_hud_text_load(const char *id)
{
	char label[96];
	prefixed(label, sizeof(label), vtw_i18n(VTW_I18N_QM_ROW_HUD));
	QuickMenuRebornSetWidgetLabel(id, label);
}

/* The HUD mode the box turns on: the last one seen that was not Off, as
 * read when the menu opened or chosen here; Compact until there is one. */
static int g_hud_last_on = VT_HUD_COMPACT;

/* As the VPN row: the state is stored before the box is built. */
static void on_hud_plane_load(const char *id)
{
	(void)id;
	VtOverlay o;
	int on = vtc_get_overlay(&o) == 0 && o.hud_on != VT_HUD_OFF;
	if (on)
		g_hud_last_on = o.hud_on;
	QuickMenuRebornSaveCheckBoxState(ID_HUD_BOX, on);
}

/* The pause button is the last widget we register, so when it is built all
 * of ours exist. */
static void on_pause_load(const char *id)
{
	(void)id;
	show_pause();
#if VTW_QM_LIVE
	live_open();
#endif
}

/* ---- events (the menu is open: all our widgets exist) --------------------- */

static void on_toggle(const char *id, SceInt32 hash, SceInt32 event, void *user)
{
	(void)id; (void)hash; (void)event; (void)user;
	int on = QuickMenuRebornGetCheckboxValue(ID_TOG_BOX) > 0;
	int r = on ? vtc_connect() : vtc_disconnect();
	if (r < 0)
		VTW_LOG("quick menu: %s failed 0x%08X", on ? "connect" : "disconnect", (unsigned)r);
#if VTW_QM_LIVE
	g_gen++;
#endif
	show_status();
	show_pause();
}

static void on_hud_toggle(const char *id, SceInt32 hash, SceInt32 event, void *user)
{
	(void)id; (void)hash; (void)event; (void)user;
	int on = QuickMenuRebornGetCheckboxValue(ID_HUD_BOX) > 0;
	VtOverlay o;
	int r = vtc_get_overlay(&o);
	if (r == 0) {
		if (o.hud_on != VT_HUD_OFF)
			g_hud_last_on = o.hud_on;
		o.size = sizeof(o);
		o.hud_on = (uint8_t)(on ? g_hud_last_on : VT_HUD_OFF);
		o.lang = (uint8_t)vtw_i18n_lang();
		r = vtc_set_overlay(&o);
	}
	if (r < 0)
		VTW_LOG("quick menu: HUD %s failed 0x%08X", on ? "on" : "off", (unsigned)r);
}

static void on_pause(const char *id, SceInt32 hash, SceInt32 event, void *user)
{
	(void)id; (void)hash; (void)event; (void)user;
	VtStatus st;
	if (vtc_get_status(&st) != 0 || !vtc_wanted(st.state))
		return;
	int r;
	if (st.state == VT_STATE_PAUSED) {
		r = vtc_resume();
		if (r < 0)
			VTW_LOG("quick menu: resume failed 0x%08X", (unsigned)r);
	} else {
		r = vtc_pause(PAUSE_SECONDS);
		if (r < 0)
			VTW_LOG("quick menu: pause failed 0x%08X", (unsigned)r);
	}
#if VTW_QM_LIVE
	g_gen++;
#endif
	show_status();
	show_pause();
}

/* ---- registration --------------------------------------------------------- */

/* QuickMenuReborn's own text template centres its text, so a label would
 * start further left or right depending on its length. Labels use PAF's
 * default text style, which is left-aligned, with the box's left edge at
 * screen x 161 like the system's own rows (box centre at x = 161 + w/2 - 480
 * relative to the row). */
#define TEXT_LEFT_X 161.0f
#define TEXT_STYLE  "_common_default_style_text"

static void text_row(const char *id, const char *parent, float w, float h, float x,
                     QmrIdHandler onload)
{
	(void)x;
	x = TEXT_LEFT_X + w / 2 - 480.0f;
	QuickMenuRebornRegisterWidgetFromStyle(id, parent, "text", TEXT_STYLE);
	QuickMenuRebornSetWidgetSize(id, w, h, 0, 0);
	QuickMenuRebornSetWidgetColor(id, 1, 1, 1, 1);
	QuickMenuRebornSetWidgetPosition(id, x, 0, 0, 0);
	QuickMenuRebornAssignOnLoadHandler(onload, id);
}

static int icon_file_ready(const QmIcon *ic)
{
	SceIoStat st;
	if (sceIoGetstat(ic->path, &st) >= 0 && st.st_size == (SceOff)ic->size)
		return 1;
	sceIoMkdir(ICON_DIR, 0777);
	SceUID fd = sceIoOpen(ic->path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return 0;
	int n = sceIoWrite(fd, ic->png, ic->size);
	sceIoClose(fd);
	return n == (int)ic->size;
}

/* The row's icon, centred at x 118 on screen like Sony's rows (Wi-Fi Turbo's
 * position); the row works without it if the file cannot be written. */
static void row_icon(const char *id, const char *parent, const QmIcon *ic)
{
	if (!icon_file_ready(ic) || QuickMenuRebornRegisterTexture(ic->tex_id, ic->path) < 0)
		return;
	QuickMenuRebornRegisterWidget(id, parent, QMR_PLANE);
	QuickMenuRebornSetWidgetSize(id, 48, 48, 0, 0);
	QuickMenuRebornSetWidgetColor(id, 1, 1, 1, 1);
	QuickMenuRebornSetWidgetPosition(id, -362, 0, 0, 0);
	QuickMenuRebornSetWidgetTexture(id, ic->tex_id);
}

static void container(const char *id, float h)
{
	QuickMenuRebornRegisterWidget(id, NULL, QMR_PLANE);
	QuickMenuRebornSetWidgetSize(id, QMR_ROW_WIDTH, h, 0, 0);
	QuickMenuRebornSetWidgetColor(id, 1, 1, 1, 0);   /* invisible */
}

int module_start(SceSize argc, const void *args);
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	vtw_log_set_tag("qm");
	VTW_LOG("=== vpnturbo_qm %s ===", VT_VERSION_STR);

	QuickMenuRebornSeparator(ID_SEP, QMR_SEPARATOR_HEIGHT);


	/* VPN toggle first, laid out like Wi-Fi Turbo's Keep Wi-Fi row. */
	container(ID_TOG_PLANE, 80);
	QuickMenuRebornAssignOnLoadHandler(on_tog_plane_load, ID_TOG_PLANE);
	row_icon(ID_TOG_ICON, ID_TOG_PLANE, &k_icons[ICON_VPN]);
	text_row(ID_TOG_TEXT, ID_TOG_PLANE, 640, 75, -152, on_tog_text_load);
	QuickMenuRebornRegisterWidget(ID_TOG_BOX, ID_TOG_PLANE, QMR_CHECK_BOX);
	QuickMenuRebornSetWidgetSize(ID_TOG_BOX, 48, 48, 0, 0);
	QuickMenuRebornSetWidgetColor(ID_TOG_BOX, 1, 1, 1, 1);
	QuickMenuRebornSetWidgetPosition(ID_TOG_BOX, 350, 0, 0, 0);
	QuickMenuRebornAssignDefaultCheckBoxRecall(ID_TOG_BOX);
	QuickMenuRebornRegisterEventHanlder(ID_TOG_BOX, QMR_EVENT_RELEASE, on_toggle, NULL);

	/* Status, then profile, left-aligned with the VPN row's text; each will
	 * get its icon on the left. */
	container(ID_STAT_PLANE, 70);
	row_icon(ID_STAT_ICON, ID_STAT_PLANE, &k_icons[ICON_STATUS]);
	text_row(ID_STATUS, ID_STAT_PLANE, 640, 65, -152, on_status_load);
	container(ID_PROF_PLANE, 70);
	row_icon(ID_PROF_ICON, ID_PROF_PLANE, &k_icons[ICON_PROFILE]);
	text_row(ID_PROF_TEXT, ID_PROF_PLANE, 640, 65, -152, on_profile_load);

	/* HUD on/off, laid out like the VPN row, without an icon. */
	container(ID_HUD_PLANE, 80);
	QuickMenuRebornAssignOnLoadHandler(on_hud_plane_load, ID_HUD_PLANE);
	text_row(ID_HUD_TEXT, ID_HUD_PLANE, 640, 75, -152, on_hud_text_load);
	QuickMenuRebornRegisterWidget(ID_HUD_BOX, ID_HUD_PLANE, QMR_CHECK_BOX);
	QuickMenuRebornSetWidgetSize(ID_HUD_BOX, 48, 48, 0, 0);
	QuickMenuRebornSetWidgetColor(ID_HUD_BOX, 1, 1, 1, 1);
	QuickMenuRebornSetWidgetPosition(ID_HUD_BOX, 350, 0, 0, 0);
	QuickMenuRebornAssignDefaultCheckBoxRecall(ID_HUD_BOX);
	QuickMenuRebornRegisterEventHanlder(ID_HUD_BOX, QMR_EVENT_RELEASE, on_hud_toggle, NULL);

	/* Pause 5 min / Resume now (the last widget: see on_pause_load). */
	container(ID_PAUSE_PLANE, PAUSE_BTN_H + 30);
	QuickMenuRebornRegisterWidget(ID_PAUSE_BTN, ID_PAUSE_PLANE, QMR_BUTTON);
	QuickMenuRebornSetWidgetSize(ID_PAUSE_BTN, PAUSE_BTN_W, PAUSE_BTN_H, 0, 0);
	QuickMenuRebornSetWidgetPosition(ID_PAUSE_BTN, 0, 0, 0, 0);
	QuickMenuRebornRegisterEventHanlder(ID_PAUSE_BTN, QMR_EVENT_RELEASE, on_pause, NULL);
	QuickMenuRebornAssignOnLoadHandler(on_pause_load, ID_PAUSE_BTN);
#if VTW_QM_LIVE
	QuickMenuRebornAssignOnDeleteHandler(on_rows_deleted, ID_PAUSE_BTN);
#endif

	VTW_LOG("quick menu: rows registered%s%s", vtc_is_stub() ? " (test build)" : "",
	        VTW_QM_LIVE ? ", live refresh" : "");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
#if VTW_QM_LIVE
	/* QuickMenuReborn never unloads its clients; best effort, as its own
	 * module_stop does. */
	live_stop_task();
	g_quit = 1;
	if (g_wake >= 0)
		sceKernelSignalSema(g_wake, 1);
#endif
	QuickMenuRebornUnregisterWidget(ID_PAUSE_BTN);
	QuickMenuRebornUnregisterWidget(ID_PAUSE_PLANE);
	QuickMenuRebornUnregisterWidget(ID_HUD_BOX);
	QuickMenuRebornUnregisterWidget(ID_HUD_TEXT);
	QuickMenuRebornUnregisterWidget(ID_HUD_PLANE);
	QuickMenuRebornUnregisterWidget(ID_TOG_BOX);
	QuickMenuRebornUnregisterWidget(ID_TOG_TEXT);
	QuickMenuRebornUnregisterWidget(ID_TOG_PLANE);
	QuickMenuRebornUnregisterWidget(ID_PROF_ICON);
	QuickMenuRebornUnregisterWidget(ID_STAT_ICON);
	QuickMenuRebornUnregisterWidget(ID_TOG_ICON);
	for (int i = 0; i < 3; i++)
		QuickMenuRebornUnregisterTexture(k_icons[i].tex_id);
	QuickMenuRebornUnregisterWidget(ID_STATUS);
	QuickMenuRebornUnregisterWidget(ID_STAT_PLANE);
	QuickMenuRebornUnregisterWidget(ID_PROF_TEXT);
	QuickMenuRebornUnregisterWidget(ID_PROF_PLANE);
	QuickMenuRebornRemoveSeparator(ID_SEP);
	return SCE_KERNEL_STOP_SUCCESS;
}
