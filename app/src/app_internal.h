/*
 * app_internal.h - state shared by app.c (navigation, actions, input) and
 * screens.c (drawing).
 */
#ifndef APP_INTERNAL_H
#define APP_INTERNAL_H

#include <stddef.h>

#include "app.h"
#include "backend.h"
#include "ui.h"

/* What a button or a touch target does */
enum {
	A_NONE,
	A_CONNECT,          /* arg: profile, -1 = last used */
	A_CONNECT_SEL,      /* the selected profile on Profiles or Details */
	A_CONNECT_ANYWAY,   /* past the subnet clash warning */
	A_DISCONNECT,
	A_DISCONNECT_ASK,   /* arg: the button pressed; confirm with it again */
	A_RETRY,
	A_KILL_SWITCH,      /* toggle */
	A_ALLOW_LAN,        /* toggle */
	A_LOCKDOWN,         /* toggle; turning it on asks first (DLG_LOCKDOWN) */
	A_LOCKDOWN_ON,      /* confirmed in the dialog */
	A_PAUSE,
	A_RESUME,
	A_OPEN,             /* arg: Screen */
	A_BACK,
	A_PROFILE_DETAILS,  /* Details for the selected profile */
	A_SELECT,           /* arg: list position; a second tap activates */
	A_MIGRATE_SEL,
	A_LEAK_RUN,
	A_NET_SETTINGS,
	A_RESCAN,
	A_DIALOG_OK,
	A_DIALOG_CANCEL,
	A_CHECK_AGAIN,
	A_GUARD_RESET,          /* clear the plugin's boot guard, restart */
	A_SCAN_QR,          /* open the QR import screen */
};

/* How the home screen presents the state */
typedef enum HomeView {
	HV_PROTECTED,
	HV_CONNECTING,
	HV_UNPROTECTED,     /* down, kill switch off: traffic goes direct */
	HV_HELD,            /* down, kill switch on: traffic waits */
	HV_BLOCKED,         /* resume lockdown */
	HV_PAUSED,
	HV_LOCKDOWN,        /* disconnected under Lockdown: everything blocked */
	HV_IDLE,
	HV_MISSING,         /* kernel module not loaded or out of date, or the
	                     * SceShell plugin not loaded */
} HomeView;

/* One line of the Profiles list */
typedef struct ListItem {
	int header;         /* a section heading, not a profile */
	const char *title;  /* heading text */
	int profile;
} ListItem;

#define MAX_HINTS 6
#define MAX_ITEMS (BK_MAX_PROFILES * 2 + 8)

typedef struct App {
	Screen screen;
	Screen stack[8];
	int depth;

	uint64_t now;
	int trace_frames;     /* frames still to log after a pause */
	BkStatus st;
	BkComponents comp;
	BkCheck chk;
	BkProfile prof[BK_MAX_PROFILES];
	int nprof;
	int last_profile;

	/* Profiles */
	ListItem items[MAX_ITEMS];
	int nitems;
	int sel;            /* selected profile row, counting rows only */
	float scroll;
	int key_notice_done;

	/* Migrate */
	int mig[BK_MAX_PROFILES];
	int nmig;
	int mig_sel;
	float mig_scroll;

	/* Details: -1 = the active profile */
	int detail_profile;

	/* QR import */
	int cam_on;
	int cam_tried;      /* opened (or tried) since the scan screen came up */
	int cam_error;      /* the camera did not start */
	uint32_t scan_hash; /* the last code handled, so each is handled once */
	int scan_seen;      /* frames since a code was last seen, 0 = this one */
	int scan_invalid;   /* the last code was not a usable profile */
	char scan_reason[160];

	int dialog, dialog_arg;
	char toast[96];
	uint64_t toast_until;

	/* this frame's footer, for the buttons */
	Hint hints[MAX_HINTS];
	int nhints;
	char hint_buf[MAX_HINTS][64];
} App;

extern App g_app;

HomeView home_view(const App *a);
void build_profile_list(App *a);
void build_migrate_list(App *a);
int selected_profile(const App *a);

/* Formatting */
const char *fmt_ip(uint32_t ip, char *out, size_t n);
const char *fmt_ip_port(uint32_t ip, uint16_t port, char *out, size_t n);
const char *fmt_clock(uint32_t s, char *out, size_t n);     /* H:MM:SS or M:SS */
const char *fmt_hms(uint32_t s, char *out, size_t n);       /* HH:MM:SS */
const char *fmt_bytes(uint64_t b, char *out, size_t n);
const char *profile_display(const BkProfile *p);
const char *profile_routes(const BkProfile *p, char *out, size_t n);
const char *profile_exit_label(const BkProfile *p, char *out, size_t n);
const char *error_cause(BkError e);

/* Screens: each draws itself and fills a->hints */
void screen_home(App *a);
void screen_profiles(App *a);
void screen_migrate(App *a);
void screen_leak(App *a);
void screen_details(App *a);
void screen_about(App *a);
void screen_help(App *a);
void screen_dialog(App *a);
void screen_scan(App *a);

/* Add a footer hint; label is copied */
void hint(App *a, int button, const char *label, int action, int arg, int right);
/* Draw the last hint added dimmed (still pressable: it explains itself) */
void hint_dim(App *a);
/* The kill switch is on, or implied by Lockdown */
int ks_effective(const BkStatus *st);

#endif
