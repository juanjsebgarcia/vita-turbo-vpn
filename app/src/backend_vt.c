/*
 * backend_vt.c - the real back end, talking to vpnturbo_kernel.skprx
 * through the SceVpnTurbo syscall library
 * (include/vpnturbo.h).
 *
 * The profile list is real: the profiles folders on ux0, uma0, imc0 and
 * ur0 (profiles.c), parsed in the app, and QR import writes into
 * the one on ux0.
 *
 * The module's library is a weak import, so the app starts without it.
 * A weak import that was never linked must not be called, so nothing here
 * calls the module until module_probe() has found it by name and checked
 * its API version (as the SceShell plugin's vt_client.c does). Kernel
 * modules load at boot, before any app, so a module found now was linked
 * when the app started; one missing at the first probe is never called in
 * this run, even if it appears later.
 *
 * Connecting is done on a worker thread, so a hostname lookup does not
 * hold up the screen: the worker resolves the endpoint with the system
 * resolver, reads and parses the profile, loads it with vtSetProfile (the
 * address filled in), records its path for the SceShell plugin's preload
 * (path only, never contents) and connects. The profile text and the
 * keys are wiped as soon as the module has them. Until the worker is
 * done the status says Connecting; if it fails, Error with the reason.
 *
 * The checks run on a second worker thread, so an HTTPS
 * request that takes seconds never holds up the screen or a connect: the
 * exit IP comes from an HTTPS echo service through the app's own TLS
 * (https_get.c), and the DNS servers in use are matched against the
 * module's routes. They run once each time the tunnel comes up, once at
 * start while it is down (to learn the ISP's address), and when the user
 * asks; never on a timer.
 *
 * The app does not set the tunnel interface's address: the SceShell plugin
 * does that whichever surface connected, and it also lends the module the
 * threads that carry the traffic. Without the plugin the tunnel
 * cannot work, so the app says so instead of offering to connect.
 */
#include <psp2/io/fcntl.h>
#include "../../include/vt_paths.h"
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/vshbridge.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../include/vpnturbo.h"
#include "backend.h"
#include "https_get.h"
#include "profiles.h"
#include "vtconf.h"

/* The kernel module's name in its yml, not its file name */
#define VT_MODULE_NAME   "vpnturbo_kernel"
/* The SceShell plugin's file, as listed in the taiHEN config */
#define SHELL_FILE       "vpnturbo.suprx"
/* The last profile connected, one line, its path; read by the plugin */
#define PLUGIN_DATA_DIR  VT_DATA_DIR
#define LAST_PROFILE     PLUGIN_DATA_DIR "/last_profile"
/* 8.2: the last public address seen with the tunnel down, one dotted line */
#define ISP_FILE         PLUGIN_DATA_DIR "/isp_ip"
#define APP_LOG          PLUGIN_DATA_DIR "/app.log"
#define APP_LOG_OLD      PLUGIN_DATA_DIR "/app.old.log"
#define APP_LOG_MAX      (64 * 1024)

static void app_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The exit-IP service unless the profile names its own */
#define DEFAULT_CHECK_URL "https://am.i.mullvad.net/json"
#define CHECK_TIMEOUT_MS 8000

#define POLL_MS          250
#define WIFI_POLL_MS     2000
/* The protocol's REKEY_TIMEOUT: an initiation is retried every 5 s */
#define RETRY_S          5
/* 2025-01-01 00:00 UTC: a clock before this has been reset */
#define CLOCK_FLOOR      1735689600

/* ---- small helpers ------------------------------------------------------ */

static void wipe(void *p, size_t n)
{
	vtc_wipe(p, n);
}

static uint32_t parse_ip(const char *s)
{
	unsigned a, b, c, d;
	if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return 0;
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

/* A dotted quad and nothing else: no lookup needed */
static int is_dotted(const char *s)
{
	for (const char *p = s; *p; p++)
		if ((*p < '0' || *p > '9') && *p != '.')
			return 0;
	return parse_ip(s) != 0;
}

/* Copy, cut to fit (never inside a UTF-8 character) */
static void copy_str(char *dst, size_t n, const char *src)
{
	size_t len = src ? strlen(src) : 0;
	if (len >= n) {
		len = n - 1;
		while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80)
			len--;
	}
	if (len)
		memcpy(dst, src, len);
	dst[len] = 0;
}

/* ---- network: Wi-Fi subnet and the resolver ----------------------- */

static uint32_t g_wlan_ip, g_wlan_gw;
static uint8_t g_wlan_prefix;

static void net_init(void)
{
	static int done;
	static char pool[128 * 1024];
	if (done)
		return;
	done = 1;
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	SceNetInitParam p;
	p.memory = pool;
	p.size = sizeof(pool);
	p.flags = 0;
	sceNetInit(&p);       /* already initialised is fine */
	sceNetCtlInit();
}

/* The Wi-Fi address, prefix length and gateway now; 0 when not connected */
static void wifi_subnet(void)
{
	SceNetCtlInfo info;
	uint32_t ip = 0, gw = 0;
	uint8_t prefix = 0;
	net_init();
	memset(&info, 0, sizeof(info));
	if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
		ip = parse_ip(info.ip_address);
	memset(&info, 0, sizeof(info));
	if (ip && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &info) >= 0) {
		uint32_t mask = parse_ip(info.netmask);
		while (prefix < 32 && (mask & (0x80000000u >> prefix)))
			prefix++;
	}
	memset(&info, 0, sizeof(info));
	if (ip && sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE, &info) >= 0)
		gw = parse_ip(info.default_route);
	g_wlan_ip = prefix ? ip : 0;
	g_wlan_prefix = prefix;
	g_wlan_gw = prefix ? gw : 0;
}

/* host to an IPv4 address in host order; 0, or -1 (worker thread only) */
static int resolve(const char *host, uint32_t *ip)
{
	SceNetInAddr addr;
	memset(&addr, 0, sizeof(addr));
	int rid = sceNetResolverCreate("vpnturbo_app_resolver", NULL, 0);
	if (rid < 0)
		return -1;
	int r = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
	sceNetResolverDestroy(rid);
	if (r < 0 || addr.s_addr == 0)
		return -1;
	*ip = __builtin_bswap32(addr.s_addr);   /* network to host order */
	return 0;
}

/* ---- installation ------------------------------------------- */

enum { MOD_UNPROBED, MOD_ABSENT, MOD_OLD, MOD_READY };
enum { CFG_UNKNOWN, CFG_ABSENT, CFG_LISTED };

static int g_mod = MOD_UNPROBED;
static int g_mod_never;         /* absent at the first probe: never call it */
static int g_shell_cfg = CFG_UNKNOWN;
static int g_shell_seen;        /* the plugin's lent threads have done work */

static void module_probe(void)
{
	int search[4] = { 0, 0, 0, 0 };
	if (g_mod_never) {
		g_mod = MOD_ABSENT;
		return;
	}
	if (_vshKernelSearchModuleByName(VT_MODULE_NAME, search) < 0) {
		if (g_mod == MOD_UNPROBED)
			g_mod_never = 1;
		g_mod = MOD_ABSENT;
		return;
	}
	VtStatus st;
	memset(&st, 0, sizeof(st));
	st.size = sizeof(st);
	int r = vtGetStatus(&st);
	g_mod = r < 0 || st.api_version != VT_API_VERSION ? MOD_OLD : MOD_READY;
}

static int ends_with_ci(const char *s, size_t n, const char *suffix)
{
	size_t m = strlen(suffix);
	if (n < m)
		return 0;
	for (size_t i = 0; i < m; i++) {
		char a = s[n - m + i], b = suffix[i];
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

/*
 * Whether the SceShell plugin is listed in the taiHEN config that was read
 * at boot (ux0:tai/config.txt if it exists, else ur0's) under *main or
 * *ALL, and its file is there. Listed is not proof that it is running, but
 * not listed means it is not; the status then confirms it (shell_seen).
 */
static void shell_probe(void)
{
	static char cfg[32 * 1024];
	SceIoStat st;
	const char *path = sceIoGetstat("ux0:tai/config.txt", &st) >= 0
		? "ux0:tai/config.txt" : "ur0:tai/config.txt";
	g_shell_cfg = CFG_UNKNOWN;
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int n = sceIoRead(fd, cfg, sizeof(cfg) - 1);
	sceIoClose(fd);
	if (n < 0)
		return;
	cfg[n] = 0;
	g_shell_cfg = CFG_ABSENT;
	int in_shell = 0;
	for (char *line = cfg; line && *line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		while (*line == ' ' || *line == '\t')
			line++;
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
			line[--len] = 0;
		if (line[0] == '*')
			in_shell = !strcmp(line + 1, "main") || !strcmp(line + 1, "ALL");
		else if (in_shell && line[0] != '#' && ends_with_ci(line, len, SHELL_FILE) &&
		         sceIoGetstat(line, &st) >= 0)
			g_shell_cfg = CFG_LISTED;
		line = next;
	}
}

/* ---- status polling ----------------------------------------------------- */

static VtStatus g_vs;           /* the last status read; main thread */
static int g_vs_ok;
static uint64_t g_now, g_next_poll, g_next_wifi;
static uint32_t g_prev_state = 0xFFFFFFFFu;
static int g_active_idx = -1;   /* the module's profile in the list, as last reported */
static uint64_t g_state_since, g_up_since;

static void poll_status(void)
{
	if (g_mod != MOD_READY) {
		g_vs_ok = 0;
		return;
	}
	VtStatus s;
	memset(&s, 0, sizeof(s));
	s.size = sizeof(s);
	if (vtGetStatus(&s) < 0) {
		g_vs_ok = 0;
		return;
	}
	g_vs = s;
	g_vs_ok = 1;
	if (s.state != g_prev_state) {
		g_prev_state = s.state;
		g_state_since = g_now;
		if (s.state == VT_STATE_UP)
			g_up_since = g_now;
	}
	/* Only the plugin's lent threads find the gateway for the endpoint
	 * route and carry handshakes and received packets: any of them
	 * means the plugin is running. */
	if (s.wlan_gateway || s.handshake_age_s != VT_NEVER || s.rx_packets)
		g_shell_seen = 1;
}

/* ---- the connect worker ------------------------------------------------- */

enum { JOB_NONE, JOB_CONNECT, JOB_MIGRATE, JOB_RETRY };

typedef struct Job {
	int kind;
	int seamless;             /* JOB_MIGRATE: keep the tunnel's key */
	char real[512];           /* the file, or the zip it is in */
	int zip_index;
	char name[64];            /* the profile's display name for the module */
	char host[256];           /* the endpoint as written */
	uint16_t port;
} Job;

typedef struct Source {
	int valid;
	char real[512];
	int zip_index;
} Source;

/* Shared with the worker, under g_lock */
static SceUID g_lock = -1, g_wake = -1, g_thread = -1;
static volatile int g_run;
static Job g_job;               /* the request waiting for the worker */
static uint32_t g_gen;          /* bumped by every request and disconnect */
static int g_pending;           /* a connect or migrate is being worked on */
static int g_poll_now;          /* the worker changed the module's state */
static uint64_t g_pending_since;
static Source g_target;         /* the profile being connected */
static Source g_active;         /* the profile the module has */
static int g_fail;              /* the last connect failed before vtConnect */
static BkError g_fail_err;
static char g_fail_detail[64];

static void lock(void)
{
	sceKernelLockMutex(g_lock, 1, NULL);
}

static void unlock(void)
{
	sceKernelUnlockMutex(g_lock, 1);
}

static BkError map_error(uint32_t e)
{
	switch (e) {
	case VT_ERR_NONE:
		return BK_ERR_NONE;
	case VT_ERR_BAD_PROFILE:
	case VT_ERR_NO_PROFILE:
		return BK_ERR_PROFILE;
	case VT_ERR_NO_WIFI:
		return BK_ERR_WIFI_OFF;
	case VT_ERR_NO_ANSWER:
		return BK_ERR_NO_ANSWER;
	default:
		return BK_ERR_SOCKET;
	}
}

/* English detail under the cause, as the parser's messages are */
static const char *error_detail(uint32_t e)
{
	switch (e) {
	case VT_ERR_BAD_PROFILE:
		return "the kernel module refused the profile";
	case VT_ERR_NO_PROFILE:
		return "no profile loaded";
	case VT_ERR_ROUTE:
		return "a route could not be added";
	case VT_ERR_DENIED:
		return "the kernel module refused this app";
	case VT_ERR_SOCKET:
		return "the tunnel could not be opened";
	default:
		return "";
	}
}

/* Called with g_lock held */
static void fail(BkError e, const char *detail)
{
	g_fail = 1;
	g_fail_err = e;
	copy_str(g_fail_detail, sizeof(g_fail_detail), detail);
}

/* A syscall's return code as a failure (g_lock held) */
static void fail_rc(int r)
{
	uint32_t e = r < 0 && r > -64 ? (uint32_t)-r : VT_ERR_SOCKET;
	fail(map_error(e), error_detail(e));
}

/* The path only, for a loose file; a zip entry cannot be preloaded,
 * so an older path is removed rather than left to preload the wrong one */
static void record_last(const Source *s)
{
	if (s->zip_index >= 0) {
		sceIoRemove(LAST_PROFILE);
		return;
	}
	char line[sizeof(s->real) + 2];
	int n = snprintf(line, sizeof(line), "%s\n", s->real);
	sceIoMkdir(PLUGIN_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(LAST_PROFILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, line, (SceSize)n);
	sceIoClose(fd);
}

/* Connect or migrate: resolve, read, parse, load, record, connect */
static void run_load(const Job *j, uint32_t gen)
{
	static char text[PF_TEXT_MAX];
	static VtcResult res;
	static VtProfile prof;
	uint32_t ip = 0;
	int have_ip = 0;

	/* the lookup first, before any key is in memory */
	if (!is_dotted(j->host)) {
		if (resolve(j->host, &ip) != 0) {
			lock();
			if (gen == g_gen)
				fail(BK_ERR_RESOLVE, j->host);
			unlock();
			return;
		}
		have_ip = 1;
	}

	long n = pf_read_source(j->real, j->zip_index, text, sizeof(text));
	if (n < 0) {
		lock();
		if (gen == g_gen)
			fail(BK_ERR_PROFILE, "the file could not be read");
		unlock();
		return;
	}
	int pr = vtc_parse(text, (size_t)n, &res);
	wipe(text, sizeof(text));
	if (pr != VTC_OK || vtc_to_profile(&res, j->name, &prof) != 0) {
		const char *why = vtc_error_text(res.error);
		wipe(&res, sizeof(res));
		wipe(&prof, sizeof(prof));
		lock();
		if (gen == g_gen)
			fail(BK_ERR_PROFILE, why);
		unlock();
		return;
	}
	char host[VTC_HOST_MAX];
	int is_ip = res.endpoint_is_ip;
	copy_str(host, sizeof(host), res.endpoint_host);
	wipe(&res, sizeof(res));

	/* the file changed since the list was read: look its endpoint up now */
	if (!is_ip && (!have_ip || strcmp(host, j->host) != 0)) {
		if (resolve(host, &ip) != 0) {
			wipe(&prof, sizeof(prof));
			lock();
			if (gen == g_gen)
				fail(BK_ERR_RESOLVE, host);
			unlock();
			return;
		}
	}
	if (!is_ip)
		prof.endpoint_ip = ip;

	/* the module calls under the lock, so a disconnect in the meantime
	 * wins: nothing is connected after it */
	lock();
	if (gen != g_gen) {
		unlock();
		wipe(&prof, sizeof(prof));
		return;
	}
	int r;
	if (j->kind == JOB_MIGRATE && j->seamless) {
		r = vtMigrate(&prof);          /* loads and connects */
	} else {
		if (j->kind == JOB_MIGRATE)
			vtDisconnect();             /* reconnect */
		r = vtSetProfile(&prof);
	}
	wipe(&prof, sizeof(prof));
	if (r < 0) {
		fail_rc(r);
		unlock();
		return;
	}
	g_active.valid = 1;
	copy_str(g_active.real, sizeof(g_active.real), j->real);
	g_active.zip_index = j->zip_index;
	record_last(&g_active);
	if (!(j->kind == JOB_MIGRATE && j->seamless)) {
		r = vtConnect();
		if (r < 0)
			fail_rc(r);
	}
	unlock();
}

/* Retry: a hostname endpoint is looked up again, as its address may have
 * changed; a new address restarts the handshake, else vtConnect does */
static void run_retry(const Job *j, uint32_t gen)
{
	uint32_t ip = 0;
	int have_ip = j->host[0] && !is_dotted(j->host) && resolve(j->host, &ip) == 0;
	lock();
	if (gen == g_gen) {
		VtStatus s;
		memset(&s, 0, sizeof(s));
		s.size = sizeof(s);
		int r;
		if (have_ip && vtGetStatus(&s) >= 0 && s.endpoint_ip != ip)
			r = vtSetEndpoint(ip, j->port);
		else
			r = vtConnect();
		(void)r;
	}
	unlock();
}

static int worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (g_run) {
		if (sceKernelWaitSema(g_wake, 1, NULL) < 0 || !g_run)
			break;
		Job j;
		lock();
		j = g_job;
		g_job.kind = JOB_NONE;
		uint32_t gen = g_gen;
		unlock();
		if (j.kind == JOB_CONNECT || j.kind == JOB_MIGRATE)
			run_load(&j, gen);
		else if (j.kind == JOB_RETRY)
			run_retry(&j, gen);
		lock();
		if (gen == g_gen && j.kind != JOB_RETRY)
			g_pending = 0;
		g_poll_now = 1;
		unlock();
	}
	return sceKernelExitDeleteThread(0);
}

static int worker_start(void)
{
	if (g_thread >= 0)
		return 0;
	g_lock = sceKernelCreateMutex("vpnturbo_lock", 0, 0, NULL);
	g_wake = sceKernelCreateSema("vpnturbo_wake", 0, 0, 1, NULL);
	if (g_lock < 0 || g_wake < 0)
		return -1;
	g_run = 1;
	g_thread = sceKernelCreateThread("vpnturbo_connect", worker, 0x10000100, 0x10000, 0, 0, NULL);
	if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
		g_run = 0;
		g_thread = -1;
		return -1;
	}
	return 0;
}

/* Hand a job to the worker; a waiting one is replaced */
static void submit(const Job *j)
{
	lock();
	g_gen++;
	g_job = *j;
	if (j->kind != JOB_RETRY) {
		g_pending = 1;
		g_pending_since = g_now;
		g_fail = 0;
		g_target.valid = 1;
		copy_str(g_target.real, sizeof(g_target.real), j->real);
		g_target.zip_index = j->zip_index;
	}
	unlock();
	sceKernelSignalSema(g_wake, 1);
}

static int job_for(int profile, int kind, Job *j)
{
	const BkProfile *p = pf_get(profile);
	int zip;
	const char *real = pf_source(profile, &zip);
	if (!p || !real || (p->warnings & BK_WARN_INVALID))
		return -1;
	memset(j, 0, sizeof(*j));
	j->kind = kind;
	copy_str(j->real, sizeof(j->real), real);
	j->zip_index = zip;
	copy_str(j->name, sizeof(j->name), p->name);
	copy_str(j->host, sizeof(j->host), p->endpoint_host);
	j->port = p->endpoint_port;
	return 0;
}

static int source_index(const Source *s)
{
	return s->valid ? pf_find(s->real, s->zip_index) : -1;
}

/*
 * The profile the module has when the app starts: the plugin preloads the
 * last one, or an earlier run of the app loaded one. The recorded
 * path is trusted when the module's name for it agrees; else the first
 * profile with that name.
 */
static int name_matches(const char *list_name, const char *module_name)
{
	size_t m = strlen(module_name), l = strlen(list_name);
	if (!m || m > l || memcmp(list_name, module_name, m) != 0)
		return 0;
	return m == l || m >= VT_NAME_MAX - 4;   /* the module keeps 31 bytes */
}

static void infer_active(void)
{
	char path[512];
	int idx = -1;
	if (g_active.valid)
		return;
	SceUID fd = sceIoOpen(LAST_PROFILE, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int n = sceIoRead(fd, path, sizeof(path) - 1);
		sceIoClose(fd);
		path[n > 0 ? n : 0] = 0;
		path[strcspn(path, "\r\n")] = 0;
		vt_path_current(path, sizeof(path));   /* saved under a former folder name */
		idx = path[0] ? pf_find(path, -1) : -1;
	}
	const char *name = g_vs_ok ? g_vs.profile_name : "";
	if (idx >= 0 && name[0] && !name_matches(pf_get(idx)->name, name))
		idx = -1;
	for (int i = 0; idx < 0 && name[0] && i < pf_count(); i++)
		if (name_matches(pf_get(i)->name, name))
			idx = i;
	if (idx >= 0) {
		const char *real = pf_source(idx, &g_active.zip_index);
		if (real) {
			copy_str(g_active.real, sizeof(g_active.real), real);
			g_active.valid = 1;
		}
	}
}

/* ---- lifetime ----------------------------------------------------------- */

static void isp_load(void);
static void check_worker_start(void);
static void check_worker_stop(void);
static void check_tick(void);

/* A former name's profile folder becomes the current one, on ux0: and ur0:,
 * when the current one does not exist yet (vt_paths.h; nothing to do while
 * the list of former names is empty). */
static void profile_dir_migrate(void)
{
	static const char *const old[] = { VT_LEGACY_PROFILE_DIRS 0 };
	static const char *const dev[] = { "ux0:", "ur0:" };
	char from[96], to[96];
	SceIoStat st;

	for (int d = 0; d < 2; d++) {
		snprintf(to, sizeof(to), "%s%s", dev[d], VT_PROFILE_DIR);
		if (sceIoGetstat(to, &st) >= 0)
			continue;
		for (int i = 0; old[i]; i++) {
			snprintf(from, sizeof(from), "%s%s", dev[d], old[i]);
			if (sceIoGetstat(from, &st) >= 0) {
				sceIoRename(from, to);
				break;
			}
		}
	}
}

int bk_init(void)
{
	profile_dir_migrate();
	pf_make_folder();
	module_probe();
	shell_probe();
	if (g_mod == MOD_READY)
		worker_start();
	g_next_poll = 0;
	poll_status();
	bk_rescan();
	if (g_lock >= 0) {
		lock();
		infer_active();
		unlock();
	}
	isp_load();
	check_worker_start();
	return 0;
}

void bk_shutdown(void)
{
	if (g_thread >= 0) {
		g_run = 0;
		sceKernelSignalSema(g_wake, 1);
		sceKernelWaitThreadEnd(g_thread, NULL, NULL);
		g_thread = -1;
	}
	check_worker_stop();
}

void bk_tick(uint64_t now_ms)
{
	g_now = now_ms;
	if (now_ms >= g_next_poll) {
		g_next_poll = now_ms + POLL_MS;
		poll_status();
	}
	if (now_ms >= g_next_wifi) {
		g_next_wifi = now_ms + WIFI_POLL_MS;
		wifi_subnet();
	}
	check_tick();
}

static BkState map_state(uint32_t s, int *locked)
{
	*locked = 0;
	switch (s) {
	case VT_STATE_HANDSHAKING:
		return BK_HANDSHAKING;
	case VT_STATE_UP:
		return BK_UP;
	case VT_STATE_STALLED:
		return BK_STALLED;
	case VT_STATE_ERROR:
		return BK_ERROR;
	case VT_STATE_PAUSED:
		return BK_PAUSED;
	case VT_STATE_LOCKED:
		*locked = 1;          /* Blocked, reconnecting */
		return BK_HANDSHAKING;
	default:
		return BK_IDLE;
	}
}

static uint32_t secs_since(uint64_t t)
{
	return g_now > t ? (uint32_t)((g_now - t) / 1000) : 0;
}

void bk_get_status(BkStatus *s)
{
	memset(s, 0, sizeof(*s));
	s->profile = -1;
	s->handshake_age_s = BK_NEVER;
	s->latency_ms = BK_NEVER;
	s->allow_lan = 1;
	s->wlan_ip = g_wlan_ip;
	s->wlan_prefix = g_wlan_prefix;
	s->wlan_gateway = g_wlan_gw;
	if (g_mod != MOD_READY || g_lock < 0)
		return;

	/* Under the lock throughout, so the worker's result and the module's
	 * state it led to are seen together */
	lock();
	if (g_poll_now) {
		g_poll_now = 0;
		poll_status();
	}
	if (!g_vs_ok) {
		unlock();
		return;
	}
	const VtStatus *v = &g_vs;
	s->state = map_state(v->state, &s->locked);
	s->handshake_age_s = v->handshake_age_s == VT_NEVER ? BK_NEVER : v->handshake_age_s;
	s->latency_ms = v->latency_source == VT_LAT_NONE || v->latency_ms == VT_NEVER
		? BK_NEVER : v->latency_ms;
	s->state_s = secs_since(g_state_since);
	if (s->state == BK_UP)
		s->up_s = secs_since(g_up_since);
	if (s->state == BK_HANDSHAKING || s->state == BK_STALLED || s->state == BK_ERROR) {
		/* the module does not report its retry timer; one initiation
		 * every RETRY_S from the state change is what it does */
		s->attempts = 1 + s->state_s / RETRY_S;
		s->retry_in_s = RETRY_S - s->state_s % RETRY_S;
	}
	s->pause_left_s = v->pause_left_s;
	s->rx_bytes = v->rx_bytes;
	s->tx_bytes = v->tx_bytes;
	s->rx_packets = v->rx_packets;
	s->tx_packets = v->tx_packets;
	s->endpoint_ip = v->endpoint_ip;
	s->endpoint_port = v->endpoint_port;
	s->mtu = v->mtu;
	s->tunnel_ip = v->tunnel_address;
	s->tunnel_prefix = v->tunnel_prefix;
	if (v->wlan_address) {
		s->wlan_ip = v->wlan_address;
		s->wlan_prefix = v->wlan_prefix;
	}
	if (v->wlan_gateway)
		s->wlan_gateway = v->wlan_gateway;
	s->kill_switch = v->kill_switch;
	s->lock_drops = v->lock_drops;
	if (v->hold == VT_HOLD_KILL_SWITCH && s->state == BK_IDLE) {
		/* Disconnected with the kill switch on: the routes stay and the
		 * console is offline, so say so rather than "Off" */
		s->state = BK_STALLED;
		s->kill_switch = 1;
	}
	s->allow_lan = v->allow_local;
	s->lockdown_avail = (VT_ST_FEATURES(v) & VT_FEAT_LOCKDOWN) != 0;
	s->lockdown = s->lockdown_avail && VT_ST_LOCKDOWN(v) != 0;
	s->lockdown_ep_unresolved = s->lockdown && (VT_ST_FEATURES(v) & VT_FEAT_NOTES) &&
		(VT_ST_NOTES(v) & VT_NOTE_EP_UNRESOLVED);
	if (s->lockdown && s->state == BK_IDLE) {
		/* VT_RT_BLOCK: the reject routes that stand in before the interface is up */
		if (v->hold == VT_HOLD_LOCKDOWN_MODE && (v->route_flags & (VT_RT_TUNNEL | VT_RT_BLOCK)))
			s->lockdown_block = 1;
		else
			s->lockdown_wait = 1;
	}
	if (s->state != BK_IDLE && s->state != BK_UP) {
		s->last_error = map_error(v->last_error);
		copy_str(s->last_error_detail, sizeof(s->last_error_detail), error_detail(v->last_error));
	}

	s->profile = g_active_idx = source_index(&g_active);
	if (g_pending) {
		/* the worker is looking up, loading or connecting */
		s->state = BK_HANDSHAKING;
		s->locked = 0;
		s->lockdown_block = s->lockdown_wait = 0;
		s->profile = source_index(&g_target);
		s->state_s = secs_since(g_pending_since);
		s->attempts = 1;
		s->retry_in_s = 0;
		s->handshake_age_s = BK_NEVER;
		s->latency_ms = BK_NEVER;
		s->last_error = BK_ERR_NONE;
		s->last_error_detail[0] = 0;
	} else if (g_fail && v->state == VT_STATE_IDLE) {
		/* it failed before the module was asked to connect */
		s->state = BK_ERROR;
		s->lockdown_block = s->lockdown_wait = 0;
		s->profile = source_index(&g_target);
		s->state_s = secs_since(g_state_since);
		s->last_error = g_fail_err;
		copy_str(s->last_error_detail, sizeof(s->last_error_detail), g_fail_detail);
	}
	unlock();
}

/* The plugin's boot guard (shell/src/main.c): a count of starts that did
 * not finish; two or more and it stays inert. Read at most every 5 s. */
#define GUARD_FILE  PLUGIN_DATA_DIR "/bootguard"
#define GUARD_LIMIT 2

static int guard_tripped(void)
{
	static uint64_t read_at;
	static int tripped, read_once;
	if (read_once && g_now - read_at < 5000)
		return tripped;
	read_once = 1;
	read_at = g_now;
	char c = 0;
	SceUID fd = sceIoOpen(GUARD_FILE, SCE_O_RDONLY, 0);
	tripped = 0;
	if (fd >= 0) {
		tripped = sceIoRead(fd, &c, 1) == 1 && c >= '0' + GUARD_LIMIT && c <= '9';
		sceIoClose(fd);
	}
	return tripped;
}

void bk_guard_reset(void)
{
	sceIoRemove(GUARD_FILE);
	app_log("boot guard cleared by the user; restarting");
	scePowerRequestColdReset();
}

void bk_get_components(BkComponents *c)
{
	memset(c, 0, sizeof(*c));
	c->module_loaded = g_mod == MOD_READY || g_mod == MOD_OLD;
	c->module_api_ok = g_mod == MOD_READY;
	/* No call tells the app whether the plugin is running. Seen working
	 * (shell_seen) or listed in the taiHEN config counts as loaded; only
	 * a readable config without it counts as missing. */
	c->shell_loaded = g_shell_seen || g_shell_cfg != CFG_ABSENT;
	/* The module knows better: no SceShell thread lent this boot, a minute
	 * after boot, means the plugin is not running whatever config.txt says */
	if (g_vs_ok && (VT_ST_FEATURES(&g_vs) & VT_FEAT_NO_SHELL) &&
	    (VT_ST_NOTES(&g_vs) & VT_NOTE_NO_SHELL)) {
		c->shell_loaded = 0;
		c->shell_guard = guard_tripped();
	}
	c->shell_api_ok = c->shell_loaded;
	c->clock_ok = time(NULL) >= (time_t)CLOCK_FLOOR;
}

int bk_open_network_settings(void)
{
	/* TODO: launch Settings at Network with sceAppMgrLaunchAppByUri */
	return -1;
}

/* ---- profiles: parsed in the app, not the kernel ------------ */

int bk_profile_count(void)
{
	return pf_count();
}

int bk_profile_get(int index, BkProfile *out)
{
	const BkProfile *p = pf_get(index);
	if (!p)
		return -1;
	*out = *p;
	return 0;
}

void bk_rescan(void)
{
	/* TODO: match profiles.meta for nicknames and Recent */
	wifi_subnet();
	pf_scan(g_wlan_ip, g_wlan_prefix);
}

unsigned bk_profiles_storages(void)
{
	return pf_storages();
}

int bk_import_text(const char *text, size_t len, BkImport *out)
{
	return pf_import(text, len, out);
}

const char *bk_profiles_folder(void)
{
	return "ux0:" VT_PROFILE_DIR "/profiles/";
}

int bk_profile_set_nickname(int index, const char *nickname)
{
	(void)index;
	(void)nickname;
	return -1;
}

int bk_profile_rename(int index, const char *new_name)
{
	(void)index;
	(void)new_name;
	return -1;
}

int bk_profile_delete(int index)
{
	(void)index;
	return -1;
}

int bk_profile_set_check(int index, BkCheckPreset preset)
{
	(void)index;
	(void)preset;
	return -1;
}

/* ---- actions ----------------------------------------------------------- */

static int ready(void)
{
	BkComponents c;
	bk_get_components(&c);
	return g_mod == MOD_READY && g_thread >= 0 && c.shell_loaded;
}

int bk_connect(int profile)
{
	Job j;
	if (!ready() || job_for(profile, JOB_CONNECT, &j) != 0)
		return -1;
	submit(&j);
	return 0;
}

int bk_disconnect(void)
{
	if (g_mod != MOD_READY || g_lock < 0)
		return -1;
	lock();
	g_gen++;                  /* whatever the worker is doing is dropped */
	g_job.kind = JOB_NONE;
	g_pending = 0;
	g_fail = 0;
	int r = vtDisconnect();
	unlock();
	poll_status();
	return r < 0 ? -1 : 0;
}

int bk_retry(void)
{
	Job j;
	if (!ready())
		return -1;
	lock();
	int failed = g_fail && g_vs_ok && g_vs.state == VT_STATE_IDLE;
	int target = source_index(failed ? &g_target : &g_active);
	unlock();
	if (failed)
		return target >= 0 ? bk_connect(target) : -1;
	if (!g_vs_ok || g_vs.state == VT_STATE_IDLE)
		return -1;
	if (target < 0 || job_for(target, JOB_RETRY, &j) != 0) {
		memset(&j, 0, sizeof(j));
		j.kind = JOB_RETRY;   /* nothing to look up: vtConnect */
	}
	submit(&j);
	return 0;
}

BkMigrateKind bk_migrate_kind(int profile)
{
	const BkProfile *to = pf_get(profile), *from = pf_get(g_active_idx);
	if (!to)
		return BK_MIGRATE_RECONNECT;
	/* the same private key and tunnel address, only the peer differs */
	if (from && from->key_id && from->key_id == to->key_id &&
	    from->address == to->address && from->address_prefix == to->address_prefix)
		return BK_MIGRATE_SEAMLESS;
	return BK_MIGRATE_RECONNECT;
}

int bk_migrate(int profile)
{
	Job j;
	if (!ready())
		return -1;
	if (!g_vs_ok || g_vs.state == VT_STATE_IDLE)
		return bk_connect(profile);
	if (job_for(profile, JOB_MIGRATE, &j) != 0)
		return -1;
	j.seamless = bk_migrate_kind(profile) == BK_MIGRATE_SEAMLESS;
	submit(&j);
	return 0;
}

int bk_pause(uint32_t seconds)
{
	if (g_mod != MOD_READY)
		return -1;
	app_log("pause: calling the module");
	int r = vtPause(seconds);
	app_log("pause: module returned 0x%08X", (unsigned)r);
	poll_status();
	app_log("pause: status read, state %u", g_vs_ok ? (unsigned)g_vs.state : 0xFFu);
	if (r == -(int)VT_ERR_LOCKDOWN)
		return BK_PAUSE_LOCKDOWN;
	return r < 0 ? -1 : 0;
}

int bk_resume(void)
{
	if (g_mod != MOD_READY)
		return -1;
	int r = vtResume();
	poll_status();
	return r < 0 ? -1 : 0;
}

void bk_get_options(BkOptions *o)
{
	/* kept by the module; defaults until it answers */
	o->kill_switch = g_vs_ok ? g_vs.kill_switch : 0;
	o->allow_lan = g_vs_ok ? g_vs.allow_local : 1;
	o->lockdown = g_vs_ok && (VT_ST_FEATURES(&g_vs) & VT_FEAT_LOCKDOWN) && VT_ST_LOCKDOWN(&g_vs);
}

int bk_set_options(const BkOptions *o)
{
	if (g_mod != MOD_READY)
		return -1;
	VtOptions v;
	memset(&v, 0, sizeof(v));
	v.size = sizeof(v);
	v.kill_switch = o->kill_switch ? 1 : 0;
	v.allow_local = o->allow_lan ? 1 : 0;
	/* Lockdown is only asked for when it changes, so a change made on the
	 * Settings page meanwhile is not undone by a kill switch toggle */
	if (g_vs_ok && (VT_ST_FEATURES(&g_vs) & VT_FEAT_LOCKDOWN) &&
	    (o->lockdown != 0) != (VT_ST_LOCKDOWN(&g_vs) != 0))
		VT_OPT_LOCKDOWN(&v) = o->lockdown ? VT_LOCKDOWN_ON : VT_LOCKDOWN_OFF;
	int r = vtSetOptions(&v);
	poll_status();
	return r < 0 ? -1 : 0;
}

/* ---- checks -------------------------------------------- */

/* What the tunnel was doing when a check was asked for: a result only
 * stands while that is still so */
enum { CL_DOWN, CL_UP, CL_OTHER };

typedef struct CheckJob {
	uint32_t gen;
	int cls;
	int profile_dns;          /* the profile has DNS = */
	int full_tunnel;
	uint32_t endpoint_ip;
	uint32_t wlan_ip;
	uint8_t wlan_prefix;
	char url[160];
} CheckJob;

/* Shared with the check worker, under g_chk_lock */
static SceUID g_chk_lock = -1, g_chk_wake = -1, g_chk_thread = -1;
static volatile int g_chk_run;
static BkCheck g_chk;           /* what bk_check_get reports */
static CheckJob g_cjob;         /* the latest request */
static uint32_t g_chk_gen;      /* bumped by every request and every drop */
static int g_chk_cls = -1;      /* the class the current result is for */
static uint32_t g_isp_ip;

/* Main thread: the automatic runs (one per connection, one at start) */
static uint64_t g_auto_up_since = ~0ull;
static int g_auto_profile = -2;
static int g_auto_down_done;
static uint64_t g_chk_started;          /* g_now when the running check began */
static uint64_t g_retry_for = ~0ull;    /* the connection the retries below count for */
static int g_retry_n;                    /* automatic retries used on it */
#define CHECK_STUCK_MS 20000            /* a running check older than this has failed */
#define CHECK_RETRIES 5                 /* automatic retries per connection, after */
static const uint64_t k_retry_ms[CHECK_RETRIES] = { 3000, 5000, 15000, 30000, 60000 };   /* each failure */

static void chk_lock(void)
{
	sceKernelLockMutex(g_chk_lock, 1, NULL);
}

static void chk_unlock(void)
{
	sceKernelUnlockMutex(g_chk_lock, 1);
}

static void isp_load(void)
{
	char line[24];
	SceUID fd = sceIoOpen(ISP_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return;
	int n = sceIoRead(fd, line, sizeof(line) - 1);
	sceIoClose(fd);
	line[n > 0 ? n : 0] = 0;
	line[strcspn(line, "\r\n")] = 0;
	g_isp_ip = is_dotted(line) ? parse_ip(line) : 0;
}

/* The address only, nothing else; rewritten when it changes */
/* The app's own log (the exit-IP and DNS checks), one write per line so
 * the main thread and the check worker never split a line; kept to
 * APP_LOG_MAX with one older file. Never a key, a profile line or an
 * address the check found. */
static void app_log(const char *fmt, ...)
{
	char line[256];
	unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
	int n = snprintf(line, sizeof(line), "[app] %u.%03u ", ms / 1000, ms % 1000);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	SceIoStat st;
	if (sceIoGetstat(APP_LOG, &st) >= 0 && st.st_size > APP_LOG_MAX) {
		sceIoRemove(APP_LOG_OLD);
		sceIoRename(APP_LOG, APP_LOG_OLD);
	}
	SceUID fd = sceIoOpen(APP_LOG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, line, (SceSize)n);
	sceIoClose(fd);
}

void bk_trace(const char *what) { app_log("%s", what); }

/* What resolvers are given now, for the log: "profile a.b.c.d", "blocked",
 * "system a.b.c.d" or "unknown" */
static const char *dns_view(char *buf, size_t cap)
{
	VtDns d;
	memset(&d, 0, sizeof(d));
	d.size = sizeof(d);
	if (vtGetDns(&d) < 0 || !(d.flags & VT_DNS_HOOKED))
		snprintf(buf, cap, "unknown");
	else if (d.flags & VT_DNS_BLOCKED)
		snprintf(buf, cap, "blocked");
	else
		snprintf(buf, cap, "%s %u.%u.%u.%u", (d.flags & VT_DNS_OVERRIDE) ? "profile" : "system",
			(unsigned)(d.effective[0] >> 24), (unsigned)(d.effective[0] >> 16 & 255),
			(unsigned)(d.effective[0] >> 8 & 255), (unsigned)(d.effective[0] & 255));
	return buf;
}

static void isp_save(uint32_t ip)
{
	char line[24];
	int n = snprintf(line, sizeof(line), "%u.%u.%u.%u\n",
		(unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
	sceIoMkdir(PLUGIN_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(ISP_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, line, (SceSize)n);
	sceIoClose(fd);
}

/* ---- the service's answer: JSON with "ip", "city", "country" and
 * "organization" at the top level, or the bare address as text ---------- */

static const char *js_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return p;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
		: c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int hex4(const char *p, unsigned *v)
{
	*v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexval(p[i]);
		if (h < 0)
			return -1;
		*v = *v << 4 | (unsigned)h;
	}
	return 0;
}

/* Drop a UTF-8 character cut short at the end of s */
static void utf8_trim(char *s, size_t len)
{
	size_t i = len;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80)
		i--;
	if (i == 0)
		return;
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (len - (i - 1) < need)
		s[i - 1] = 0;
}

/* A JSON string at p (on its quote) into out (n bytes; out may be NULL),
 * cut to fit, control characters as spaces; the byte after it, or NULL */
static const char *js_string(const char *p, char *out, size_t n)
{
	size_t len = 0;
	int full = 0;
	if (*p++ != '"')
		return NULL;
	while (*p && *p != '"') {
		unsigned char b[4];
		size_t bn = 1;
		if (*p == '\\') {
			p++;
			unsigned cp;
			switch (*p) {
			case 'u':
				if (hex4(p + 1, &cp) != 0)
					return NULL;
				p += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
					unsigned lo;
					if (hex4(p + 3, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				if ((cp >= 0xD800 && cp < 0xE000) || cp < 0x20)
					cp = cp < 0x20 ? ' ' : 0xFFFD;
				if (cp < 0x80) {
					b[0] = (unsigned char)cp;
				} else if (cp < 0x800) {
					b[0] = (unsigned char)(0xC0 | cp >> 6);
					b[1] = (unsigned char)(0x80 | (cp & 0x3F));
					bn = 2;
				} else if (cp < 0x10000) {
					b[0] = (unsigned char)(0xE0 | cp >> 12);
					b[1] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
					b[2] = (unsigned char)(0x80 | (cp & 0x3F));
					bn = 3;
				} else {
					b[0] = (unsigned char)(0xF0 | cp >> 18);
					b[1] = (unsigned char)(0x80 | (cp >> 12 & 0x3F));
					b[2] = (unsigned char)(0x80 | (cp >> 6 & 0x3F));
					b[3] = (unsigned char)(0x80 | (cp & 0x3F));
					bn = 4;
				}
				break;
			case 'b': case 'f': case 'n': case 'r': case 't':
				b[0] = ' ';
				break;
			case 0:
				return NULL;
			default:          /* \" \\ \/ */
				b[0] = (unsigned char)*p;
				break;
			}
			p++;
		} else {
			b[0] = (unsigned char)*p++;
			if (b[0] < 0x20)
				b[0] = ' ';
		}
		if (out && !full) {
			if (len + bn < n) {
				memcpy(out + len, b, bn);
				len += bn;
			} else {
				full = 1;
			}
		}
	}
	if (*p != '"')
		return NULL;
	if (out && n) {
		out[len] = 0;
		utf8_trim(out, len);
	}
	return p + 1;
}

/* Past one JSON value of any kind; NULL if it is not one */
static const char *js_skip(const char *p)
{
	if (*p == '"')
		return js_string(p, NULL, 0);
	if (*p == '{' || *p == '[') {
		int depth = 0;
		do {
			if (*p == '"') {
				p = js_string(p, NULL, 0);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']')
				depth--;
			else if (!*p)
				return NULL;
			p++;
		} while (depth > 0);
		return p;
	}
	const char *s = p;
	while (*p && !strchr(",}] \t\r\n", *p))
		p++;
	return p > s ? p : NULL;
}

/* The address on its own, as a plain-text service answers */
static uint32_t plain_ip(const char *body)
{
	char t[24];
	const char *p = js_ws(body);
	size_t n = 0;
	while (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n' && n < sizeof(t) - 1) {
		t[n] = p[n];
		n++;
	}
	t[n] = 0;
	if (*js_ws(p + n))
		return 0;
	return is_dotted(t) ? parse_ip(t) : 0;
}

/* 0 when an IPv4 exit address was found */
static int parse_answer(const char *body, BkCheck *c)
{
	const char *p = js_ws(body);
	if (*p != '{') {
		c->exit_ip = plain_ip(body);
		return c->exit_ip ? 0 : -1;
	}
	char ip[48] = "";
	p = js_ws(p + 1);
	while (*p == '"') {
		char key[16];
		p = js_string(p, key, sizeof(key));
		if (!p)
			return -1;
		p = js_ws(p);
		if (*p++ != ':')
			return -1;
		p = js_ws(p);
		char *dst = NULL;
		size_t dn = 0;
		if (!strcmp(key, "ip"))
			dst = ip, dn = sizeof(ip);
		else if (!strcmp(key, "city"))
			dst = c->city, dn = sizeof(c->city);
		else if (!strcmp(key, "country"))
			dst = c->country, dn = sizeof(c->country);
		else if (!strcmp(key, "organization"))
			dst = c->org, dn = sizeof(c->org);
		p = dst && *p == '"' ? js_string(p, dst, dn) : js_skip(p);
		if (!p)
			return -1;
		p = js_ws(p);
		if (*p == ',')
			p = js_ws(p + 1);
		else
			break;
	}
	c->exit_ip = is_dotted(ip) ? parse_ip(ip) : 0;
	return c->exit_ip ? 0 : -1;
}

/* ---- the DNS servers in use and where they go ---------------------- */

/*
 * Whether ip leaves through the tunnel. vtGetRouteFor answers from the
 * profile's AllowedIPs; two routes the module keeps on Wi-Fi are added
 * here: the endpoint's /32 and the Wi-Fi subnet, which netctl's subnet route
 * keeps direct by being more specific than the tunnel's 0/1 routes
 * (kernel/routes.c).
 */
static int via_tunnel(const CheckJob *j, uint32_t ip)
{
	if (j->cls != CL_UP || g_mod != MOD_READY)
		return 0;
	if (ip == j->endpoint_ip)
		return 0;
	if (j->wlan_prefix && j->wlan_ip) {
		uint32_t mask = j->wlan_prefix >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> j->wlan_prefix);
		if ((ip & mask) == (j->wlan_ip & mask))
			return 0;
	}
	int iface = VT_IFACE_NONE;
	return vtGetRouteFor(ip, &iface) >= 0 && iface == VT_IFACE_TUNNEL;
}

/*
 * The servers are netctl's, from sceNetCtlInetGetInfo. Reading the
 * kernel's effective record instead, which would show
 * the module's own override; the module does not override DNS yet, so
 * the two agree for now. Read the effective record once that lands.
 */
static void dns_check(const CheckJob *j, BkCheck *c)
{
	static const int which[2] = { SCE_NETCTL_INFO_GET_PRIMARY_DNS, SCE_NETCTL_INFO_GET_SECONDARY_DNS };
	int any_wifi = 0;
	c->local_count = 0;
	/* The module's effective record when its DNS hook is in place:
	 * what every resolver is given right now, override included */
	VtDns d;
	memset(&d, 0, sizeof(d));
	d.size = sizeof(d);
	int hooked = vtGetDns(&d) >= 0 && (d.flags & VT_DNS_HOOKED);
	for (int i = 0; i < 2; i++) {
		uint32_t ip;
		if (hooked) {
			ip = d.effective[i];
		} else {
			SceNetCtlInfo info;
			memset(&info, 0, sizeof(info));
			if (sceNetCtlInetGetInfo(which[i], &info) < 0)
				continue;
			const char *s = i ? info.secondary_dns : info.primary_dns;
			ip = is_dotted(s) ? parse_ip(s) : 0;
		}
		if (!ip || (c->local_count && c->local[0].ip == ip))
			continue;
		int t = via_tunnel(j, ip);
		c->local[c->local_count].ip = ip;
		c->local[c->local_count].via_tunnel = t;
		c->local_count++;
		any_wifi |= !t;
	}
	if (j->cls != CL_UP || !j->profile_dns)
		c->dns = BK_DNS_UNMANAGED;
	else if (!any_wifi)
		c->dns = BK_DNS_OK;
	else
		c->dns = j->full_tunnel ? BK_DNS_LEAK : BK_DNS_SPLIT;
}

/* ---- the worker --------------------------------------------------------- */

static BkError map_hg(HgResult r)
{
	switch (r) {
	case HG_RESOLVE:
		return BK_ERR_RESOLVE;
	case HG_CERT_TIME:
		return BK_ERR_CLOCK;
	default:
		return BK_ERR_SOCKET;   /* "Network error" */
	}
}

static void run_check(const CheckJob *j)
{
	static char body[4096];
	BkCheck r;
	memset(&r, 0, sizeof(r));
	r.state = BK_CHECK_DONE;
	dns_check(j, &r);

	uint32_t peer = 0;
	HgResult h = HG_OK;
	if (!j->wlan_ip) {
		r.state = BK_CHECK_FAILED;
		r.fail_reason = BK_ERR_WIFI_OFF;
	} else if (time(NULL) < (time_t)CLOCK_FLOOR) {
		r.state = BK_CHECK_FAILED;     /* no certificate is valid at this date */
		r.fail_reason = BK_ERR_CLOCK;
	} else if ((h = hg_get(j->url, body, sizeof(body), &peer, CHECK_TIMEOUT_MS)) != HG_OK) {
		r.state = BK_CHECK_FAILED;
		r.fail_reason = map_hg(h);
	} else if (parse_answer(body, &r) != 0) {
		r.state = BK_CHECK_FAILED;
		r.fail_reason = BK_ERR_SOCKET;
	}
	vtc_wipe(body, sizeof(body));
	{
		char dv[40];
		app_log("check %u %s: reason %d, https %d, lookup 0x%08X, dns %s",
			(unsigned)j->gen, r.state == BK_CHECK_DONE ? "done" : "FAILED", (int)r.fail_reason,
			(int)h, (unsigned)hg_lookup_err, dns_view(dv, sizeof(dv)));
	}

	int save = 0;
	chk_lock();
	if (j->gen == g_chk_gen) {
		if (r.state == BK_CHECK_DONE && j->cls == CL_DOWN && r.exit_ip != g_isp_ip) {
			g_isp_ip = r.exit_ip;
			save = 1;
		}
		r.isp_ip = g_isp_ip;
		g_chk = r;
	}
	chk_unlock();
	/* the HUD's EXIT: line */
	if (r.state == BK_CHECK_DONE)
		vtSetExitIp(j->cls == CL_UP ? r.exit_ip : 0);
	if (save)
		isp_save(r.exit_ip);
}

static int check_worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	while (g_chk_run) {
		if (sceKernelWaitSema(g_chk_wake, 1, NULL) < 0 || !g_chk_run)
			break;
		chk_lock();
		CheckJob j = g_cjob;
		chk_unlock();
		if (j.gen == g_chk_gen)       /* not dropped while it waited */
			run_check(&j);
	}
	return sceKernelExitDeleteThread(0);
}

static void check_worker_start(void)
{
	g_chk_lock = sceKernelCreateMutex("vpnturbo_chk_lock", 0, 0, NULL);
	g_chk_wake = sceKernelCreateSema("vpnturbo_chk_wake", 0, 0, 1, NULL);
	if (g_chk_lock < 0 || g_chk_wake < 0)
		return;
	g_chk_run = 1;
	/* TLS needs a deeper stack than the connect worker */
	g_chk_thread = sceKernelCreateThread("vpnturbo_check", check_worker, 0x10000100, 0x40000, 0, 0, NULL);
	if (g_chk_thread < 0 || sceKernelStartThread(g_chk_thread, 0, NULL) < 0) {
		g_chk_run = 0;
		g_chk_thread = -1;
	}
}

static void check_worker_stop(void)
{
	if (g_chk_thread < 0)
		return;
	/* a check in flight ends within its timeout; quitting does not wait
	 * that long for it */
	SceUInt wait_us = 1000000;
	g_chk_run = 0;
	sceKernelSignalSema(g_chk_wake, 1);
	sceKernelWaitThreadEnd(g_chk_thread, NULL, &wait_us);
	g_chk_thread = -1;
}

/* Main thread: up, down (idle or paused) or in between, as the screens see it */
static int tunnel_class(void)
{
	if (g_mod != MOD_READY || !g_vs_ok)
		return CL_DOWN;
	int pending = 0;
	if (g_lock >= 0) {
		lock();
		pending = g_pending;
		unlock();
	}
	if (pending)
		return CL_OTHER;
	switch (g_vs.state) {
	case VT_STATE_IDLE:
	case VT_STATE_PAUSED:
		return CL_DOWN;
	case VT_STATE_UP:
		return CL_UP;
	default:
		return CL_OTHER;
	}
}

/* Main thread: a new check for the tunnel as it is now */
static void check_submit(int cls, const char *why)
{
	if (g_chk_thread < 0)
		return;
	CheckJob j;
	memset(&j, 0, sizeof(j));
	j.cls = cls;
	j.endpoint_ip = g_vs_ok ? g_vs.endpoint_ip : 0;
	j.wlan_ip = g_wlan_ip;
	j.wlan_prefix = g_wlan_prefix;
	/* the active profile's DNS and its own check URL, if it has one */
	const BkProfile *p = pf_get(g_active_idx >= 0 ? g_active_idx : source_index(&g_active));
	if (p) {
		j.profile_dns = p->dns[0] != 0;
		j.full_tunnel = p->full_tunnel;
	}
	copy_str(j.url, sizeof(j.url),
		p && p->check == BK_CHECK_URL && p->check_url[0] ? p->check_url : DEFAULT_CHECK_URL);

	chk_lock();
	j.gen = ++g_chk_gen;
	g_cjob = j;
	g_chk_cls = cls;
	memset(&g_chk, 0, sizeof(g_chk));
	g_chk.state = BK_CHECK_RUNNING;
	g_chk.isp_ip = g_isp_ip;
	g_chk_started = g_now;
	chk_unlock();
	char dv[40];
	app_log("check %u start (%s): %s, state %u hold %u routes 0x%02X, dns %s, %s url",
		(unsigned)j.gen, why, cls == CL_UP ? "up" : cls == CL_DOWN ? "down" : "other",
		g_vs_ok ? (unsigned)g_vs.state : 99u, g_vs_ok ? (unsigned)g_vs.hold : 0u,
		g_vs_ok ? (unsigned)g_vs.route_flags : 0u, dns_view(dv, sizeof(dv)),
		p && p->check == BK_CHECK_URL && p->check_url[0] ? "profile" : "default");
	sceKernelSignalSema(g_chk_wake, 1);
}

/* Whether resolvers are given a server right now: not while the module
 * blocks them (the lock after a wake, a hold). An older module without
 * the DNS call counts as ready. */
static int dns_ready(void)
{
	VtDns d;
	memset(&d, 0, sizeof(d));
	d.size = sizeof(d);
	if (vtGetDns(&d) < 0 || !(d.flags & VT_DNS_HOOKED))
		return 1;
	return !(d.flags & VT_DNS_BLOCKED);
}

/* Main thread, every tick: drop a result the tunnel has moved away from,
 * and start the automatic runs */
static void check_tick(void)
{
	if (g_chk_thread < 0)
		return;
	int cls = tunnel_class();
	chk_lock();
	if (g_chk.state != BK_CHECK_NOT_RUN && cls != g_chk_cls) {
		g_chk_gen++;              /* a request in flight is dropped too */
		memset(&g_chk, 0, sizeof(g_chk));
		g_chk.isp_ip = g_isp_ip;
		g_chk_cls = -1;
	} else if (g_chk.state == BK_CHECK_RUNNING && g_now - g_chk_started > CHECK_STUCK_MS) {
		/* a lookup or connection that never answers: stop saying Checking */
		g_chk_gen++;
		g_chk.state = BK_CHECK_FAILED;
		g_chk.fail_reason = BK_ERR_SOCKET;
	}
	int failed_up = g_chk.state == BK_CHECK_FAILED && g_chk_cls == CL_UP;
	chk_unlock();
	if (!g_wlan_ip)
		return;
	/* The tunnel's routes (and its DNS) are in place a moment after the
	 * handshake; a check sent before them leaves over Wi-Fi and its lookup
	 * goes to the profile's DNS server, which only answers inside the tunnel */
	int routed = g_vs_ok && ((g_vs.route_flags & VT_RT_TUNNEL) || secs_since(g_up_since) >= 5);
	if (g_retry_for != g_up_since) {
		g_retry_for = g_up_since;
		g_retry_n = 0;
	}
	{
		/* the conditions the automatic runs wait for, logged when they change */
		static int last = -1;
		int now = cls | routed << 4 | (cls == CL_UP ? dns_ready() : 1) << 5 | failed_up << 6;
		if (now != last) {
			last = now;
			app_log("check gate: %s, routed %d, dns ready %d, last failed %d (state %u hold %u)",
				cls == CL_UP ? "up" : cls == CL_DOWN ? "down" : "other", routed, now >> 5 & 1,
				failed_up, g_vs_ok ? (unsigned)g_vs.state : 99u, g_vs_ok ? (unsigned)g_vs.hold : 0u);
		}
		if (cls == CL_UP && routed && !(now >> 5 & 1))
			return;   /* after a wake the resolvers are still blocked for a moment */
	}
	if (cls == CL_UP && routed && failed_up && g_retry_n < CHECK_RETRIES &&
	    g_now - g_chk_started > k_retry_ms[g_retry_n]) {
		g_retry_n++;                   /* a few automatic retries per connection */
		check_submit(cls, g_retry_n == 1 ? "retry 1" : g_retry_n == 2 ? "retry 2" :
			g_retry_n == 3 ? "retry 3" : g_retry_n == 4 ? "retry 4" : "retry 5");
	} else if (cls == CL_UP && routed && secs_since(g_up_since) >= 1 &&
	    (g_up_since != g_auto_up_since || g_active_idx != g_auto_profile)) {
		/* once per connection, and again after a migrate */
		g_auto_up_since = g_up_since;
		g_auto_profile = g_active_idx;
		check_submit(cls, "up");
	} else if (cls == CL_DOWN && !g_auto_down_done) {
		/* once per run, to learn the ISP's address */
		g_auto_down_done = 1;
		check_submit(cls, "down");
	}
}

void bk_check_start(void)
{
	if (g_chk_thread < 0)
		return;
	chk_lock();
	int running = g_chk.state == BK_CHECK_RUNNING;
	chk_unlock();
	if (!running)
		check_submit(tunnel_class(), "asked");
}

void bk_check_get(BkCheck *out)
{
	if (g_chk_lock < 0) {
		memset(out, 0, sizeof(*out));
		return;
	}
	chk_lock();
	*out = g_chk;
	chk_unlock();
}

/* ---- no scenarios outside the mock ------------------------------------- */

int bk_scenario_count(void)
{
	return 0;
}

int bk_scenario_get(void)
{
	return 0;
}

void bk_scenario_set(int index)
{
	(void)index;
}

const char *bk_scenario_name(int index)
{
	(void)index;
	return "";
}

int bk_mock_profiles_from(const char *host_dir)
{
	(void)host_dir;
	return -1;
}

