/*
 * app.c - navigation, actions and input. Drawing is in screens.c.
 *
 * Buttons act through the footer hints the current screen drew last frame,
 * so what a button does is always what its hint says. Touch works through
 * the targets registered while drawing.
 */
#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "camera.h"
#include "profiles.h"
#include "qr.h"
#include "vtconf.h"

App g_app;

#define PAUSE_SECONDS (5 * 60)
#define TOAST_MS 2500

/* ---- formatting -------------------------------------------------------- */

const char *fmt_ip(uint32_t ip, char *out, size_t n)
{
	snprintf(out, n, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
	return out;
}

const char *fmt_ip_port(uint32_t ip, uint16_t port, char *out, size_t n)
{
	snprintf(out, n, "%u.%u.%u.%u:%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255, port);
	return out;
}

const char *fmt_clock(uint32_t s, char *out, size_t n)
{
	if (s >= 3600)
		snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, n, "%u:%02u", s / 60, s % 60);
	return out;
}

const char *fmt_hms(uint32_t s, char *out, size_t n)
{
	snprintf(out, n, "%02u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	return out;
}

const char *fmt_bytes(uint64_t b, char *out, size_t n)
{
	if (b >= 1000000000ull)
		snprintf(out, n, "%.1f GB", b / 1e9);
	else if (b >= 1000000)
		snprintf(out, n, "%.1f MB", b / 1e6);
	else if (b >= 1000)
		snprintf(out, n, "%.1f kB", b / 1e3);
	else
		snprintf(out, n, "%u B", (unsigned)b);
	return out;
}

const char *profile_display(const BkProfile *p)
{
	return p->nickname[0] ? p->nickname : p->name;
}

const char *profile_routes(const BkProfile *p, char *out, size_t n)
{
	char net[24];
	if (p->full_tunnel)
		return trf(out, n, S_ROUTES_ALL, NULL, NULL);
	if (p->allowed_count == 1) {
		char ip[16];
		snprintf(net, sizeof(net), "%s/%u", fmt_ip(p->allowed[0].net, ip, sizeof(ip)),
			p->allowed[0].prefix);
		return trf(out, n, S_ROUTES_ONLY, net, NULL);
	}
	snprintf(net, sizeof(net), "%u", p->allowed_count);
	return trf(out, n, S_ROUTES_N, net, NULL);
}

/*
 * A short name for the exit in the migrate diagram: a trailing country
 * code ("VPS EU" -> "EU"), else the first word.
 */
const char *profile_exit_label(const BkProfile *p, char *out, size_t n)
{
	const char *s = profile_display(p);
	const char *last = strrchr(s, ' ');
	if (last) {
		size_t len = strlen(last + 1);
		int caps = len >= 2 && len <= 3;
		for (size_t i = 0; caps && i < len; i++)
			caps = last[1 + i] >= 'A' && last[1 + i] <= 'Z';
		if (caps) {
			snprintf(out, n, "%s", last + 1);
			return out;
		}
	}
	size_t len = strcspn(s, " ");
	if (len >= n)
		len = n - 1;
	memcpy(out, s, len);
	out[len] = 0;
	return out;
}

const char *error_cause(BkError e)
{
	switch (e) {
	case BK_ERR_RESOLVE:
		return tr(S_CAUSE_RESOLVE);
	case BK_ERR_WIFI_OFF:
		return tr(S_CAUSE_WIFI_OFF);
	case BK_ERR_PROFILE:
		return tr(S_CAUSE_PROFILE);
	case BK_ERR_SOCKET:
		return tr(S_CAUSE_SOCKET);
	case BK_ERR_CLOCK:
		return tr(S_CAUSE_CLOCK);
	default:
		return tr(S_CAUSE_NO_ANSWER);
	}
}

/* ---- derived state ----------------------------------------------------- */

HomeView home_view(const App *a)
{
	/* without the SceShell plugin nothing carries the tunnel's traffic */
	if (!a->comp.module_loaded || !a->comp.module_api_ok || !a->comp.shell_loaded)
		return HV_MISSING;
	if (a->st.locked)
		return HV_BLOCKED;
	switch (a->st.state) {
	case BK_UP:
		return HV_PROTECTED;
	case BK_HANDSHAKING:
		return HV_CONNECTING;
	case BK_STALLED:
	case BK_ERROR:
		return ks_effective(&a->st) ? HV_HELD : HV_UNPROTECTED;
	case BK_PAUSED:
		return HV_PAUSED;
	default:
		return a->st.lockdown_block ? HV_LOCKDOWN : HV_IDLE;
	}
}

/* Recent, then loose files, then each folder or zip */
void build_profile_list(App *a)
{
	static char group_titles[BK_MAX_PROFILES][56];
	int n = 0, ngroups = 0;
	const char *groups[BK_MAX_PROFILES];

	int any_recent = 0, any_group = 0;
	for (int i = 0; i < a->nprof; i++) {
		any_recent |= a->prof[i].recent > 0;
		any_group |= a->prof[i].group[0] != 0;
	}
	if (any_recent) {
		a->items[n++] = (ListItem){ 1, tr(S_GROUP_RECENT), -1 };
		for (int r = 1; r <= 5; r++)
			for (int i = 0; i < a->nprof; i++)
				if (a->prof[i].recent == r && n < MAX_ITEMS)
					a->items[n++] = (ListItem){ 0, NULL, i };
	}
	int top_header = any_recent || any_group;
	int top_done = 0;
	for (int i = 0; i < a->nprof && n < MAX_ITEMS; i++) {
		if (a->prof[i].group[0])
			continue;
		if (top_header && !top_done) {
			a->items[n++] = (ListItem){ 1, tr(S_GROUP_TOP), -1 };
			top_done = 1;
		}
		a->items[n++] = (ListItem){ 0, NULL, i };
	}
	for (int i = 0; i < a->nprof; i++) {
		const char *g = a->prof[i].group;
		if (!g[0])
			continue;
		int seen = 0;
		for (int k = 0; k < ngroups; k++)
			seen |= !strcmp(groups[k], g);
		if (seen || ngroups >= BK_MAX_PROFILES)
			continue;
		groups[ngroups] = g;
		if (a->prof[i].in_zip)
			snprintf(group_titles[ngroups], sizeof(group_titles[0]), "%s", g);
		else
			snprintf(group_titles[ngroups], sizeof(group_titles[0]), "%s/", g);
		if (n < MAX_ITEMS)
			a->items[n++] = (ListItem){ 1, group_titles[ngroups], -1 };
		ngroups++;
		for (int j = i; j < a->nprof && n < MAX_ITEMS; j++)
			if (!strcmp(a->prof[j].group, g))
				a->items[n++] = (ListItem){ 0, NULL, j };
	}
	a->nitems = n;
}

int selected_profile(const App *a)
{
	int row = 0;
	for (int i = 0; i < a->nitems; i++)
		if (!a->items[i].header) {
			if (row == a->sel)
				return a->items[i].profile;
			row++;
		}
	return -1;
}

static int profile_rows(const App *a)
{
	int rows = 0;
	for (int i = 0; i < a->nitems; i++)
		rows += !a->items[i].header;
	return rows;
}

/* Every usable profile but the active one; seamless moves first */
void build_migrate_list(App *a)
{
	int n = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < a->nprof; i++) {
			if (i == a->st.profile || (a->prof[i].warnings & BK_WARN_INVALID))
				continue;
			int seamless = bk_migrate_kind(i) == BK_MIGRATE_SEAMLESS;
			if (seamless == (pass == 0))
				a->mig[n++] = i;
		}
	a->nmig = n;
	if (a->mig_sel >= n)
		a->mig_sel = n ? n - 1 : 0;
}

static void refresh(App *a)
{
	bk_get_status(&a->st);
	bk_get_components(&a->comp);
	bk_check_get(&a->chk);
	a->nprof = bk_profile_count();
	if (a->nprof > BK_MAX_PROFILES)
		a->nprof = BK_MAX_PROFILES;
	for (int i = 0; i < a->nprof; i++)
		bk_profile_get(i, &a->prof[i]);
	if (a->st.profile >= 0)
		a->last_profile = a->st.profile;
	if (a->last_profile >= a->nprof)
		a->last_profile = a->nprof ? 0 : -1;
	build_profile_list(a);
	int rows = profile_rows(a);
	if (a->sel >= rows)
		a->sel = rows ? rows - 1 : 0;
	build_migrate_list(a);
}

/* ---- navigation -------------------------------------------------------- */

static void toast(App *a, const char *s)
{
	snprintf(a->toast, sizeof(a->toast), "%s", s);
	a->toast_until = a->now + TOAST_MS;
}

/* Select the row of profile p on the Profiles list */
static void select_profile(App *a, int p)
{
	int row = 0;
	for (int i = 0; i < a->nitems; i++)
		if (!a->items[i].header) {
			if (a->items[i].profile == p) {
				a->sel = row;
				return;
			}
			row++;
		}
}

static void open_screen(App *a, Screen s)
{
	if (s == a->screen)
		return;
	if (a->depth < (int)(sizeof(a->stack) / sizeof(a->stack[0])))
		a->stack[a->depth++] = a->screen;
	a->screen = s;
	if (s == SCR_SCAN) {
		a->cam_tried = 0;
		a->cam_error = 0;
		a->scan_invalid = 0;
		a->scan_hash = 0;
		a->scan_seen = 1000;
	}
	if (s == SCR_PROFILES) {
		/* start on the active profile */
		select_profile(a, a->st.profile);
		if (!a->key_notice_done && a->nprof > 0) {
			a->dialog = DLG_KEY_NOTICE;
			a->key_notice_done = 1;
		}
	}
	if (s == SCR_LEAK && a->chk.state == BK_CHECK_NOT_RUN)
		bk_check_start();
}

static void back(App *a)
{
	if (a->depth > 0)
		a->screen = a->stack[--a->depth];
	else
		a->screen = SCR_HOME;
}

static void go_home(App *a)
{
	a->depth = 0;
	a->screen = SCR_HOME;
}

static void connect(App *a, int p, int checked)
{
	char buf[96];
	if (p < 0 || p >= a->nprof) {
		open_screen(a, SCR_PROFILES);
		return;
	}
	const BkProfile *pr = &a->prof[p];
	if (pr->warnings & BK_WARN_INVALID) {
		toast(a, trf(buf, sizeof(buf), S_TOAST_INVALID, profile_display(pr), NULL));
		return;
	}
	/* already on a tunnel: that is a migrate */
	if (a->st.state != BK_IDLE && a->st.profile >= 0 && a->st.profile != p &&
	    a->screen != SCR_MIGRATE) {
		for (int i = 0; i < a->nmig; i++)
			if (a->mig[i] == p)
				a->mig_sel = i;
		open_screen(a, SCR_MIGRATE);
		return;
	}
	if (a->st.profile == p && a->st.state != BK_IDLE) {
		go_home(a);
		return;
	}
	if (a->comp.module_api_ok && !a->comp.shell_loaded) {
		toast(a, tr(S_SUB_PLUGIN_MISSING));
		return;
	}
	if ((pr->warnings & BK_WARN_CLASH) && !checked) {
		a->dialog = DLG_CLASH;
		a->dialog_arg = p;
		return;
	}
	if (bk_connect(p) == 0)
		go_home(a);
	else
		toast(a, tr(S_TOAST_FAILED));
}

static void set_option(App *a, int which)
{
	BkOptions o;
	bk_get_options(&o);
	if (which == A_KILL_SWITCH)
		o.kill_switch = !o.kill_switch;
	else if (which == A_ALLOW_LAN)
		o.allow_lan = !o.allow_lan;
	else
		o.lockdown = which == A_LOCKDOWN_ON;
	int r = bk_set_options(&o);
	if (which == A_KILL_SWITCH)
		toast(a, tr(o.kill_switch ? S_TOAST_KS_ON : S_TOAST_KS_OFF));
	else if (which == A_ALLOW_LAN)
		toast(a, tr(o.allow_lan ? S_TOAST_LAN_ON : S_TOAST_LAN_OFF));
	else if (r != 0)
		toast(a, tr(S_TOAST_LD_FAILED));
	else
		toast(a, tr(o.lockdown ? S_TOAST_LD_ON : S_TOAST_LD_OFF));
}

/* Lockdown: turning it off is immediate; turning it on blocks everything
 * while disconnected, so it asks first */
static void lockdown_toggle(App *a)
{
	if (!a->st.lockdown_avail)
		return;
	if (a->st.lockdown) {
		set_option(a, A_LOCKDOWN);
	} else {
		a->dialog = DLG_LOCKDOWN;
		a->dialog_arg = 0;
	}
}

static void select_row(App *a, int pos, int activate)
{
	if (a->screen == SCR_PROFILES) {
		if (activate && pos == a->sel)
			connect(a, selected_profile(a), 0);
		a->sel = pos;
	} else if (a->screen == SCR_MIGRATE) {
		if (activate && pos == a->mig_sel && pos < a->nmig) {
			if (bk_migrate(a->mig[pos]) == 0)
				go_home(a);
		}
		a->mig_sel = pos;
	}
}

static void act(App *a, int action, int arg, int touch)
{
	char buf[96];
	switch (action) {
	case A_CONNECT:
		connect(a, arg >= 0 ? arg : a->last_profile, 0);
		break;
	case A_CONNECT_SEL:
		connect(a, a->screen == SCR_DETAILS ? a->detail_profile : selected_profile(a), 0);
		break;
	case A_CONNECT_ANYWAY:
		a->dialog = DLG_NONE;
		connect(a, a->dialog_arg, 1);
		break;
	case A_DISCONNECT_ASK:
		a->dialog = DLG_DISCONNECT;
		a->dialog_arg = arg;
		break;
	case A_DISCONNECT:
		a->dialog = DLG_NONE;
		bk_disconnect();
		toast(a, tr(S_TOAST_DISCONNECTED));
		go_home(a);
		break;
	case A_RETRY:
		bk_retry();
		toast(a, tr(S_TOAST_RETRYING));
		break;
	case A_KILL_SWITCH:
	case A_ALLOW_LAN:
		set_option(a, action);
		break;
	case A_LOCKDOWN:
		lockdown_toggle(a);
		break;
	case A_LOCKDOWN_ON:
		a->dialog = DLG_NONE;
		set_option(a, A_LOCKDOWN_ON);
		break;
	case A_PAUSE: {
		/* the module refuses a pause under Lockdown */
		int r = a->st.lockdown ? BK_PAUSE_LOCKDOWN : bk_pause(PAUSE_SECONDS);
		if (r == 0) {
			toast(a, tr(S_TOAST_PAUSED));
			go_home(a);
			bk_trace("pause: toast and home done");
			a->trace_frames = 60;   /* the next frames, logged */
		} else if (r == BK_PAUSE_LOCKDOWN) {
			toast(a, tr(S_TOAST_PAUSE_LOCKDOWN));
		}
		break;
	}
	case A_RESUME:
		bk_resume();
		go_home(a);
		break;
	case A_OPEN:
		if (arg == SCR_DETAILS)
			a->detail_profile = -1;
		open_screen(a, (Screen)arg);
		break;
	case A_BACK:
		back(a);
		break;
	case A_PROFILE_DETAILS:
		a->detail_profile = selected_profile(a);
		if (a->detail_profile >= 0)
			open_screen(a, SCR_DETAILS);
		break;
	case A_SELECT:
		select_row(a, arg, touch);
		break;
	case A_MIGRATE_SEL:
		if (a->mig_sel < a->nmig && bk_migrate(a->mig[a->mig_sel]) == 0)
			go_home(a);
		break;
	case A_LEAK_RUN:
		bk_check_start();
		break;
	case A_NET_SETTINGS:
		if (bk_open_network_settings() != 0)
			toast(a, tr(S_TOAST_SETTINGS));
		break;
	case A_RESCAN:
		bk_rescan();
		toast(a, trf(buf, sizeof(buf), S_TOAST_RESCANNED, NULL, NULL));
		break;
	case A_DIALOG_OK:
	case A_DIALOG_CANCEL:
		a->dialog = DLG_NONE;
		break;
	case A_CHECK_AGAIN:
		bk_init();
		break;
	case A_GUARD_RESET:
		bk_guard_reset();
		break;
	case A_SCAN_QR:
		open_screen(a, SCR_SCAN);
		break;
	default:
		break;
	}
}

static int button_of(uint32_t pressed)
{
	if (pressed & IN_CROSS)
		return BTN_CROSS;
	if (pressed & IN_TRIANGLE)
		return BTN_TRIANGLE;
	if (pressed & IN_SQUARE)
		return BTN_SQUARE;
	if (pressed & IN_CIRCLE)
		return BTN_CIRCLE;
	if (pressed & IN_START)
		return BTN_START;
	return -1;
}

static void handle_input(App *a, const AppInput *in)
{
	char buf[96];
	if (in->pressed & IN_SELECT && bk_scenario_count() > 0) {
		int sc = (bk_scenario_get() + 1) % bk_scenario_count();
		bk_scenario_set(sc);
		refresh(a);
		toast(a, trf(buf, sizeof(buf), S_TOAST_SCENARIO, bk_scenario_name(sc), NULL));
		return;
	}
	if (in->tap) {
		const Hit *h = ui_hit_at(in->tx, in->ty);
		if (h)
			act(a, h->action, h->arg, 1);
		return;
	}
	int b = button_of(in->pressed);
	if (b >= 0) {
		for (int i = 0; i < a->nhints; i++)
			if (a->hints[i].button == b) {
				act(a, a->hints[i].action, a->hints[i].arg, 0);
				return;
			}
		/* Details leaves Pause out under Lockdown: △ still says why */
		if (b == BTN_TRIANGLE && !a->dialog && a->screen == SCR_DETAILS && a->st.lockdown &&
		    a->st.state != BK_IDLE && a->st.state != BK_PAUSED)
			act(a, A_PAUSE, 0, 0);
		/* Start does what its hint says, else its usual job below */
		if (b != BTN_START)
			return;
	}
	if (a->dialog)
		return;
	if (in->pressed & IN_START && a->screen == SCR_HOME) {
		act(a, A_OPEN, SCR_HELP, 0);
		return;
	}
	if (in->pressed & IN_START && a->screen == SCR_DETAILS) {
		act(a, A_LOCKDOWN, 0, 0);
		return;
	}
	if (a->screen == SCR_DETAILS && in->pressed & (IN_L | IN_R)) {
		set_option(a, in->pressed & IN_L ? A_KILL_SWITCH : A_ALLOW_LAN);
		return;
	}
	int dir = in->pressed & IN_UP ? -1 : in->pressed & IN_DOWN ? 1 : 0;
	if (!dir)
		return;
	if (a->screen == SCR_PROFILES) {
		int rows = profile_rows(a);
		if (rows)
			a->sel = (a->sel + dir + rows) % rows;
	} else if (a->screen == SCR_MIGRATE && a->nmig) {
		a->mig_sel = (a->mig_sel + dir + a->nmig) % a->nmig;
	}
}

void hint(App *a, int button, const char *label, int action, int arg, int right)
{
	if (a->nhints >= MAX_HINTS)
		return;
	int i = a->nhints++;
	snprintf(a->hint_buf[i], sizeof(a->hint_buf[i]), "%s", label);
	a->hints[i] = (Hint){ button, a->hint_buf[i], action, arg, right, 0 };
}

void hint_dim(App *a)
{
	if (a->nhints > 0)
		a->hints[a->nhints - 1].dim = 1;
}

int ks_effective(const BkStatus *st)
{
	return st->kill_switch || st->lockdown;
}

/* ---- QR import ----------------------------------------- */

static uint32_t fnv1a(const char *p, size_t n)
{
	uint32_t h = 2166136261u;
	while (n--)
		h = (h ^ (unsigned char)*p++) * 16777619u;
	return h ? h : 1;
}

/* A decoded code: save it as a profile, or say why not */
static void import_text(App *a, const char *text, size_t len)
{
	char buf[160];
	BkImport r;
	uint32_t h = fnv1a(text, len);
	if (h == a->scan_hash)
		return;   /* the same code, still in view */
	a->scan_hash = h;
	bk_import_text(text, len, &r);
	switch (r.result) {
	case BK_IMPORT_SAVED:
	case BK_IMPORT_DUPLICATE:
		/* back to the list, on the profile */
		if (a->screen == SCR_SCAN)
			back(a);
		refresh(a);
		if (a->screen == SCR_PROFILES && r.profile >= 0)
			select_profile(a, r.profile);
		toast(a, trf(buf, sizeof(buf), r.result == BK_IMPORT_SAVED ? S_TOAST_SAVED : S_TOAST_ALREADY,
			r.name, NULL));
		/* the private key now sits on the card: say so once */
		if (r.result == BK_IMPORT_SAVED && !a->key_notice_done) {
			a->dialog = DLG_KEY_NOTICE;
			a->key_notice_done = 1;
		}
		break;
	case BK_IMPORT_INVALID:
		a->scan_invalid = 1;
		snprintf(a->scan_reason, sizeof(a->scan_reason), "%s", r.reason);
		break;
	default:
		toast(a, tr(S_TOAST_SAVE_FAILED));
		a->scan_hash = 0;   /* let the next frame try again */
		break;
	}
}

void app_import_text(const char *text, size_t len)
{
	App *a = &g_app;
	a->scan_hash = 0;
	import_text(a, text, len);
}

/* The camera runs exactly while the scan screen is up */
static void camera_follow(App *a)
{
	if (a->screen == SCR_SCAN && !a->cam_on && !a->cam_tried) {
		a->cam_tried = 1;
		a->cam_error = cam_open() != 0;
		if (!a->cam_error && qr_open(640, 480) != 0) {
			cam_close();
			a->cam_error = 1;
		}
		a->cam_on = !a->cam_error;
	} else if (a->screen != SCR_SCAN && a->cam_on) {
		cam_close();
		qr_close();
		a->cam_on = 0;
	}
}

static void scan(App *a)
{
	static char payload[PF_TEXT_MAX];
	int w, h, stride;
	if (!a->cam_on)
		return;
	const uint8_t *luma = cam_frame(&w, &h, &stride);
	if (!luma)
		return;
	int n = qr_scan(luma, stride, payload, (int)sizeof(payload), NULL);
	a->scan_seen = n != 0 ? 0 : a->scan_seen + 1;
	if (n > 0) {
		import_text(a, payload, (size_t)n);
		vtc_wipe(payload, (size_t)n);
	}
}

/* ---- frame ------------------------------------------------------------- */

void app_init(uint64_t now_ms)
{
	memset(&g_app, 0, sizeof(g_app));
	g_app.now = now_ms;
	g_app.screen = SCR_HOME;
	g_app.detail_profile = -1;
	g_app.last_profile = 0;
	bk_init();
	bk_tick(now_ms);
	refresh(&g_app);
}

void app_show(Screen s, int select)
{
	App *a = &g_app;
	refresh(a);
	a->depth = 0;
	a->screen = SCR_HOME;
	a->key_notice_done = 1;
	a->toast[0] = 0;
	open_screen(a, s);
	a->dialog = DLG_NONE;
	if (s == SCR_PROFILES && select >= 0)
		a->sel = select;
	if (s == SCR_MIGRATE && select >= 0)
		a->mig_sel = select;
	if (s == SCR_DETAILS)
		a->detail_profile = select;
}

void app_show_dialog(int kind, int arg)
{
	g_app.dialog = kind;
	g_app.dialog_arg = arg;
}

void app_show_toast(const char *text)
{
	toast(&g_app, text);
}

void app_frame(uint64_t now_ms, const AppInput *in)
{
	App *a = &g_app;
	a->now = now_ms;
	/* After a pause from the app the device once froze with no line
	 * from the app; the first 60 frames log each stage, then every 30th
	 * frame for a while. */
	static unsigned tf;
	int stages = 0;
	if (a->trace_frames > 0) {
		char t[48];
		tf++;
		stages = tf <= 60;
		if (stages || tf % 30 == 0) {
			snprintf(t, sizeof(t), "frame %u after the pause", tf);
			bk_trace(t);
		}
		if (tf > 60 && tf % 30 == 0)
			a->trace_frames--;
	} else {
		tf = 0;
	}
#define STAGE(x) do { if (stages) bk_trace(x); } while (0)
	{
		extern void (*be_trace)(const char *what);
		be_trace = stages ? bk_trace : NULL;
	}
	bk_tick(now_ms);
	STAGE(" tick done");
	refresh(a);
	STAGE(" refresh done");
	if (in && (in->pressed || in->tap)) {
		handle_input(a, in);
		refresh(a);
	}
	camera_follow(a);
	if (a->screen == SCR_SCAN)
		scan(a);
	camera_follow(a);
	STAGE(" input and camera done");

	draw_begin(C_BG);
	STAGE(" draw begun");
	ui_hits_clear();
	a->nhints = 0;
	switch (a->screen) {
	case SCR_HOME:
		screen_home(a);
		break;
	case SCR_PROFILES:
		screen_profiles(a);
		break;
	case SCR_MIGRATE:
		screen_migrate(a);
		break;
	case SCR_LEAK:
		screen_leak(a);
		break;
	case SCR_DETAILS:
		screen_details(a);
		break;
	case SCR_HELP:
		screen_help(a);
		break;
	case SCR_ABOUT:
		screen_about(a);
		break;
	case SCR_SCAN:
		screen_scan(a);
		break;
	}
	if (a->dialog) {
		/* the dialog's own buttons replace the screen's */
		a->nhints = 0;
		ui_hits_clear();
		screen_dialog(a);
	}
	STAGE(" screen drawn");
	if (a->toast[0] && a->now < a->toast_until)
		ui_toast(a->toast);
	STAGE(" toast drawn");
	draw_end();
	STAGE(" frame ended");
#undef STAGE
}
