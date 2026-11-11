/*
 * app.h - the app, independent of platform: the platform feeds it input
 * and a clock once per frame, and it draws through draw.h.
 */
#ifndef APP_H
#define APP_H

#include <stddef.h>
#include <stdint.h>

/* Buttons, as edges (pressed this frame) */
enum {
	IN_CROSS    = 1 << 0,
	IN_CIRCLE   = 1 << 1,
	IN_TRIANGLE = 1 << 2,
	IN_SQUARE   = 1 << 3,
	IN_UP       = 1 << 4,
	IN_DOWN     = 1 << 5,
	IN_LEFT     = 1 << 6,
	IN_RIGHT    = 1 << 7,
	IN_L        = 1 << 8,
	IN_R        = 1 << 9,
	IN_START    = 1 << 10,
	IN_SELECT   = 1 << 11,
};

typedef struct AppInput {
	uint32_t pressed;
	int tap;            /* a touch was released this frame at tx, ty */
	float tx, ty;
} AppInput;

typedef enum Screen {
	SCR_HOME,
	SCR_PROFILES,
	SCR_MIGRATE,
	SCR_LEAK,
	SCR_DETAILS,
	SCR_ABOUT,
	SCR_HELP,           /* the tunnel without the app, Settings, quick menu */
	SCR_SCAN,           /* QR import: camera and framing guide */
} Screen;

void app_init(uint64_t now_ms);
/* Handle input, advance, and draw one frame */
void app_frame(uint64_t now_ms, const AppInput *in);

/* For screenshots: go straight to a screen, with a list selection */
void app_show(Screen s, int select);
/* For screenshots: show a dialog or a toast on top */
enum { DLG_NONE, DLG_CLASH, DLG_KEY_NOTICE, DLG_DISCONNECT, DLG_LOCKDOWN };
void app_show_dialog(int kind, int arg);
void app_show_toast(const char *text);
/* Handle text as if the camera had just decoded it (screenshots, tests) */
void app_import_text(const char *text, size_t len);

#endif
