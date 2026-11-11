/*
 * main_host.c - renders screens to PNG on a desktop, with the mock back
 * end: every screen in the scenarios that matter, plus the five main
 * screens (main, profiles, migrate, leak, disconnected). Also draws the
 * LiveArea images.
 *
 *   vpnturbo-shots [--lang <code>] <font dir/> <out dir>   screenshots
 *   vpnturbo-shots <font dir/> --livearea <sce_sys>        LiveArea images
 *   vpnturbo-shots --profiles <dir> <font dir/> <out dir>  the Profiles screens
 *              from the real files under <dir> (see bk_mock_profiles_from)
 *   vpnturbo-shots --profiles <dir> --qr-test <font dir/> <png>...
 *              decode each QR image through the app's QR path, as a camera
 *              frame, and import it into <dir>; prints the outcome only
 *
 * <code> is a language's file name in src/lang/ (ja, de, zhs, ...), or en
 * or engb; the default is engb. After each screenshot it lists text that
 * runs off the screen, out of its box or into other text, and characters
 * no font could draw (CJK and Cyrillic need the fallback fonts: make
 * fallback-fonts). The exit status is 1 if anything was listed.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "backend.h"
#include "draw.h"
#include "draw_host.h"
#include "qr.h"
#include "strings.h"
#include "ui.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

/*
 * Profiles for the import screenshots. The keys are made up (random bytes),
 * the addresses are from the documentation ranges.
 */
static const char k_sample[] =
	"[Interface]\n"
	"PrivateKey = kJ3pY0vB8mQ2cX7dT5nR1wL9fH4gK6sA0zE3uI8oP2c=\n"
	"Address = 10.66.0.2/32\n"
	"DNS = 10.66.0.1\n"
	"\n"
	"[Peer]\n"
	"PublicKey = Qm7vR2xT9bN4cL1kD8fH3jS6gP0wY5zA2eU7iO4rT1E=\n"
	"Endpoint = vpn.example.net:51820\n"
	"AllowedIPs = 0.0.0.0/0\n"
	"PersistentKeepalive = 25\n";

static const char k_sample_fork[] =
	"[Interface]\n"
	"PrivateKey = kJ3pY0vB8mQ2cX7dT5nR1wL9fH4gK6sA0zE3uI8oP2c=\n"
	"Address = 10.66.0.2/32\n"
	"Jc = 4\n"
	"\n"
	"[Peer]\n"
	"PublicKey = Qm7vR2xT9bN4cL1kD8fH3jS6gP0wY5zA2eU7iO4rT1E=\n"
	"Endpoint = vpn.example.net:51820\n"
	"AllowedIPs = 0.0.0.0/0\n";

static const char *g_out;
static int g_failed;
static int g_problems;

static void save(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.png", g_out, name);
	if (host_write_png(path) != 0) {
		fprintf(stderr, "vpnturbo-shots: cannot write %s\n", path);
		g_failed = 1;
	} else {
		printf("  %s\n", path);
	}
	g_problems += host_layout_report(name);
	uint32_t last;
	int missing = draw_missing_glyphs(&last);
	if (missing) {
		printf("  ! %s: %d characters with no glyph, the last U+%04X\n", name, missing, (unsigned)last);
		g_problems += missing;
	}
}

static int scenario(const char *name)
{
	for (int i = 0; i < bk_scenario_count(); i++)
		if (!strcmp(bk_scenario_name(i), name))
			return i;
	fprintf(stderr, "vpnturbo-shots: no scenario \"%s\"\n", name);
	g_failed = 1;
	return 0;
}

/* Scenario, screen, selection, then one frame at a fixed time */
static void shot(const char *name, const char *sc, Screen s, int sel)
{
	bk_scenario_set(scenario(sc));
	app_show(s, sel);
	app_frame(1000, NULL);
	save(name);
}

static void shots(void)
{
	/* the five main screens */
	shot("main", "connected", SCR_HOME, -1);
	shot("profiles", "connected", SCR_PROFILES, 0);
	shot("migrate", "connected, Travel", SCR_MIGRATE, 0);
	shot("leak", "leak found", SCR_LEAK, -1);
	shot("disconnected", "not protected", SCR_HOME, -1);

	/* home in every scenario */
	for (int i = 0; i < bk_scenario_count(); i++) {
		char name[96];
		const char *sc = bk_scenario_name(i);
		size_t o = (size_t)snprintf(name, sizeof(name), "home-");
		for (; *sc && o + 1 < sizeof(name); sc++)
			if (*sc != ',')
				name[o++] = *sc == ' ' ? '-' : *sc;
		name[o] = 0;
		shot(name, bk_scenario_name(i), SCR_HOME, -1);
	}

	/* everything else */
	shot("profiles-many", "many profiles", SCR_PROFILES, 3);
	shot("profiles-scrolled", "many profiles", SCR_PROFILES, 8);
	shot("migrate-reconnect", "connected, Travel", SCR_MIGRATE, 1);
	shot("migrate-held", "stalled, kill switch holding", SCR_MIGRATE, 0);
	shot("leak-ok", "connected", SCR_LEAK, -1);
	shot("leak-not-connected", "idle", SCR_LEAK, -1);
	shot("details", "connected", SCR_DETAILS, -1);
	shot("details-blocked", "blocked after wake", SCR_DETAILS, -1);
	shot("details-profile", "connected", SCR_DETAILS, 2);
	shot("details-invalid", "many profiles", SCR_DETAILS, 8);
	shot("about", "connected", SCR_ABOUT, -1);
	shot("help", "connected", SCR_HELP, -1);

	bk_scenario_set(scenario("connected"));
	app_show(SCR_PROFILES, 2);
	app_show_dialog(DLG_CLASH, 2);
	app_frame(1000, NULL);
	save("dialog-clash");

	app_show(SCR_HOME, -1);
	app_show_dialog(DLG_DISCONNECT, 0 /* BTN_CROSS */);
	app_frame(1000, NULL);
	save("dialog-disconnect");

	app_show(SCR_PROFILES, 0);
	app_show_dialog(DLG_KEY_NOTICE, 0);
	app_frame(1000, NULL);
	save("dialog-key-notice");

	bk_scenario_set(scenario("connected"));
	app_show(SCR_HOME, -1);
	app_show_toast(tr(S_TOAST_PAUSED));
	app_frame(1000, NULL);
	save("toast");

	/* Lockdown: Details with the row and the dimmed pause,
	 * the confirmation, Disconnect's warning, the refused pause, Help */
	shot("details-lockdown", "connected, lockdown", SCR_DETAILS, -1);
	shot("details-lockdown-off", "lockdown, disconnected", SCR_DETAILS, -1);
	shot("help-lockdown", "lockdown, disconnected", SCR_HELP, -1);
	bk_scenario_set(scenario("connected"));
	app_show(SCR_DETAILS, -1);
	app_show_dialog(DLG_LOCKDOWN, 0);
	app_frame(1000, NULL);
	save("dialog-lockdown");
	bk_scenario_set(scenario("connected, lockdown"));
	app_show(SCR_HOME, -1);
	app_show_dialog(DLG_DISCONNECT, 0 /* BTN_CROSS */);
	app_frame(1000, NULL);
	save("dialog-disconnect-lockdown");
	/* after a wake under Lockdown: Pause is dimmed, and says why */
	bk_scenario_set(scenario("blocked after wake"));
	BkOptions opt;
	bk_get_options(&opt);
	opt.lockdown = 1;
	bk_set_options(&opt);
	app_show(SCR_HOME, -1);
	app_frame(1000, NULL);
	AppInput tri = { IN_TRIANGLE, 0, 0, 0 };
	app_frame(1000, &tri);
	save("toast-pause-lockdown");
	/* Pause from Details (the device once froze here): toast, then home */
	opt.lockdown = 0;
	bk_set_options(&opt);
	bk_scenario_set(scenario("connected"));
	app_show(SCR_DETAILS, -1);
	app_frame(1000, NULL);
	app_frame(1000, &tri);
	save("toast-paused-from-details");
	for (int i = 0; i < 120; i++)
		app_frame(16, NULL);
	save("home-after-pause-from-details");
	/* confirmed from Details, then home; no profile loaded in the module yet */
	bk_scenario_set(scenario("idle"));
	app_show(SCR_DETAILS, -1);
	app_frame(1000, NULL);
	AppInput st = { IN_START, 0, 0, 0 }, x = { IN_CROSS, 0, 0, 0 }, o = { IN_CIRCLE, 0, 0, 0 };
	app_frame(1000, &st);
	app_frame(1000, &x);
	app_frame(1000, &o);
	save("home-lockdown-waiting");   /* no profile loaded yet: not blocking */

	/* profiles: none yet, then QR import */
	shot("profiles-empty", "no profiles", SCR_PROFILES, -1);
	shot("scan", "connected", SCR_SCAN, -1);
	bk_scenario_set(scenario("connected"));
	app_show(SCR_SCAN, -1);
	app_import_text(k_sample_fork, sizeof(k_sample_fork) - 1);
	app_frame(1000, NULL);
	save("scan-invalid");
	shot("scan-no-profiles", "no profiles", SCR_SCAN, -1);

	/* from Profiles, Start opens the scan; a decoded code comes back to the list */
	AppInput start = { IN_START, 0, 0, 0 };
	bk_scenario_set(scenario("connected"));
	app_show(SCR_PROFILES, 0);
	app_frame(1000, NULL);
	app_frame(1000, &start);
	app_import_text(k_sample, sizeof(k_sample) - 1);
	app_frame(1000, NULL);
	save("profiles-saved");
	app_frame(1000, &start);
	app_import_text(k_sample, sizeof(k_sample) - 1);
	app_frame(1000, NULL);
	save("profiles-already");
}

/* The Profiles screens from real files (make shots-files) */
static void shots_files(void)
{
	shot("files-profiles", "idle", SCR_PROFILES, 0);
	int n = bk_profile_count(), invalid = -1;
	for (int i = 0; i < n && invalid < 0; i++) {
		BkProfile p;
		if (bk_profile_get(i, &p) == 0 && (p.warnings & BK_WARN_INVALID))
			invalid = i;
	}
	shot("files-profiles-end", "idle", SCR_PROFILES, n > 0 ? n - 1 : 0);
	if (invalid >= 0)
		shot("files-details-invalid", "idle", SCR_DETAILS, invalid);
	shot("files-profiles-clash", "idle", SCR_PROFILES, 2);

	AppInput start = { IN_START, 0, 0, 0 };
	bk_scenario_set(scenario("idle"));
	app_show(SCR_PROFILES, 0);
	app_frame(1000, NULL);
	app_frame(1000, &start);
	app_import_text(k_sample, sizeof(k_sample) - 1);
	app_frame(1000, NULL);
	save("files-import-saved");
	app_frame(1000, &start);
	app_import_text(k_sample, sizeof(k_sample) - 1);
	app_frame(1000, NULL);
	save("files-import-already");
}

/* ---- QR path test: images as camera frames ----------------------------- */

#define FRAME_W 640
#define FRAME_H 480

static int qr_test(int n, char **files)
{
	static uint8_t frame[FRAME_W * FRAME_H];
	static char payload[16384];
	int bad = 0;
	if (qr_open(FRAME_W, FRAME_H) != 0)
		return 1;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n; i++) {
			int w, h, c;
			uint8_t *img = stbi_load(files[i], &w, &h, &c, 1);
			if (!img) {
				printf("%s: cannot read\n", files[i]);
				bad = 1;
				continue;
			}
			/* the code filling 85% of the frame's height, as the guide asks */
			float scale = FRAME_H * 0.85f / (float)(w > h ? w : h);
			int ox = (int)(FRAME_W - w * scale) / 2, oy = (int)(FRAME_H - h * scale) / 2;
			memset(frame, 0xE0, sizeof(frame));
			for (int y = 0; y < (int)(h * scale); y++)
				for (int x = 0; x < (int)(w * scale); x++)
					frame[(oy + y) * FRAME_W + ox + x] = img[(int)(y / scale) * w + (int)(x / scale)];
			stbi_image_free(img);
			int len = qr_scan(frame, FRAME_W, payload, (int)sizeof(payload), NULL);
			const char *base = strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i];
			if (len <= 0) {
				/* a measurement, not a failure: dense codes need a sharp image */
				printf("%-24s no decode\n", base);
				continue;
			}
			BkImport r;
			bk_import_text(payload, (size_t)len, &r);
			memset(payload, 0, sizeof(payload));
			static const char *const what[] = { "saved as", "already saved as", "not a profile:", "failed" };
			printf("%-24s %4d bytes  %s %s\n", base, len, what[r.result],
				r.result == BK_IMPORT_INVALID ? r.reason : r.name);
		}
	qr_close();
	return bad;
}

/* ---- LiveArea images: 8-bit palette PNGs ------------------------------- */

static Colour mix(Colour a, Colour b, float t)
{
	unsigned r = (unsigned)((a >> 16 & 255) * (1 - t) + (b >> 16 & 255) * t + 0.5f);
	unsigned g = (unsigned)((a >> 8 & 255) * (1 - t) + (b >> 8 & 255) * t + 0.5f);
	unsigned bl = (unsigned)((a & 255) * (1 - t) + (b & 255) * t + 0.5f);
	return RGB(r, g, bl);
}

/*
 * A vertical wash from top to bottom over rows y0..y1, in 32 steps with a
 * 4x4 ordered dither between neighbouring steps, so it survives the
 * reduction to a 256-colour palette without visible bands.
 */
static void wash(int w, int y0, int y1, Colour top, Colour bottom)
{
	static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
	const int steps = 32;
	for (int y = y0; y < y1; y++) {
		float t = (float)(y - y0) / (float)(y1 - y0);
		float f = t * t * (steps - 1);
		int k = (int)f;
		float frac = f - (float)k;
		Colour a = mix(top, bottom, (float)k / (steps - 1));
		Colour b = mix(top, bottom, (float)(k + 1 < steps ? k + 1 : k) / (steps - 1));
		for (int x = 0; x < w; x++)
			draw_rect((float)x, (float)y, 1, 1, frac * 16 > bayer[y & 3][x & 3] + 0.5f ? b : a);
	}
}

static void livearea(const char *dir)
{
	char path[512];
	TextStyle name = ST_OPT_NAME;
	/* Branding (ui.h BRAND_LEVEL): red ground, white mark; otherwise the
	 * mark in lime on black, as mocked up */
	int brand = BRAND_LEVEL >= 1;
	Colour mark = brand ? C_TEXT : C_LIME;
	Colour deep = RGB(0x6e, 0x0a, 0x14);

	/* icon0.png, 128x128: the mark */
	host_target(128, 128);
	draw_begin(C_BG);
	if (brand)
		wash(128, 0, 128, C_BRAND, deep);
	draw_icon(ICON_MARK, 20, 20, 88, 88, mark);
	draw_end();
	snprintf(path, sizeof(path), "%s/icon0.png", dir);
	g_failed |= host_write_png8(path) != 0;

	/* livearea/contents/startup.png, 280x158: mark and name on the dark
	 * ground, so the logo sits on the background with no panel behind it;
	 * the mark carries the red */
	host_target(280, 158);
	draw_begin(C_BG);
	draw_icon(ICON_MARK, 24, 55, 48, 48, brand ? C_BRAND : C_LIME);
	name.size = 30;
	while (name.size > 16 && text_width(&name, tr(S_APP_DISPLAY_NAME)) > 280 - 88 - 14)
		name.size -= 1;
	draw_text(&name, 88, 79 - text_height(&name) / 2, tr(S_APP_DISPLAY_NAME));
	draw_end();
	snprintf(path, sizeof(path), "%s/livearea/contents/startup.png", dir);
	g_failed |= host_write_png8(path) != 0;

	/* livearea/contents/bg.png, 840x500: the pipe, lit */
	host_target(840, 500);
	draw_begin(C_BG);
	/* Dark where the start gate sits, so the gate (always an opaque
	 * rectangle) blends into the ground; the red is carried by the pipe. */
	if (brand)
		ui_pipe_accent(C_BRAND, C_TEXT);
	PipeItem pipe[5];
	memset(pipe, 0, sizeof(pipe));
	pipe[0] = (PipeItem){ .kind = PIPE_NODE, .width = 150, .disc = DISC_ON, .icon = ICON_VITA,
		.icon_colour = C_TEXT, .label = tr(S_NODE_VITA), .tube_icon = -1 };
	pipe[1] = (PipeItem){ .kind = PIPE_LINE, .width = 60, .icon = -1, .tube_icon = -1 };
	pipe[2] = (PipeItem){ .kind = PIPE_TUBE, .tube = TUBE_SOLID, .tube_text = tr(S_TUBE_ENCRYPTED),
		.tube_icon = ICON_LOCK, .icon = -1 };
	pipe[3] = (PipeItem){ .kind = PIPE_LINE, .width = 60, .icon = -1, .tube_icon = -1 };
	pipe[4] = (PipeItem){ .kind = PIPE_NODE, .width = 150, .disc = DISC_ON, .icon = ICON_GLOBE,
		.icon_colour = C_TEXT, .label = tr(S_NODE_INTERNET), .tube_icon = -1 };
	ui_pipe(pipe, 5, 40, 380, 760, &PIPE_LARGE, NULL);
	ui_pipe_accent(0, 0);
	draw_end();
	snprintf(path, sizeof(path), "%s/livearea/contents/bg.png", dir);
	g_failed |= host_write_png8(path) != 0;
}

int main(int argc, char **argv)
{
	int lang = LANG_ENGLISH_GB;
	const char *profiles = NULL;
	int qr = 0;
	while (argc > 1 && !strncmp(argv[1], "--", 2) && strcmp(argv[1], "--livearea")) {
		if (!strcmp(argv[1], "--lang") && argc > 2) {
			lang = str_lang_from_code(argv[2]);
			if (lang < 0) {
				fprintf(stderr, "vpnturbo-shots: no language \"%s\"\n", argv[2]);
				return 2;
			}
			argc -= 2;
			argv += 2;
		} else if (!strcmp(argv[1], "--profiles") && argc > 2) {
			profiles = argv[2];
			argc -= 2;
			argv += 2;
		} else if (!strcmp(argv[1], "--qr-test")) {
			qr = 1;
			argc -= 1;
			argv += 1;
		} else {
			break;
		}
	}
	if (argc < 3) {
		fprintf(stderr, "usage: vpnturbo-shots [--lang <code>] [--profiles <dir>] <font dir/> <out dir>\n"
			"       vpnturbo-shots <font dir/> --livearea <sce_sys dir>\n"
			"       vpnturbo-shots --profiles <dir> --qr-test <font dir/> <png>...\n");
		return 2;
	}
	str_set_language(lang);
	draw_set_language(lang);
	if (draw_init(argv[1]) != 0) {
		fprintf(stderr, "vpnturbo-shots: cannot load fonts from %s\n", argv[1]);
		return 1;
	}
	if (profiles)
		bk_mock_profiles_from(profiles);
	if (qr) {
		host_target(SCREEN_W, SCREEN_H);
		app_init(1000);
		g_failed = qr_test(argc - 2, argv + 2);
	} else if (!strcmp(argv[2], "--livearea") && argc > 3) {
		livearea(argv[3]);
	} else {
		g_out = argv[2];
		host_target(SCREEN_W, SCREEN_H);
		app_init(1000);
		if (profiles)
			shots_files();
		else
			shots();
		if (g_problems)
			printf("vpnturbo-shots: %d layout or glyph problems (%s)\n", g_problems, str_lang_code(lang));
	}
	draw_shutdown();
	return g_failed || g_problems;
}
