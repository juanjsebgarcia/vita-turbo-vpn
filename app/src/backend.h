/*
 * backend.h - everything the screens need from the rest of the system:
 * tunnel status, profiles, actions, options and check results.
 *
 * Two implementations: backend_mock.c (scripted scenarios, for screenshots
 * and for running the app without the kernel module) and backend_vt.c (the
 * kernel module's control API). The screens only ever see
 * this header.
 *
 * The state and option types mirror the module's; backend_vt.c maps the
 * kernel module's own (include/vpnturbo.h) onto them, so the screens and the
 * host build do not depend on that header.
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* ---- lifecycle; values as VtState will define them ------------ */

typedef enum BkState {
	BK_IDLE,          /* no profile active */
	BK_HANDSHAKING,   /* sending initiations */
	BK_UP,            /* valid session, handshake within 180 s */
	BK_STALLED,       /* no handshake for 180 s, still retrying */
	BK_ERROR,         /* unrecoverable: bad profile, socket failure */
	BK_PAUSED,        /* user paused; resumes by itself */
} BkState;

/* Why the tunnel is not working, worded by the app */
typedef enum BkError {
	BK_ERR_NONE,
	BK_ERR_NO_ANSWER,       /* wrong key, server down, or the port is blocked */
	BK_ERR_RESOLVE,         /* endpoint hostname did not resolve */
	BK_ERR_WIFI_OFF,
	BK_ERR_PROFILE,         /* profile invalid; detail names the key */
	BK_ERR_SOCKET,
	BK_ERR_CLOCK,           /* date reset: HTTPS checks cannot work */
} BkError;

typedef struct BkStatus {
	BkState state;
	int locked;               /* resume lockdown: Blocked, reconnecting */
	uint32_t lock_drops;      /* frames dropped while locked */
	int profile;              /* index into the profile list, -1 = none */

	uint32_t handshake_age_s; /* BK_NEVER if no handshake yet */
	uint32_t latency_ms;      /* tunnel round trip, BK_NEVER = unknown */
	uint32_t up_s;            /* time since the tunnel came up */
	uint32_t state_s;         /* time in the current state */
	uint32_t retry_in_s;      /* next handshake attempt, when not UP */
	uint32_t attempts;        /* handshake attempts since the last success */
	uint32_t pause_left_s;    /* BK_PAUSED: time until it resumes */

	uint64_t rx_bytes, tx_bytes;
	uint32_t rx_packets, tx_packets;

	uint32_t endpoint_ip;     /* host order */
	uint16_t endpoint_port;
	uint16_t mtu;
	uint32_t tunnel_ip;
	uint8_t tunnel_prefix;
	uint32_t wlan_ip;         /* 0 = Wi-Fi not connected */
	uint8_t wlan_prefix;
	uint32_t wlan_gateway;

	int kill_switch;
	int allow_lan;
	/* Lockdown mode: only tunnel traffic may leave */
	int lockdown_avail;       /* the kernel module understands it (VT_FEAT_LOCKDOWN) */
	int lockdown;             /* the setting */
	int lockdown_block;       /* disconnected under Lockdown and the block is in
	                           * effect: everything but the tunnel's own path,
	                           * DHCP, ARP (and the LAN with allow_lan) is dropped */
	int lockdown_ep_unresolved; /* Lockdown on and the server's name never looked up */
	int lockdown_wait;        /* disconnected under Lockdown, block not in place
	                           * yet (the interface waits for its address, or no
	                           * profile is loaded) */
	BkError last_error;
	char last_error_detail[64];
} BkStatus;

#define BK_NEVER 0xFFFFFFFFu

/* ---- installation ------------------------------------------- */

typedef struct BkComponents {
	int module_loaded;        /* vpnturbo_kernel.skprx */
	int module_api_ok;        /* its API version matches the app's */
	int shell_loaded;         /* vpnturbo.suprx (SceShell: transport, quick
	                           * menu); 0 only when known to be absent */
	int shell_api_ok;
	int shell_guard;          /* not running because its boot guard stood it
	                           * down after two starts that did not finish */
	int clock_ok;             /* the date looks set */
} BkComponents;

/* ---- profiles ------------------------------------------------- */

#define BK_MAX_ALLOWED 32       /* as the parser and the kernel take */
#define BK_MAX_PROFILES 256     /* a provider's zip of servers fits */

enum {
	BK_WARN_CLASH    = 1 << 0,  /* AllowedIPs overlap the Wi-Fi subnet */
	BK_WARN_IGNORED  = 1 << 1,  /* lines ignored (PostUp, IPv6, ...) */
	BK_WARN_INVALID  = 1 << 2,  /* cannot be used; invalid_reason says why */
};

/* Exit-IP check: the generic service, or the profile's own
 * https:// URL (its check directive). There are no provider presets. */
typedef enum BkCheckPreset {
	BK_CHECK_GENERIC,
	BK_CHECK_URL,
} BkCheckPreset;

typedef struct BkProfile {
	char name[64];            /* file name without .conf */
	char nickname[33];        /* "" = none */
	char group[48];           /* sub-folder or zip name; "" = top level */
	char storage[8];          /* "ux0", "uma0", "imc0", "ur0" */
	char path[256];            /* as the console names it, e.g. ux0:data/.../home.conf */
	int in_zip;
	int recent;               /* 1-5 = position in Recent, 0 = not recent */

	uint32_t address;         /* tunnel address */
	uint8_t address_prefix;
	uint8_t allowed_count;
	struct { uint32_t net; uint8_t prefix; } allowed[BK_MAX_ALLOWED];
	int full_tunnel;          /* AllowedIPs cover 0.0.0.0/0 */
	char endpoint_host[256];  /* as written: hostname or address */
	uint16_t endpoint_port;
	uint32_t dns[2];          /* 0 = none */
	uint16_t mtu;
	uint16_t keepalive_s;
	BkCheckPreset check;
	char check_url[160];      /* BK_CHECK_URL: the https:// URL */

	uint32_t key_id;          /* a 32-bit hash of the private key, to tell
	                           * whether two profiles share it; 0 = invalid */

	uint32_t warnings;        /* BK_WARN_* */
	int clash_prefix;         /* BK_WARN_CLASH: the AllowedIPs entry that clashes */
	int ignored_lines;        /* the parser's notes: lines or entries not used */
	char invalid_reason[160]; /* the parser's message, e.g. "Line 7: `Jc': ..." */
} BkProfile;

int bk_profile_count(void);
int bk_profile_get(int index, BkProfile *out);
/* Read the profile folders again */
void bk_rescan(void);
/* Where profiles were found: a set of BK_STORE_* */
enum { BK_STORE_UX0 = 1, BK_STORE_UMA0 = 2, BK_STORE_IMC0 = 4, BK_STORE_UR0 = 8 };
unsigned bk_profiles_storages(void);
/* The folder new profiles go in, to tell the user */
const char *bk_profiles_folder(void);
int bk_profile_set_nickname(int index, const char *nickname);
int bk_profile_rename(int index, const char *new_name);
int bk_profile_delete(int index);
int bk_profile_set_check(int index, BkCheckPreset preset);

/*
 * QR import: text is a decoded code. If it parses as a
 * profile and no listed profile has the same text (line endings aside), it
 * is written to the profiles folder on ux0 under a name taken from the
 * endpoint, and the list is read again. The text is never kept or shown.
 */
typedef enum BkImportResult {
	BK_IMPORT_SAVED,          /* written; name and profile say where */
	BK_IMPORT_DUPLICATE,      /* already listed as name (profile) */
	BK_IMPORT_INVALID,        /* not a usable profile; reason says why */
	BK_IMPORT_FAILED,         /* could not be written */
} BkImportResult;

typedef struct BkImport {
	BkImportResult result;
	char name[96];            /* the profile as the list shows it */
	int profile;              /* its index after the rescan, -1 = unknown */
	char reason[160];         /* BK_IMPORT_INVALID: the parser's message */
} BkImport;

int bk_import_text(const char *text, size_t len, BkImport *out);

/* ---- actions ----------------------------------------------------------- */

typedef enum BkMigrateKind {
	BK_MIGRATE_SEAMLESS,      /* same key and address: only the peer changes */
	BK_MIGRATE_RECONNECT,
} BkMigrateKind;

int bk_connect(int profile);
int bk_disconnect(void);
/* Try a handshake now instead of waiting for the next retry */
int bk_retry(void);
BkMigrateKind bk_migrate_kind(int profile);
int bk_migrate(int profile);
/* 0, -1 on failure, BK_PAUSE_LOCKDOWN if refused because Lockdown is on */
#define BK_PAUSE_LOCKDOWN (-2)
int bk_pause(uint32_t seconds);
/* One line in the app log (the device back end; the mock drops it). */
void bk_trace(const char *what);
int bk_resume(void);

typedef struct BkOptions {
	int kill_switch;          /* off by default */
	int allow_lan;            /* on by default */
	int lockdown;             /* only sent when the module has the feature */
} BkOptions;

void bk_get_options(BkOptions *o);
int bk_set_options(const BkOptions *o);

/* ---- checks --------------------------------------------------- */

typedef enum BkCheckState {
	BK_CHECK_NOT_RUN,
	BK_CHECK_RUNNING,
	BK_CHECK_DONE,
	BK_CHECK_FAILED,          /* fail_reason says why */
} BkCheckState;

typedef enum BkDnsVerdict {
	BK_DNS_OK,                /* every server goes through the tunnel */
	BK_DNS_LEAK,              /* full tunnel, a server goes over Wi-Fi */
	BK_DNS_SPLIT,             /* split tunnel: Wi-Fi DNS is intended */
	BK_DNS_UNMANAGED,         /* the profile has no DNS = */
} BkDnsVerdict;

typedef enum BkResolverKind {
	BK_RES_VPN,               /* the profile's own resolver */
	BK_RES_ISP,
	BK_RES_OTHER,
} BkResolverKind;

typedef struct BkCheck {
	BkCheckState state;
	BkError fail_reason;

	/* exit IP */
	uint32_t exit_ip;
	uint32_t isp_ip;          /* last address seen with the tunnel down, 0 = unknown */
	char city[48];            /* where the service places exit_ip; "" = not given */
	char country[48];
	char org[64];             /* the network that holds exit_ip; "" = not given */

	/* local DNS route check */
	BkDnsVerdict dns;
	int local_count;
	struct { uint32_t ip; int via_tunnel; } local[4];

	/* remote resolver check, optional */
	int remote_done;
	int remote_count;
	struct { uint32_t ip; BkResolverKind kind; } remote[4];
} BkCheck;

void bk_check_start(void);
void bk_check_get(BkCheck *out);

/* ---- lifetime ---------------------------------------------------------- */

int bk_init(void);
void bk_shutdown(void);
/* Called once per frame with a monotonic clock */
void bk_tick(uint64_t now_ms);
void bk_get_status(BkStatus *s);
void bk_get_components(BkComponents *c);
/* Open the system's network settings (Leak check's fix) */
int bk_open_network_settings(void);

/* Clear the plugin's boot guard and restart the console (shell_guard) */
void bk_guard_reset(void);

/*
 * Scripted scenarios: only the mock has them (scenario count 0 elsewhere).
 * Select cycles through them at run time.
 */
int bk_scenario_count(void);
int bk_scenario_get(void);
void bk_scenario_set(int index);
const char *bk_scenario_name(int index);

/*
 * Mock only: list the real .conf and .zip files under a folder on this
 * computer instead of the scripted profiles. The folder stands for the
 * console's profiles folders: its ux0/, uma0/, imc0/ and ur0/
 * sub-folders if it has any, else it is ux0's. 0 on success.
 */
int bk_mock_profiles_from(const char *host_dir);

#endif
