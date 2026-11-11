/*
 * settings.c - Settings → Network → "VPN Settings".
 *
 * The technique of Wi-Fi Turbo's Settings page:
 *   - hook sceKernelLoadStartModule in SceSettings and wait for
 *     system_settings_core.suprx;
 *   - then hook scePafMiscLoadXmlLayout to splice a nested setting_list into
 *     the Network page (plugin="network_settings_plugin"), before the page's
 *     closing </setting_list>, exactly where Wi-Fi Turbo puts its entry;
 *   - hook the SceRegMgr get/set/info that SceSystemSettingsCore uses, so the
 *     toggle's key /CONFIG/VPNTURBO/connect goes to the kernel module instead of
 *     the registry;
 *   - hook scePafToplevelGetText: every title and hint carries an id that is
 *     resolved here to translated UTF-16 text, including the status line.
 *
 * Rows: the on/off toggle for the last-used profile and the status line,
 * Lockdown mode (only tunnel traffic may leave, kept across
 * reboots; the kernel accepts Lockdown alone from the Settings app), then
 * the on-screen overlay's settings: the HUD (Off, Compact,
 * Extended), HUD position, in-game HUD and the status-bar padlock, which go
 * to the kernel module's vtSetOverlay (it keeps them in
 * ur0:data/vpnturbo/overlay.txt). HUD position and In-game HUD are greyed
 * out while the HUD is Off. No profile list;
 * that and everything else stays in the app.
 * All hooks are import hooks that pass through anything not ours, so if
 * something does not match on a firmware, the entry is simply missing.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <taihen.h>

#include "settings.h"
#include "status.h"
#include "vtc_ext.h"
#include "i18n.h"
#include "log.h"

/* ---- small string helpers (no libc in this process path) ---------------- */
static int s_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int s_eqn(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
	return 1;
}
static int s_pfx(const char *s, const char *p)
{
	for (int i = 0; p[i]; i++) if (s[i] != p[i]) return 0;
	return 1;
}
static const char *mem_find(const char *h, int hn, const char *n, int nn)
{
	for (int i = 0; i + nn <= hn; i++) {
		int j = 0;
		while (j < nn && h[i + j] == n[j]) j++;
		if (j == nn) return h + i;
	}
	return 0;
}
static void s_cpy(char *d, const char *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

/* ---- the page ------------------------------------------------------------ */

#define VTW_CAT "/CONFIG/VPNTURBO"

/* A nested setting_list is a sub-page the Settings app can enter (a made-up
 * child file is not). The toggle comes first; the status row under it is a button with no handler: it shows
 * its title and a '?' hint and changes nothing (as Wi-Fi Turbo's Bluetooth
 * reminder row). */
static const char X_GROUP[] =
"<setting_list id=\"vpnturbo_settings\" title=\"vpnturbo_t_title\" icon=\"tex_spanner\">"
 "<toggle_switch id=\"vpnturbo_on\" title=\"vpnturbo_t_on\" icon=\"tex_spanner\" hint=\"vpnturbo_h_on\" "
  "key=\"" VTW_CAT "/connect\"/>"
 "<button id=\"vpnturbo_status\" title=\"vpnturbo_t_status\" icon=\"tex_spanner\" hint=\"vpnturbo_h_status\"/>"
 "<toggle_switch id=\"vpnturbo_lockdown\" title=\"vpnturbo_t_lockdown\" icon=\"tex_spanner\" hint=\"vpnturbo_h_lockdown\" "
  "key=\"" VTW_CAT "/lockdown\"/>"
 "<toggle_switch id=\"vpnturbo_fastsend\" title=\"vpnturbo_t_fastsend\" icon=\"tex_spanner\" hint=\"vpnturbo_h_fastsend\" "
  "key=\"" VTW_CAT "/fastsend\"/>"
 "<list id=\"vpnturbo_hud\" title=\"vpnturbo_t_hud\" icon=\"tex_spanner\" hint=\"vpnturbo_h_hud\" "
  "key=\"" VTW_CAT "/hud\">"
  "<list_item id=\"vpnturbo_hud0\" title=\"vpnturbo_i_hud0\" value=\"0\"/>"
  "<list_item id=\"vpnturbo_hud1\" title=\"vpnturbo_i_hud1\" value=\"1\"/>"
  "<list_item id=\"vpnturbo_hud2\" title=\"vpnturbo_i_hud2\" value=\"2\"/>"
 "</list>"
 "<list id=\"vpnturbo_hudpos\" title=\"vpnturbo_t_hudpos\" icon=\"tex_spanner\" hint=\"vpnturbo_h_hudpos\" "
  "key=\"" VTW_CAT "/hudpos\">"
  "<list_item id=\"vpnturbo_pos0\" title=\"vpnturbo_i_pos0\" value=\"0\"/>"
  "<list_item id=\"vpnturbo_pos1\" title=\"vpnturbo_i_pos1\" value=\"1\"/>"
  "<list_item id=\"vpnturbo_pos2\" title=\"vpnturbo_i_pos2\" value=\"2\"/>"
  "<list_item id=\"vpnturbo_pos3\" title=\"vpnturbo_i_pos3\" value=\"3\"/>"
 "</list>"
 "<toggle_switch id=\"vpnturbo_hudgame\" title=\"vpnturbo_t_hudgame\" icon=\"tex_spanner\" hint=\"vpnturbo_h_hudgame\" "
  "key=\"" VTW_CAT "/hudgame\"/>"
 /* Last, and keyed: its get is the first call after the HUD rows exist, where they are greyed
  * (grey_hud_rows). */
 "<toggle_switch id=\"vpnturbo_padlock\" title=\"vpnturbo_t_padlock\" icon=\"tex_spanner\" hint=\"vpnturbo_h_padlock\" "
  "key=\"" VTW_CAT "/padlock\"/>"
"</setting_list>";

#define PAGE_ORIG_MAX 16384
static char g_pagebuf[PAGE_ORIG_MAX + sizeof(X_GROUP)];

/* ---- text for ids (GetText) --------------------------------------------- */
/* The renderer wants UTF-16 (literal text in the XML shows blank). A small
 * ring of buffers so the visible strings never share one. */
typedef unsigned short u16;
static u16 g_txt[6][512];
static int g_txt_next = 0;

static const u16 *to_u16(const char *utf8)
{
	u16 *o = g_txt[g_txt_next];
	g_txt_next = (g_txt_next + 1) % 6;
	vtw_to_u16(utf8, o, (int)(sizeof(g_txt[0]) / sizeof(g_txt[0][0])));
	return o;
}

/* The overlay rows' fixed titles and hints. */
static const struct { const char *id; int str; } k_ovl_text[] = {
	{ "vpnturbo_t_padlock", VTW_I18N_SET_ROW_PADLOCK }, { "vpnturbo_h_padlock", VTW_I18N_SET_HINT_PADLOCK },
	{ "vpnturbo_t_hud",     VTW_I18N_SET_ROW_HUD },     { "vpnturbo_h_hud",     VTW_I18N_SET_HINT_HUD },
	{ "vpnturbo_i_hud0",    VTW_I18N_SET_ITEM_HUD_OFF }, { "vpnturbo_i_hud1",   VTW_I18N_SET_ITEM_HUD_COMPACT },
	{ "vpnturbo_i_hud2",    VTW_I18N_SET_ITEM_HUD_EXTENDED },
	{ "vpnturbo_t_hudpos",  VTW_I18N_SET_ROW_HUDPOS },  { "vpnturbo_h_hudpos",  VTW_I18N_SET_HINT_HUDPOS },
	{ "vpnturbo_i_pos0",    VTW_I18N_SET_POS_TL },      { "vpnturbo_i_pos1",    VTW_I18N_SET_POS_TR },
	{ "vpnturbo_i_pos2",    VTW_I18N_SET_POS_BL },      { "vpnturbo_i_pos3",    VTW_I18N_SET_POS_BR },
	{ "vpnturbo_t_hudgame", VTW_I18N_SET_ROW_HUDGAME }, { "vpnturbo_h_hudgame", VTW_I18N_SET_HINT_HUDGAME },
	{ "vpnturbo_t_lockdown", VTW_I18N_SET_ROW_LOCKDOWN }, { "vpnturbo_h_lockdown", VTW_I18N_SET_HINT_LOCKDOWN },
	{ "vpnturbo_t_fastsend", VTW_I18N_SET_ROW_FASTSEND }, { "vpnturbo_h_fastsend", VTW_I18N_SET_HINT_FASTSEND },
};

/* Between two sentences: a space, except in Japanese and Chinese, which
 * write none. */
static const char *hint_gap(void)
{
	int lang = vtw_i18n_lang();
	return lang == VTW_LANG_JAPANESE || lang == VTW_LANG_CHINESE_T || lang == VTW_LANG_CHINESE_S ? "" : " ";
}

static const u16 *page_text(const char *id)
{
	char line[192];
	VtStatus st;

	if (!s_pfx(id, "vpnturbo_"))
		return 0;
	if (s_eqn(id, "vpnturbo_t_title", sizeof("vpnturbo_t_title")))
		return to_u16(vtw_i18n(VTW_I18N_SET_TITLE));
	if (s_eqn(id, "vpnturbo_t_on", sizeof("vpnturbo_t_on")))
		return to_u16(vtw_i18n(VTW_I18N_SET_ROW_ON));
	if (s_eqn(id, "vpnturbo_h_on", sizeof("vpnturbo_h_on"))) {
		char help[512];
		vtw_format(help, sizeof(help), "%s\n\n%s", vtw_i18n(VTW_I18N_SET_HINT_ON),
		           vtw_i18n(VTW_I18N_SET_HINT_PROFILE));
		return to_u16(help);
	}
	for (unsigned i = 0; i < sizeof(k_ovl_text) / sizeof(k_ovl_text[0]); i++)
		if (s_eqn(id, k_ovl_text[i].id, s_len(k_ovl_text[i].id) + 1))
			return to_u16(vtw_i18n(k_ovl_text[i].str));
	if (s_eqn(id, "vpnturbo_t_status", sizeof("vpnturbo_t_status"))) {
		vtw_status_line(&st, line, sizeof(line));
		return to_u16(line);
	}
	if (s_eqn(id, "vpnturbo_h_status", sizeof("vpnturbo_h_status"))) {
		/* the '?' is pressed later than the page opened: show both */
		char help[512];
		vtw_status_line(&st, line, sizeof(line));
		vtw_format(help, sizeof(help), "%s\n\n%s%s%s", line, vtw_i18n(VTW_I18N_SET_HINT_STATUS),
		           hint_gap(), vtw_i18n(VTW_I18N_SET_HINT_PROFILE));
		return to_u16(help);
	}
	return 0;
}

/* ---- rows: grey out, refresh --------------------------------------------- */
/* SystemSettingsCore's own row exports: grey / enable a row by id, and add /
 * remove it (removing and re-adding makes the page read its text and value
 * again). All look the row up by id and do nothing if it is not there. Only
 * called from callbacks that run once the page is up. */
#define NID_ROW_ENABLE   0xF832D8FB
#define NID_ROW_DISABLE  0x9D058B17
#define NID_ROW_SHOW     0x84029629   /* (id, 1) add / (id, 0) remove */
static void (*g_row_enable)(const char *id), (*g_row_disable)(const char *id);
static int  (*g_row_show)(const char *id, int show);

static int rows_resolve(void)
{
	if (!g_row_enable || !g_row_disable || !g_row_show) {
		uintptr_t e = 0, d = 0, v = 0;
		if (taiGetModuleExportFunc("SceSystemSettingsCore", TAI_ANY_LIBRARY, NID_ROW_ENABLE, &e) < 0 ||
		    taiGetModuleExportFunc("SceSystemSettingsCore", TAI_ANY_LIBRARY, NID_ROW_DISABLE, &d) < 0 ||
		    taiGetModuleExportFunc("SceSystemSettingsCore", TAI_ANY_LIBRARY, NID_ROW_SHOW, &v) < 0 ||
		    !e || !d || !v)
			return 0;
		g_row_enable  = (void (*)(const char *))e;
		g_row_disable = (void (*)(const char *))d;
		g_row_show    = (int (*)(const char *, int))v;
	}
	return 1;
}

/* HUD position and In-game HUD mean nothing while the HUD is Off. */
static void grey_hud_rows(int hud_mode)
{
	if (!rows_resolve())
		return;
	void (*f)(const char *) = hud_mode ? g_row_enable : g_row_disable;
	f("vpnturbo_hudpos");
	f("vpnturbo_hudgame");
}

/* The status row reads the tunnel's state when it is built: rebuild it. */
static void refresh_status_row(void)
{
	if (!rows_resolve())
		return;
	g_row_show("vpnturbo_status", 0);
	g_row_show("vpnturbo_status", 1);
}

/* ---- the toggle ---------------------------------------------------------- */
/* Remember a change made here for a few seconds, so the page's re-read right
 * after the set shows what was chosen (Wi-Fi Turbo does the same). */
#define SET_SETTLE_US 3000000ull
static int       g_set_val = 0;
static SceUInt64 g_set_t = 0;
static int       g_set_valid = 0;

static int toggle_get(void)
{
	SceUInt64 now = sceKernelGetProcessTimeWide();
	if (g_set_valid && now - g_set_t < SET_SETTLE_US)
		return g_set_val;
	VtStatus st;
	if (vtc_get_status(&st) < 0)
		return 0;
	return vtc_wanted(st.state);
}

static void toggle_set(int on)
{
	int r = on ? vtc_connect() : vtc_disconnect();
	g_set_val = on;
	g_set_t = sceKernelGetProcessTimeWide();
	g_set_valid = 1;
	if (r < 0)
		VTW_LOG("settings: %s failed 0x%08X", on ? "connect" : "disconnect", (unsigned)r);
	refresh_status_row();
}

/* ---- Lockdown mode ----------------------------------------------------------- */
/* Kept in the kernel module (ur0:data/vpnturbo/options.txt); the same
 * settle window as the connect toggle, so the re-read shows the choice. */
static int       g_ld_val = 0;
static SceUInt64 g_ld_t = 0;
static int       g_ld_valid = 0;

static int lockdown_get(void)
{
	if (g_ld_valid && sceKernelGetProcessTimeWide() - g_ld_t < SET_SETTLE_US)
		return g_ld_val;
	VtStatus st;
	if (vtc_get_status(&st) < 0)
		return 0;
	return VT_ST_LOCKDOWN(&st) != 0;
}

static void lockdown_set(int on)
{
	int r = vtc_set_lockdown(on);
	g_ld_val = on;
	g_ld_t = sceKernelGetProcessTimeWide();
	g_ld_valid = 1;
	if (r < 0)
		VTW_LOG("settings: lockdown %s failed 0x%08X", on ? "on" : "off", (unsigned)r);
}

/* ---- faster sending ---------------------------------------------------- */
/* Kept in the kernel module like Lockdown; the plugin moves its sending
 * thread within a second. Same settle window. */
static int       g_fs_val = 0;
static SceUInt64 g_fs_t = 0;
static int       g_fs_valid = 0;

static int fastsend_get(void)
{
	if (g_fs_valid && sceKernelGetProcessTimeWide() - g_fs_t < SET_SETTLE_US)
		return g_fs_val;
	VtStatus st;
	if (vtc_get_status(&st) < 0)
		return 0;
	return VT_ST_SENDCORE(&st) != 0;
}

static void fastsend_set(int on)
{
	int r = vtc_set_sendcore(on);
	g_fs_val = on;
	g_fs_t = sceKernelGetProcessTimeWide();
	g_fs_valid = 1;
	if (r < 0)
		VTW_LOG("settings: faster sending %s failed 0x%08X", on ? "on" : "off", (unsigned)r);
}

/* ---- the overlay settings ------------------------------------- */
/* The value lives in the kernel module, which answers at once, so no settle
 * time is needed. Corner and HUD mode values match the list items
 * (VT_CORNER_*, VT_HUD_*). */
enum { OVL_NONE = -1, OVL_PADLOCK, OVL_HUD, OVL_HUDPOS, OVL_HUDGAME };

static int ovl_key(const char *name)
{
	if (!name) return OVL_NONE;
	if (s_eqn(name, "padlock", 8)) return OVL_PADLOCK;
	if (s_eqn(name, "hud", 4))     return OVL_HUD;
	if (s_eqn(name, "hudpos", 7))  return OVL_HUDPOS;
	if (s_eqn(name, "hudgame", 8)) return OVL_HUDGAME;
	return OVL_NONE;
}

static int ovl_get(int key)
{
	VtOverlay o;
	if (vtc_get_overlay(&o) < 0)
		return key == OVL_PADLOCK ? 1 : key == OVL_HUDPOS ? VT_CORNER_BOTTOM_LEFT : 0;
	switch (key) {
	case OVL_PADLOCK: return o.padlock_on;
	case OVL_HUD:     return o.hud_on;
	case OVL_HUDPOS:  return o.corner;
	default:          return o.in_games;
	}
}

static void ovl_set(int key, int v)
{
	VtOverlay o;
	if (vtc_get_overlay(&o) < 0)
		return;
	o.size = sizeof(o);
	switch (key) {
	case OVL_PADLOCK: o.padlock_on = v != 0; break;
	case OVL_HUD:     if (v >= VT_HUD_OFF && v <= VT_HUD_EXTENDED) o.hud_on = (uint8_t)v; break;
	case OVL_HUDPOS:  if (v >= VT_CORNER_TOP_LEFT && v <= VT_CORNER_BOTTOM_RIGHT) o.corner = (uint8_t)v; break;
	default:          o.in_games = v != 0; break;
	}
	o.lang = (uint8_t)vtw_i18n_lang();   /* the HUD's words follow the system */
	int r = vtc_set_overlay(&o);
	if (r < 0)
		VTW_LOG("settings: overlay key %d = %d failed 0x%08X", key, v, (unsigned)r);
}

/* ---- hooks ---------------------------------------------------------------- */
static SceUID g_h_load = -1, g_h_xml = -1, g_h_get = -1, g_h_set = -1, g_h_info = -1, g_h_text = -1;
static tai_hook_ref_t r_load, r_xml, r_get, r_set, r_info, r_text;
static int g_installed = 0;

/* Exact category match, NUL included. */
static int is_ours(const char *cat) { return cat && s_eqn(cat, VTW_CAT, (int)sizeof(VTW_CAT)); }

/* scePafMiscLoadXmlLayout(out, buf, size, a4): splice our group into the
 * Network page before its last </setting_list>, as Wi-Fi Turbo does. */
static int xml_hook(void *out, void *buf, int size, void *a4)
{
	static const char PLUG[] = "plugin=\"network_settings_plugin\"";
	static const char CLOSE[] = "</setting_list>";
	const char *s = (const char *)buf;

	if (s && size > 0 && mem_find(s, size, PLUG, (int)sizeof(PLUG) - 1)) {
		if (size > PAGE_ORIG_MAX) {
			VTW_LOG("settings: network page too big (%d) - not touched", size);
			return TAI_CONTINUE(int, r_xml, out, buf, size, a4);
		}
		const char *ins = 0, *p = s;
		for (;;) {
			const char *f = mem_find(p, size - (int)(p - s), CLOSE, (int)sizeof(CLOSE) - 1);
			if (!f) break;
			ins = f;
			p = f + sizeof(CLOSE) - 1;
		}
		if (!ins)
			VTW_LOG("settings: network page (size=%d): no closing tag, not touched", size);
		if (ins) {
			int head = (int)(ins - s), gl = (int)sizeof(X_GROUP) - 1;
			s_cpy(g_pagebuf, s, head);
			s_cpy(g_pagebuf + head, X_GROUP, gl);
			s_cpy(g_pagebuf + head + gl, ins, size - head);
			buf = g_pagebuf;
			size += gl;
		}
	}
	return TAI_CONTINUE(int, r_xml, out, buf, size, a4);
}

static int get_hook(const char *cat, const char *name, int *val)
{
	if (is_ours(cat) && val) {
		int k = ovl_key(name);
		*val = k != OVL_NONE ? ovl_get(k) : (name && s_eqn(name, "connect", 8)) ? toggle_get() :
		       (name && s_eqn(name, "lockdown", 9)) ? lockdown_get() :
		       (name && s_eqn(name, "fastsend", 9)) ? fastsend_get() : 0;
		/* The padlock row is the last one: by the time the page asks for it,
		 * the HUD rows exist and can be greyed. */
		if (k == OVL_PADLOCK)
			grey_hud_rows(ovl_get(OVL_HUD));
		return 0;
	}
	return TAI_CONTINUE(int, r_get, cat, name, val);
}

static int set_hook(const char *cat, const char *name, int v)
{
	if (is_ours(cat)) {
		int k = ovl_key(name);
		if (k != OVL_NONE) {
			ovl_set(k, v);
			if (k == OVL_HUD)
				grey_hud_rows(ovl_get(OVL_HUD));   /* what the kernel kept */
		} else if (name && s_eqn(name, "connect", 8))
			toggle_set(v != 0);
		else if (name && s_eqn(name, "lockdown", 9))
			lockdown_set(v != 0);
		else if (name && s_eqn(name, "fastsend", 9))
			fastsend_set(v != 0);
		return 0;
	}
	return TAI_CONTINUE(int, r_set, cat, name, v);
}

static int info_hook(const char *cat, void *info, int unk)
{
	if (is_ours(cat)) {
		if (info) *(int *)((char *)info + 8) = 0x00040000;   /* int key */
		return 0;
	}
	return TAI_CONTINUE(int, r_info, cat, info, unk);
}

/* scePafToplevelGetText(arg, char **msg) -> text for a title/hint id. */
static void *text_hook(void *arg, char **msg)
{
	if (msg && *msg) {
		const u16 *t = page_text(*msg);
		if (t) return (void *)t;
	}
	return TAI_CONTINUE(void *, r_text, arg, msg);
}

/* NID 0x2DCC4AFA as SceSettings imports it takes SIX arguments, including
 * int *status. A wrong arity corrupts the hook chain shared with the other
 * Settings plugins (Wi-Fi Turbo's note). */
static SceUID load_hook(const char *path, SceSize args, void *argp,
                        int flags, void *option, int *status)
{
	SceUID ret = TAI_CONTINUE(SceUID, r_load, path, args, argp, flags, option, status);
	if (!g_installed && path && mem_find(path, s_len(path), "system_settings_core", 20)) {
		g_installed = 1;
		g_h_xml  = taiHookFunctionImport(&r_xml,  "SceSettings",
		              0x3D643CE8, 0x19FE55A8, xml_hook);      /* ScePafMisc */
		g_h_get  = taiHookFunctionImport(&r_get,  "SceSystemSettingsCore",
		              0xC436F916, 0x16DDF3DC, get_hook);
		g_h_set  = taiHookFunctionImport(&r_set,  "SceSystemSettingsCore",
		              0xC436F916, 0xD72EA399, set_hook);
		g_h_info = taiHookFunctionImport(&r_info, "SceSystemSettingsCore",
		              0xC436F916, 0x58421DD1, info_hook);
		g_h_text = taiHookFunctionImport(&r_text, "SceSystemSettingsCore",
		              0x4D9A9DD0, 0x19CEFDA7, text_hook);     /* scePafToplevelGetText */
		VTW_LOG("settings: core loaded, hooks xml=0x%08X get=0x%08X set=0x%08X info=0x%08X text=0x%08X",
		        (unsigned)g_h_xml, (unsigned)g_h_get, (unsigned)g_h_set,
		        (unsigned)g_h_info, (unsigned)g_h_text);
	}
	return ret;
}

void vtw_settings_install(void)
{
	g_h_load = taiHookFunctionImport(&r_load, "SceSettings",
	              0xCAE9ACE6, 0x2DCC4AFA, load_hook);   /* sceKernelLoadStartModule */
	VTW_LOG("settings: LoadStartModule hook 0x%08X, module %d",
	        (unsigned)g_h_load, vtc_available());
	/* Settings shows the status bar: let the padlock and HUD draw here. */
	vtc_overlay_register();
}

void vtw_settings_release(void)
{
	if (g_h_text >= 0) { taiHookRelease(g_h_text, r_text); g_h_text = -1; }
	if (g_h_info >= 0) { taiHookRelease(g_h_info, r_info); g_h_info = -1; }
	if (g_h_set  >= 0) { taiHookRelease(g_h_set,  r_set);  g_h_set  = -1; }
	if (g_h_get  >= 0) { taiHookRelease(g_h_get,  r_get);  g_h_get  = -1; }
	if (g_h_xml  >= 0) { taiHookRelease(g_h_xml,  r_xml);  g_h_xml  = -1; }
	if (g_h_load >= 0) { taiHookRelease(g_h_load, r_load); g_h_load = -1; }
}
