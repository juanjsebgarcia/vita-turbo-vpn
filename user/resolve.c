/*
 * resolve.c - see resolve.h.
 *
 * The parser leaves a hostname endpoint's address at 0 for the caller to
 * fill (conf/). The SceShell plugin is the caller that is always
 * running, so it resolves through the system resolver once the network is
 * up and passes the address to the kernel with vtSetEndpoint, whichever
 * surface then connects (app, Settings or quick menu).
 *
 * The lookup runs on its own low-priority thread, never on the worker: a
 * resolver call can wait for its whole timeout (no answer, or no servers
 * while the kernel holds traffic), and the worker must keep polling the
 * status and the interface address. The worker only raises a request flag.
 * No lookup is started while the tunnel is LOCKED (the kernel gives
 * resolvers no servers then, so it could only time out). While traffic is
 * otherwise held (kill switch, Lockdown mode) the kernel gives every
 * resolver no server or the profile's, except this thread, which registers
 * itself (vtSetEndpoint(0, VT_EP_LOOKUP_THREAD)) and keeps the system's
 * servers for the endpoint lookup (kernel/dns.c, lookup_exempt). Each lookup
 * has a short timeout (1 s per try, 2 tries; microseconds, the unit Sony's
 * own NP signalling passes as 1000000).
 *
 * Endpoint cache (Lockdown after a reboot): with
 * Lockdown on and the tunnel off, a resolver that is not on the LAN cannot
 * be reached, so a hostname endpoint could never be looked up and the tunnel
 * never started. Every successful lookup is therefore remembered in
 * VTW_ENDPOINT_CACHE, one line:
 *
 *     v1 <key> <a.b.c.d> <port>
 *
 * where <key> is a 64-bit FNV-1a hash of the profile's path and the
 * hostname (neither is written out; no keys, nothing else from the file).
 * At boot the preload (preload.c) hands the cached address to the kernel
 * when the key and port match the profile it loaded, so a connect works
 * without DNS; a fresh lookup is still made (every 30 s until one
 * succeeds) and, when the name now points elsewhere, the cache is updated
 * and so is the kernel, unless the tunnel is already up on the cached
 * address (then the working session is left alone and the next boot uses
 * the new address). A refresh only runs while the kernel still holds the
 * cached address, so a profile loaded since by the app is never touched.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <string.h>

#include "resolve.h"
#include "log.h"
#include "vt_client.h"
#include "vt_paths.h"

#define RETRY_US       (5ull * 1000 * 1000)
#define REFRESH_US     (30ull * 1000 * 1000)
#define LOOKUP_TMO_US  (1000 * 1000)
#define LOOKUP_RETRIES 2

#ifndef VTW_DATA_DIR
#define VTW_DATA_DIR       VT_DATA_DIR
#endif
#define VTW_ENDPOINT_CACHE VTW_DATA_DIR "/endpoint_cache"

static char g_host[256];
static uint16_t g_port;
static uint64_t g_key;              /* hash of profile path + host */
static SceUInt64 g_next;
static unsigned g_fails;

static uint32_t g_cached_ip;        /* the cached address now in the kernel, or 0 */
static uint32_t g_file_ip;          /* what the cache file holds for g_key, or 0 */
static volatile int g_refresh;      /* g_cached_ip in use: still look the name up */

static volatile int g_req;          /* worker -> lookup thread: look it up now */
static volatile int g_busy;         /* a lookup is running */
static SceUID g_thread = -1;

/* ---- cache file ----------------------------------------------------------- */

static uint64_t fnv1a(uint64_t h, const char *s)
{
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 0x100000001b3ull;
	}
	return h;
}

static uint64_t key_of(const char *path, const char *host)
{
	uint64_t h = 0xcbf29ce484222325ull;
	h = fnv1a(h, path);
	h = fnv1a(h, "\n");
	return fnv1a(h, host);
}

static int put_u(char *p, unsigned v)
{
	char t[10];
	int n = 0, k = 0;
	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		p[k++] = t[--n];
	return k;
}

static const char *get_u(const char *p, unsigned max, unsigned *out)
{
	unsigned v = 0;
	const char *s = p;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (unsigned)(*p - '0');
		if (v > max || p - s > 5)
			return NULL;
		p++;
	}
	if (p == s)
		return NULL;
	*out = v;
	return p;
}

static void cache_write(uint32_t ip, uint16_t port)
{
	static const char hex[] = "0123456789abcdef";
	char line[64];
	int n = 0;

	memcpy(line, "v1 ", 3);
	n = 3;
	for (int i = 60; i >= 0; i -= 4)
		line[n++] = hex[(g_key >> i) & 0xf];
	line[n++] = ' ';
	for (int i = 3; i >= 0; i--) {
		n += put_u(line + n, (ip >> (i * 8)) & 0xff);
		line[n++] = i ? '.' : ' ';
	}
	n += put_u(line + n, port);
	line[n++] = '\n';

	sceIoMkdir(VTW_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(VTW_ENDPOINT_CACHE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	int w = fd;
	if (fd >= 0) {
		w = sceIoWrite(fd, line, n);
		sceIoClose(fd);
	}
	if (w == n)
		g_file_ip = ip;
	VTW_LOG("endpoint: cache %s", w == n ? "written" : "NOT written");
}

/* The cached address for the current key and port, or 0 (no file, another
 * profile or host, another port, or a damaged line). */
static uint32_t cache_read(void)
{
	char line[64];
	SceUID fd = sceIoOpen(VTW_ENDPOINT_CACHE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return 0;
	int n = sceIoRead(fd, line, sizeof(line) - 1);
	sceIoClose(fd);
	if (n < 3 + 16 + 1)
		return 0;
	line[n] = 0;
	if (memcmp(line, "v1 ", 3))
		return 0;

	uint64_t key = 0;
	const char *p = line + 3;
	for (int i = 0; i < 16; i++, p++) {
		int d = *p >= '0' && *p <= '9' ? *p - '0' :
		        *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
		if (d < 0)
			return 0;
		key = key << 4 | (uint64_t)d;
	}
	if (*p++ != ' ' || key != g_key)
		return 0;

	uint32_t ip = 0;
	for (int i = 0; i < 4; i++) {
		unsigned b;
		if (!(p = get_u(p, 255, &b)) || *p++ != (i < 3 ? '.' : ' '))
			return 0;
		ip = ip << 8 | b;
	}
	unsigned port;
	if (!(p = get_u(p, 65535, &port)) || (*p && *p != '\n' && *p != '\r'))
		return 0;
	if (port != g_port || ip == 0)
		return 0;
	return ip;
}

/* ---- host and lookups ----------------------------------------------------- */

void vtw_endpoint_set_host(const char *path, const char *host, uint16_t port)
{
	size_t n = host ? strlen(host) : 0;
	if (n >= sizeof(g_host))
		n = 0;
	memcpy(g_host, host, n);
	g_host[n] = 0;
	g_port = port;
	g_key = n ? key_of(path ? path : "", g_host) : 0;
	g_next = 0;
	g_fails = 0;
	g_cached_ip = 0;
	g_file_ip = 0;
	g_refresh = 0;
}

int vtw_endpoint_use_cache(void)
{
	if (!g_host[0])
		return 0;
	uint32_t ip = cache_read();
	if (!ip) {
		VTW_LOG("endpoint: no cached address for this profile, waiting for a lookup");
		return 0;
	}
	g_file_ip = ip;
	int k = vtSetEndpoint(ip, g_port);
	if (k < 0) {
		VTW_LOG("endpoint: cached address not taken -> 0x%08X", (unsigned)k);
		return 0;
	}
	g_cached_ip = ip;
	g_refresh = 1;
	VTW_LOG("endpoint: source cache (set -> 0x%08X), a fresh lookup follows", (unsigned)k);
	return 1;
}

static void lookup(void)
{
	SceNetInAddr addr;
	memset(&addr, 0, sizeof(addr));
	int rid = sceNetResolverCreate("vpnturbo_resolver", NULL, 0);
	int r = rid;
	if (rid >= 0) {
		r = sceNetResolverStartNtoa(rid, g_host, &addr, LOOKUP_TMO_US, LOOKUP_RETRIES, 0);
		sceNetResolverDestroy(rid);
	}
	if (r < 0 || addr.s_addr == 0) {
		if (g_fails++ < 5)
			VTW_LOG("endpoint: lookup failed 0x%08X, retrying%s", (unsigned)r,
			        g_refresh ? " (cached address in use)" : "");
		return;
	}
	uint32_t ip = __builtin_bswap32(addr.s_addr);   /* network to host order */
	g_fails = 0;

	if (g_refresh) {
		/* The kernel runs on the cached address: refresh once. */
		g_refresh = 0;
		VtStatus st;
		int rc = vtc_get_status(&st);
		if (ip == g_cached_ip) {
			VTW_LOG("endpoint: fresh lookup matches the cached address");
			return;
		}
		if (ip != g_file_ip)
			cache_write(ip, g_port);
		if (rc == 0 && st.endpoint_ip != g_cached_ip) {
			VTW_LOG("endpoint: fresh lookup differs; the kernel has another address now, left alone");
			return;
		}
		if (rc == 0 && st.state == VT_STATE_UP) {
			VTW_LOG("endpoint: fresh lookup differs; connected on the cached address, "
			        "kept for this session (cache updated)");
			return;
		}
		int k = vtSetEndpoint(ip, g_port);
		VTW_LOG("endpoint: source fresh lookup (differs from cache), set -> 0x%08X", (unsigned)k);
		g_cached_ip = 0;
		return;
	}

	int k = vtSetEndpoint(ip, g_port);
	VTW_LOG("endpoint: source lookup, set -> 0x%08X", (unsigned)k);
	if (k >= 0 && ip != g_file_ip)
		cache_write(ip, g_port);
}

static int lookup_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	for (;;) {
		sceKernelDelayThread(250 * 1000);
		if (!g_req)
			continue;
		g_req = 0;
		g_busy = 1;
		/* This thread does the endpoint lookups: while the kernel holds
		 * traffic (kill switch, Lockdown mode; never while LOCKED) its
		 * reads of the DNS record still get the system's servers, every
		 * other thread none (kernel/dns.c). Repeated before each lookup
		 * (the kernel only logs a change); an older module refuses it. */
		vtSetEndpoint(0, VT_EP_LOOKUP_THREAD);
		lookup();
		g_busy = 0;
	}
	return 0;
}

void vtw_endpoint_poll(uint32_t kernel_endpoint_ip, uint32_t state)
{
	if (!g_host[0] || state == VT_STATE_LOCKED)
		return;
	/* With an address in the kernel, look up only to refresh our cached
	 * one, and only while it is still the kernel's. */
	int refresh = 0;
	if (kernel_endpoint_ip) {
		if (!g_refresh || kernel_endpoint_ip != g_cached_ip)
			return;
		refresh = 1;
	}
	if (g_busy || g_req)
		return;
	SceUInt64 now = sceKernelGetProcessTimeWide();
	if (now < g_next)
		return;
	g_next = now + (refresh ? REFRESH_US : RETRY_US);
	if (g_thread < 0) {
		g_thread = sceKernelCreateThread("VtwResolve", lookup_thread, 0xA0, 0x2000, 0, 0, NULL);
		if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
			VTW_LOG("endpoint: lookup thread failed 0x%08X", (unsigned)g_thread);
			if (g_thread >= 0)
				sceKernelDeleteThread(g_thread);
			g_thread = -1;
			return;
		}
	}
	g_req = 1;
}
