/*
 * backend_mock.c - a scripted stand-in for the kernel module.
 *
 * Each scenario sets up a status, a profile list and check results, then
 * the clock moves them on: handshake ages, traffic, retry and pause timers.
 * Actions behave plausibly (connect handshakes for two seconds, then is up;
 * a scenario that is stuck keeps retrying). Addresses are from the
 * documentation ranges (RFC 5737) and private ranges.
 *
 * Given a folder on the computer (bk_mock_profiles_from), the profiles are
 * the real files in it instead, read by the same code as on the console
 * (profiles.c); the scenarios then play on that list.
 *
 * Never in a release: the release ships with an empty profile list.
 */
#ifdef VPNTURBO_RELEASE
#error "The mock back end cannot be built into a release (use VPNTURBO_BACKEND=vt)"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "backend.h"
#include "profiles.h"
#include "vtconf.h"

#define IP(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct MockProfile {
	BkProfile p;
	int key;                  /* stands for the private key: same key, same number */
} MockProfile;

/* ---- profile sets ------------------------------------------------------ */

static void mp(MockProfile *m, const char *name, const char *group, const char *storage,
	int key, uint32_t addr, const char *host, uint16_t port, uint32_t dns)
{
	memset(m, 0, sizeof(*m));
	BkProfile *p = &m->p;
	snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(p->group, sizeof(p->group), "%s", group);
	snprintf(p->storage, sizeof(p->storage), "%s", storage);
	if (group[0])
		snprintf(p->path, sizeof(p->path), "%s:" VT_PROFILE_DIR "/profiles/%s/%s.conf",
			storage, group, name);
	else
		snprintf(p->path, sizeof(p->path), "%s:" VT_PROFILE_DIR "/profiles/%s.conf",
			storage, name);
	p->in_zip = strstr(group, ".zip") != NULL;
	p->address = addr;
	p->address_prefix = 32;
	p->allowed_count = 1;
	p->allowed[0].net = 0;
	p->allowed[0].prefix = 0;
	p->full_tunnel = 1;
	snprintf(p->endpoint_host, sizeof(p->endpoint_host), "%s", host);
	p->endpoint_port = port;
	p->dns[0] = dns;
	p->mtu = 1420;
	p->keepalive_s = 25;
	m->key = key;
}

static void split(MockProfile *m, uint32_t net, uint8_t prefix)
{
	m->p.full_tunnel = 0;
	m->p.allowed[0].net = net;
	m->p.allowed[0].prefix = prefix;
}

static MockProfile g_prof[BK_MAX_PROFILES];
static int g_nprof;
static unsigned g_storages;

/* The five sample profiles */
static void set_basic(void)
{
	MockProfile *m = g_prof;
	mp(m++, "Home server", "", "ux0", 1, IP(10, 8, 0, 2), "home.example.org", 51820, IP(10, 8, 0, 1));
	mp(m++, "Travel", "", "ux0", 2, IP(10, 64, 12, 34), "192.0.2.44", 51820, IP(10, 64, 0, 1));
	mp(m, "Home LAN only", "", "ux0", 3, IP(10, 8, 0, 3), "home.example.org", 51820, 0);
	split(m, IP(192, 168, 1, 0), 24);
	(m++)->p.warnings = BK_WARN_CLASH;
	mp(m, "Cloud VPS", "", "ux0", 4, IP(10, 9, 0, 2), "198.51.100.20", 51820, IP(10, 9, 0, 1));
	m->p.warnings = BK_WARN_IGNORED;
	(m++)->p.ignored_lines = 1;
	mp(m, "Backup", "", "ux0", 2, IP(10, 64, 12, 34), "192.0.2.88", 51820, IP(10, 64, 0, 1));
	m->p.check = BK_CHECK_URL;
	snprintf(m->p.check_url, sizeof(m->p.check_url), "https://ip.example.org/json");
	m++;
	g_nprof = (int)(m - g_prof);
	g_storages = BK_STORE_UX0;
}

/* A longer list: Recent, sub-folders, a zip, nicknames, a duplicate, an invalid file */
static void set_full(void)
{
	MockProfile *m = g_prof;
	mp(m, "Home server", "", "ux0", 1, IP(10, 8, 0, 2), "home.example.org", 51820, IP(10, 8, 0, 1));
	(m++)->p.recent = 1;
	mp(m, "Cloud VPS", "", "ux0", 4, IP(10, 9, 0, 2), "198.51.100.20", 51820, IP(10, 9, 0, 1));
	m->p.warnings = BK_WARN_IGNORED;
	m->p.ignored_lines = 2;
	(m++)->p.recent = 3;
	mp(m, "Cloud VPS", "", "uma0", 4, IP(10, 9, 0, 2), "198.51.100.20", 51820, IP(10, 9, 0, 1));
	m->p.warnings = BK_WARN_IGNORED;
	(m++)->p.ignored_lines = 2;
	mp(m, "Home LAN only", "home", "ux0", 3, IP(10, 8, 0, 3), "home.example.org", 51820, 0);
	split(m, IP(192, 168, 1, 0), 24);
	(m++)->p.warnings = BK_WARN_CLASH;
	mp(m, "Media server", "home", "ux0", 5, IP(10, 8, 0, 4), "home.example.org", 51821, 0);
	split(m++, IP(10, 20, 0, 0), 16);
	mp(m, "exit-1", "travel.zip", "ux0", 2, IP(10, 64, 12, 34), "192.0.2.44", 51820, IP(10, 64, 0, 1));
	snprintf(m->p.nickname, sizeof(m->p.nickname), "Travel");
	(m++)->p.recent = 2;
	mp(m, "exit-2", "travel.zip", "ux0", 2, IP(10, 64, 12, 34), "192.0.2.88", 51820, IP(10, 64, 0, 1));
	snprintf(m->p.nickname, sizeof(m->p.nickname), "Backup");
	m++;
	mp(m++, "exit-3", "travel.zip", "ux0", 2, IP(10, 64, 12, 34), "192.0.2.120", 51820, IP(10, 64, 0, 1));
	mp(m, "friend", "", "ux0", 6, IP(10, 7, 0, 9), "vpn.example.net", 51820, IP(10, 7, 0, 1));
	m->p.warnings = BK_WARN_INVALID;
	snprintf(m->p.invalid_reason, sizeof(m->p.invalid_reason), "%s",
		"Line 4: `Jc': key from a non-standard protocol variant; a standard server would not understand this profile");
	m++;
	g_nprof = (int)(m - g_prof);
	g_storages = BK_STORE_UX0 | BK_STORE_UMA0;
}

static BkStatus g_st;

/* The real files of a folder on this computer (bk_mock_profiles_from) */
static int g_real;

static void set_real(void)
{
	g_nprof = pf_count();
	for (int i = 0; i < g_nprof; i++) {
		memset(&g_prof[i], 0, sizeof(g_prof[i]));
		g_prof[i].p = *pf_get(i);
		g_prof[i].key = 1000 + i;  /* the mock knows no keys: every move reconnects */
	}
	g_storages = pf_storages();
}

/* Scripted imports: hashes of what was imported, for the duplicate check */
static uint64_t g_imported[BK_MAX_PROFILES];
static int g_nimported;

static uint64_t text_hash(const char *s, size_t n)
{
	char buf[PF_TEXT_MAX];
	if (n > sizeof(buf))
		n = sizeof(buf);
	n = vtc_normalise(s, n, buf);
	uint64_t h = 14695981039346656037ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ (unsigned char)buf[i]) * 1099511628211ull;
	vtc_wipe(buf, sizeof(buf));
	return h;
}

static int mock_name_taken(void *ctx, const char *file)
{
	(void)ctx;
	for (int i = 0; i < g_nprof; i++) {
		char f[sizeof(g_prof[0].p.name) + 8];
		snprintf(f, sizeof(f), "%.*s.conf", (int)sizeof(g_prof[0].p.name) - 1, g_prof[i].p.name);
		if (!g_prof[i].p.group[0] && !strcmp(f, file))
			return 1;
	}
	return 0;
}

int bk_mock_profiles_from(const char *dir)
{
	static char dirs[PF_NSTORES][512];
	const char *use[PF_NSTORES] = { NULL };
	int any = 0;
	for (int s = 0; s < PF_NSTORES; s++) {
		struct stat st;
		snprintf(dirs[s], sizeof(dirs[s]), "%s/%s", dir, PF_STORE_NAME[s]);
		if (stat(dirs[s], &st) == 0 && S_ISDIR(st.st_mode)) {
			use[s] = dirs[s];
			any = 1;
		}
	}
	if (!any)
		snprintf(dirs[PF_UX0], sizeof(dirs[PF_UX0]), "%s", dir);
	use[PF_UX0] = dirs[PF_UX0];
	pf_set_dirs(use);
	g_real = 1;
	return 0;
}

int bk_import_text(const char *text, size_t len, BkImport *out)
{
	if (g_real) {
		int r = pf_import(text, len, out);
		set_real();
		return r;
	}
	static BkProfile parsed;
	memset(out, 0, sizeof(*out));
	out->profile = -1;
	memset(&parsed, 0, sizeof(parsed));
	if (pf_fill(text, len, g_st.wlan_ip, g_st.wlan_prefix, &parsed) != 0) {
		out->result = BK_IMPORT_INVALID;
		snprintf(out->reason, sizeof(out->reason), "%s", parsed.invalid_reason);
		return -1;
	}
	uint64_t h = text_hash(text, len);
	for (int i = 0; i < g_nimported; i++)
		if (g_imported[i] == h) {
			/* imported earlier this session: the copy is in the list under its name */
			for (int k = 0; k < g_nprof; k++)
				if (g_prof[k].key == -(int)(i + 1)) {
					out->result = BK_IMPORT_DUPLICATE;
					out->profile = k;
					snprintf(out->name, sizeof(out->name), "%s", g_prof[k].p.name);
					return 0;
				}
		}
	char file[VTC_FILE_NAME_MAX];
	out->result = BK_IMPORT_FAILED;
	if (g_nprof >= BK_MAX_PROFILES || g_nimported >= BK_MAX_PROFILES ||
	    vtc_qr_file_name(parsed.endpoint_host, mock_name_taken, NULL, file, sizeof(file)) != 0)
		return -1;
	/* loose files are listed before the groups, as on the console */
	int at = 0;
	while (at < g_nprof && !g_prof[at].p.group[0])
		at++;
	memmove(&g_prof[at + 1], &g_prof[at], sizeof(g_prof[0]) * (size_t)(g_nprof - at));
	if (g_st.profile >= at)
		g_st.profile++;
	g_nprof++;
	MockProfile *m = &g_prof[at];
	memset(m, 0, sizeof(*m));
	m->p = parsed;
	file[strlen(file) - 5] = 0;
	snprintf(m->p.name, sizeof(m->p.name), "%s", file);
	snprintf(m->p.storage, sizeof(m->p.storage), "ux0");
	snprintf(m->p.path, sizeof(m->p.path), "ux0:" PF_FOLDER "/%s.conf", file);
	g_imported[g_nimported++] = h;
	m->key = -g_nimported;
	g_storages |= BK_STORE_UX0;
	out->result = BK_IMPORT_SAVED;
	out->profile = at;
	snprintf(out->name, sizeof(out->name), "%s", file);
	return 0;
}

/* ---- scenarios --------------------------------------------------------- */

enum {
	SC_CONNECTED,
	SC_CONNECTED_TRAVEL,
	SC_CONNECTING,
	SC_STALLED_HELD,
	SC_NOT_PROTECTED,
	SC_LEAK,
	SC_BLOCKED,
	SC_PAUSED,
	SC_IDLE,
	SC_PLUGIN_MISSING,
	SC_SHELL_MISSING,
	SC_MANY_PROFILES,
	SC_NO_PROFILES,
	SC_LOCKDOWN_IDLE,
	SC_LOCKDOWN_UP,
	SC_COUNT
};

static const char *const g_sc_names[SC_COUNT] = {
	[SC_CONNECTED]         = "connected",
	[SC_CONNECTED_TRAVEL]  = "connected, Travel",
	[SC_CONNECTING]        = "connecting",
	[SC_STALLED_HELD]      = "stalled, kill switch holding",
	[SC_NOT_PROTECTED]     = "not protected",
	[SC_LEAK]              = "leak found",
	[SC_BLOCKED]           = "blocked after wake",
	[SC_PAUSED]            = "paused",
	[SC_IDLE]              = "idle",
	[SC_PLUGIN_MISSING]    = "plugin missing",
	[SC_SHELL_MISSING]     = "shell plugin missing",
	[SC_MANY_PROFILES]     = "many profiles",
	[SC_NO_PROFILES]       = "no profiles",
	[SC_LOCKDOWN_IDLE]     = "lockdown, disconnected",
	[SC_LOCKDOWN_UP]       = "connected, lockdown",
};

static int g_sc;
static BkStatus g_st;
static BkComponents g_comp;
static BkCheck g_check;
static int g_stuck;            /* retries never succeed */
static int g_leaky;            /* DNS goes over Wi-Fi */
static uint64_t g_now, g_last; /* ms */
static uint32_t g_ms_acc;
static uint32_t g_connect_ms;  /* HANDSHAKING: ms until it comes up, 0 = never */
static uint32_t g_check_ms;    /* RUNNING: ms until the check finishes */

static void load_profile(int i)
{
	if (i >= g_nprof)
		i = g_nprof - 1;   /* a real folder's list may be shorter */
	if (i < 0) {
		g_st.profile = -1;
		return;
	}
	const BkProfile *p = &g_prof[i].p;
	unsigned a, b, c, d;
	g_st.profile = i;
	/* a hostname "resolves" to the address shown in the screenshots */
	if (sscanf(p->endpoint_host, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
		g_st.endpoint_ip = IP(a, b, c, d);
	else
		g_st.endpoint_ip = IP(203, 0, 113, 7);
	g_st.endpoint_port = p->endpoint_port;
	g_st.tunnel_ip = p->address;
	g_st.tunnel_prefix = p->address_prefix;
	g_st.mtu = p->mtu ? p->mtu : 1420;
}

static void set_state(BkState s)
{
	g_st.state = s;
	g_st.state_s = 0;
}


/* What the checks report for the current profile and state */
static void finish_check(void)
{
	BkCheckState was = g_check.state;
	memset(&g_check, 0, sizeof(g_check));
	g_check.state = was == BK_CHECK_NOT_RUN ? BK_CHECK_NOT_RUN : BK_CHECK_DONE;
	g_check.isp_ip = IP(198, 51, 100, 7);
	if (!g_comp.clock_ok) {
		g_check.state = BK_CHECK_FAILED;
		g_check.fail_reason = BK_ERR_CLOCK;
		g_check.dns = BK_DNS_UNMANAGED;   /* as the real check: the local verdict stands */
		return;
	}
	if (g_st.profile < 0 || g_st.state != BK_UP) {
		/* no tunnel: the exit is the ISP's address */
		g_check.exit_ip = g_check.isp_ip;
		g_check.dns = BK_DNS_UNMANAGED;
		g_check.local_count = 1;
		g_check.local[0].ip = IP(192, 168, 1, 1);
		return;
	}
	const BkProfile *p = &g_prof[g_st.profile].p;
	g_check.exit_ip = g_st.endpoint_ip;
	/* made-up location and network, as the service's JSON gives them */
	snprintf(g_check.city, sizeof(g_check.city), "Example City");
	snprintf(g_check.country, sizeof(g_check.country), "Exampleland");
	snprintf(g_check.org, sizeof(g_check.org), "Example Networks");
	if (!p->dns[0]) {
		g_check.dns = p->full_tunnel ? BK_DNS_UNMANAGED : BK_DNS_SPLIT;
		g_check.local_count = 1;
		g_check.local[0].ip = IP(192, 168, 1, 1);
		g_check.remote_done = 1;
		g_check.remote_count = 1;
		g_check.remote[0].ip = IP(198, 51, 100, 53);
		g_check.remote[0].kind = BK_RES_ISP;
		return;
	}
	g_check.remote_done = 1;
	if (g_leaky) {
		g_check.dns = BK_DNS_LEAK;
		g_check.local_count = 2;
		g_check.local[0].ip = IP(192, 168, 1, 1);
		g_check.local[1].ip = p->dns[0];
		g_check.local[1].via_tunnel = 1;
		g_check.remote_count = 2;
		g_check.remote[0].ip = IP(198, 51, 100, 53);
		g_check.remote[0].kind = BK_RES_ISP;
		g_check.remote[1].ip = IP(203, 0, 113, 53);
		g_check.remote[1].kind = BK_RES_VPN;
	} else {
		g_check.dns = BK_DNS_OK;
		g_check.local_count = 1;
		g_check.local[0].ip = p->dns[0];
		g_check.local[0].via_tunnel = 1;
		g_check.remote_count = 1;
		g_check.remote[0].ip = IP(203, 0, 113, 53);
		g_check.remote[0].kind = BK_RES_VPN;
	}
}

static void base_status(void)
{
	memset(&g_st, 0, sizeof(g_st));
	g_st.profile = -1;
	g_st.handshake_age_s = BK_NEVER;
	g_st.latency_ms = BK_NEVER;
	g_st.wlan_ip = IP(192, 168, 1, 23);
	g_st.wlan_prefix = 24;
	g_st.wlan_gateway = IP(192, 168, 1, 1);
	g_st.allow_lan = 1;
	g_st.lockdown_avail = 1;
	g_st.mtu = 1420;
	memset(&g_comp, 0, sizeof(g_comp));
	g_comp.module_loaded = g_comp.module_api_ok = 1;
	g_comp.shell_loaded = g_comp.shell_api_ok = 1;
	g_comp.clock_ok = 1;
	g_stuck = 0;
	g_leaky = 0;
	g_connect_ms = 0;
	g_check_ms = 0;
	memset(&g_check, 0, sizeof(g_check));
	g_check.isp_ip = IP(198, 51, 100, 7);   /* learnt on an earlier run */
}

static void up(int profile, uint32_t up_s, uint32_t age_s)
{
	load_profile(profile);
	set_state(BK_UP);
	g_st.up_s = up_s;
	g_st.state_s = up_s;
	g_st.handshake_age_s = age_s;
	g_st.latency_ms = 23;
	g_st.rx_bytes = 48ull << 20;
	g_st.tx_bytes = 6ull << 20;
	g_st.rx_packets = 41210;
	g_st.tx_packets = 23877;
}

static void down(int profile, BkState s, BkError e)
{
	load_profile(profile);
	set_state(s);
	g_st.last_error = e;
	g_st.state_s = 220;
	g_st.handshake_age_s = 220;
	g_st.retry_in_s = 41;
	g_st.attempts = 9;
	g_st.rx_bytes = 3ull << 20;
	g_st.tx_bytes = 1ull << 20;
	g_stuck = 1;
}

/* Disconnected under Lockdown: blocked, if a profile gives the tunnel
 * interface its address (vpnturbo.h, Lockdown) */
static void lockdown_follow(void)
{
	g_st.lockdown_block = g_st.lockdown_wait = 0;
	if (g_st.lockdown && g_st.state == BK_IDLE) {
		if (g_st.profile >= 0)
			g_st.lockdown_block = 1;
		else
			g_st.lockdown_wait = 1;
	}
}

void bk_scenario_set(int sc)
{
	if (sc < 0 || sc >= SC_COUNT)
		sc = 0;
	g_sc = sc;
	base_status();
	if (g_real) {
		pf_scan(g_st.wlan_ip, g_st.wlan_prefix);
		set_real();
	} else {
		set_basic();
	}
	switch (sc) {
	case SC_CONNECTED:
		up(0, 42 * 60 + 17, 14);
		break;
	case SC_CONNECTED_TRAVEL:
		up(1, 12 * 60 + 5, 37);
		break;
	case SC_CONNECTING:
		load_profile(0);
		set_state(BK_HANDSHAKING);
		g_st.state_s = 7;
		g_st.attempts = 2;
		g_st.retry_in_s = 3;
		g_stuck = 1;
		break;
	case SC_STALLED_HELD:
		down(0, BK_STALLED, BK_ERR_NO_ANSWER);
		g_st.kill_switch = 1;
		break;
	case SC_NOT_PROTECTED:
		down(0, BK_STALLED, BK_ERR_NO_ANSWER);
		break;
	case SC_LEAK:
		up(3, 5 * 60 + 2, 48);
		g_leaky = 1;
		break;
	case SC_BLOCKED:
		load_profile(0);
		set_state(BK_HANDSHAKING);
		g_st.locked = 1;
		g_st.lock_drops = 37;
		g_st.state_s = 12;
		g_st.attempts = 3;
		g_st.retry_in_s = 4;
		g_st.handshake_age_s = 31 * 60;
		g_stuck = 1;
		break;
	case SC_PAUSED:
		load_profile(0);
		set_state(BK_PAUSED);
		g_st.pause_left_s = 4 * 60 + 12;
		break;
	case SC_IDLE:
		break;
	case SC_PLUGIN_MISSING:
		g_comp.module_loaded = g_comp.module_api_ok = 0;
		break;
	case SC_SHELL_MISSING:
		g_comp.shell_loaded = g_comp.shell_api_ok = 0;
		break;
	case SC_MANY_PROFILES:
		if (!g_real)
			set_full();
		up(5, 3 * 60 + 40, 9);
		break;
	case SC_NO_PROFILES:
		if (!g_real) {
			g_nprof = 0;
			g_storages = 0;
		}
		break;
	case SC_LOCKDOWN_IDLE:
		load_profile(0);   /* preloaded at boot: its address is the block's */
		g_st.lockdown = 1;
		g_st.lock_drops = 112;
		g_st.lockdown_ep_unresolved = 1;   /* the longest muted line, for the layout check */
		break;
	case SC_LOCKDOWN_UP:
		up(0, 42 * 60 + 17, 14);
		g_st.lockdown = 1;
		break;
	}
	lockdown_follow();
	if (g_st.profile < 0 && g_st.state != BK_IDLE)
		set_state(BK_IDLE);   /* nothing to be connected to */
	g_check.state = BK_CHECK_DONE;
	finish_check();
}

int bk_scenario_count(void)
{
	return SC_COUNT;
}

int bk_scenario_get(void)
{
	return g_sc;
}

const char *bk_scenario_name(int i)
{
	return i >= 0 && i < SC_COUNT ? g_sc_names[i] : "";
}

/* ---- lifetime ---------------------------------------------------------- */

int bk_init(void)
{
	bk_scenario_set(SC_CONNECTED);
	return 0;
}

void bk_shutdown(void)
{
}

/* Advance the simulation by one second */
static void second(void)
{
	g_st.state_s++;
	if (g_st.handshake_age_s != BK_NEVER)
		g_st.handshake_age_s++;
	switch (g_st.state) {
	case BK_UP:
		g_st.up_s++;
		/* a rekey every two minutes while traffic flows */
		if (g_st.handshake_age_s >= 120)
			g_st.handshake_age_s = 0;
		g_st.rx_bytes += 180000;
		g_st.tx_bytes += 21000;
		g_st.rx_packets += 130;
		g_st.tx_packets += 70;
		break;
	case BK_HANDSHAKING:
	case BK_STALLED:
	case BK_ERROR:
		if (g_st.retry_in_s > 0)
			g_st.retry_in_s--;
		if (g_st.retry_in_s == 0) {
			g_st.attempts++;
			g_st.retry_in_s = g_st.state == BK_HANDSHAKING ? 5 : 45;
		}
		if (g_st.locked)
			g_st.lock_drops += 3;
		break;
	case BK_PAUSED:
		if (g_st.pause_left_s > 0)
			g_st.pause_left_s--;
		if (g_st.pause_left_s == 0) {
			set_state(BK_HANDSHAKING);
			g_connect_ms = 2500;
		}
		break;
	default:
		break;
	}
}

void bk_tick(uint64_t now_ms)
{
	if (!g_last)
		g_last = now_ms;
	uint32_t dt = (uint32_t)(now_ms - g_last);
	g_last = now_ms;
	g_now = now_ms;
	if (dt > 1000)
		dt = 1000;

	if (g_connect_ms && g_st.state == BK_HANDSHAKING) {
		if (g_connect_ms <= dt) {
			g_connect_ms = 0;
			g_st.locked = 0;
			g_st.up_s = 0;
			g_st.handshake_age_s = 0;
			g_st.last_error = BK_ERR_NONE;
			set_state(BK_UP);
			g_check.state = BK_CHECK_DONE;
			finish_check();
		} else {
			g_connect_ms -= dt;
		}
	}
	if (g_check_ms) {
		if (g_check_ms <= dt) {
			g_check_ms = 0;
			finish_check();
		} else {
			g_check_ms -= dt;
		}
	}
	g_ms_acc += dt;
	while (g_ms_acc >= 1000) {
		g_ms_acc -= 1000;
		second();
	}
}

void bk_get_status(BkStatus *s)
{
	*s = g_st;
}

void bk_get_components(BkComponents *c)
{
	*c = g_comp;
}

int bk_open_network_settings(void)
{
	return 0;
}

/* ---- profiles ---------------------------------------------------------- */

int bk_profile_count(void)
{
	return g_nprof;
}

int bk_profile_get(int i, BkProfile *out)
{
	if (i < 0 || i >= g_nprof)
		return -1;
	*out = g_prof[i].p;
	return 0;
}

void bk_rescan(void)
{
	if (g_real) {
		pf_scan(g_st.wlan_ip, g_st.wlan_prefix);
		set_real();
		if (g_st.profile >= g_nprof)
			g_st.profile = g_nprof - 1;
	}
}

unsigned bk_profiles_storages(void)
{
	return g_storages;
}

const char *bk_profiles_folder(void)
{
	return "ux0:" VT_PROFILE_DIR "/profiles/";
}

int bk_profile_set_nickname(int i, const char *nickname)
{
	if (i < 0 || i >= g_nprof)
		return -1;
	snprintf(g_prof[i].p.nickname, sizeof(g_prof[i].p.nickname), "%s", nickname);
	return 0;
}

int bk_profile_rename(int i, const char *name)
{
	if (i < 0 || i >= g_nprof || g_prof[i].p.in_zip)
		return -1;
	snprintf(g_prof[i].p.name, sizeof(g_prof[i].p.name), "%s", name);
	return 0;
}

int bk_profile_delete(int i)
{
	if (i < 0 || i >= g_nprof || g_prof[i].p.in_zip)
		return -1;
	memmove(&g_prof[i], &g_prof[i + 1], sizeof(g_prof[0]) * (size_t)(g_nprof - i - 1));
	g_nprof--;
	if (g_st.profile == i)
		g_st.profile = -1;
	else if (g_st.profile > i)
		g_st.profile--;
	return 0;
}

int bk_profile_set_check(int i, BkCheckPreset preset)
{
	if (i < 0 || i >= g_nprof)
		return -1;
	g_prof[i].p.check = preset;
	return 0;
}

/* ---- actions ----------------------------------------------------------- */

int bk_connect(int i)
{
	if (i < 0 || i >= g_nprof || (g_prof[i].p.warnings & BK_WARN_INVALID) || !g_comp.module_loaded)
		return -1;
	load_profile(i);
	set_state(BK_HANDSHAKING);
	g_st.handshake_age_s = BK_NEVER;
	g_st.attempts = 1;
	g_st.retry_in_s = 5;
	g_st.last_error = BK_ERR_NONE;
	g_st.rx_bytes = g_st.tx_bytes = 0;
	g_st.rx_packets = g_st.tx_packets = 0;
	g_stuck = 0;
	g_leaky = i == 3 && g_sc == SC_LEAK;
	g_connect_ms = 2500;
	lockdown_follow();
	return 0;
}

int bk_disconnect(void)
{
	set_state(BK_IDLE);
	g_st.locked = 0;
	g_st.handshake_age_s = BK_NEVER;
	g_st.last_error = BK_ERR_NONE;
	g_connect_ms = 0;
	lockdown_follow();
	return 0;
}

int bk_retry(void)
{
	if (g_st.state == BK_IDLE || g_st.state == BK_PAUSED || g_st.state == BK_UP)
		return -1;
	g_st.attempts++;
	g_st.retry_in_s = 5;
	if (!g_stuck) {
		set_state(BK_HANDSHAKING);
		g_connect_ms = 1500;
	}
	return 0;
}

BkMigrateKind bk_migrate_kind(int i)
{
	if (g_st.profile < 0 || i < 0 || i >= g_nprof)
		return BK_MIGRATE_RECONNECT;
	const MockProfile *a = &g_prof[g_st.profile], *b = &g_prof[i];
	return a->key == b->key && a->p.address == b->p.address
		? BK_MIGRATE_SEAMLESS : BK_MIGRATE_RECONNECT;
}

int bk_migrate(int i)
{
	if (i < 0 || i >= g_nprof)
		return -1;
	if (bk_migrate_kind(i) == BK_MIGRATE_SEAMLESS && g_st.state == BK_UP) {
		load_profile(i);
		g_st.handshake_age_s = 0;
		finish_check();
		return 0;
	}
	return bk_connect(i);
}

void bk_trace(const char *what) { (void)what; }

int bk_pause(uint32_t seconds)
{
	if (g_st.state == BK_IDLE)
		return -1;
	if (g_st.lockdown)
		return BK_PAUSE_LOCKDOWN;   /* as the module refuses it */
	set_state(BK_PAUSED);
	g_st.locked = 0;
	g_st.pause_left_s = seconds;
	g_connect_ms = 0;
	return 0;
}

int bk_resume(void)
{
	if (g_st.state != BK_PAUSED)
		return -1;
	set_state(BK_HANDSHAKING);
	g_stuck = 0;
	g_connect_ms = 2000;
	return 0;
}

void bk_get_options(BkOptions *o)
{
	o->kill_switch = g_st.kill_switch;
	o->allow_lan = g_st.allow_lan;
	o->lockdown = g_st.lockdown;
}

int bk_set_options(const BkOptions *o)
{
	g_st.kill_switch = o->kill_switch;
	g_st.allow_lan = o->allow_lan;
	if (g_st.lockdown_avail) {
		g_st.lockdown = o->lockdown != 0;
		/* turning it on during a pause ends the pause */
		if (g_st.lockdown && g_st.state == BK_PAUSED) {
			set_state(BK_HANDSHAKING);
			g_connect_ms = 2000;
		}
	}
	lockdown_follow();
	return 0;
}

/* ---- checks ------------------------------------------------------------ */

void bk_check_start(void)
{
	g_check.state = BK_CHECK_RUNNING;
	g_check_ms = 1500;
}

void bk_check_get(BkCheck *out)
{
	*out = g_check;
}

void bk_guard_reset(void)
{
}
