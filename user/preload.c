/*
 * preload.c - at boot the SceShell plugin loads the last-used profile
 * into the kernel module without connecting, so the Settings and quick-menu
 * toggles have something to connect straight away. The tunnel still starts
 * off after a reboot.
 *
 * The app records the *path* of the last profile it connected (path only,
 * never keys) in VTW_LAST_PROFILE. This reads that path, parses the .conf
 * with the shared parser (conf/), builds a VtProfile and loads it
 * with vtSetProfile. The parse buffer and the VtProfile are wiped afterwards;
 * the kernel keeps keys write-only. Nothing here connects.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <string.h>

#include "shell.h"
#include "vt_client.h"
#include "log.h"
#include "resolve.h"
#include "../conf/vtconf.h"
#include "vt_paths.h"

#define VTW_LAST_PROFILE VT_DATA_DIR "/last_profile"
#define PATH_MAX_LEN     256
#define CONF_MAX         8192

static int
read_file(const char *path, char *buf, int cap)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	int n;

	if (fd < 0)
		return -1;
	n = sceIoRead(fd, buf, cap);
	sceIoClose(fd);
	return n;
}

/* The base file name (after the last '/' or ':'), ".conf" trimmed by the
 * parser, for the profile's display name. */
static const char *
basename_of(const char *path)
{
	const char *p = path, *b = path;

	for (; *p; p++)
		if (*p == '/' || *p == ':')
			b = p + 1;
	return b;
}

/* Parse the profile in `conf` (CONF_MAX bytes, wiped by the caller) and load
 * it. */
static void
preload_file(const char *path, char *conf)
{
	VtcResult res;
	VtProfile prof;
	int n;

	n = read_file(path, conf, CONF_MAX);
	if (n <= 0) {
		VTW_LOG("preload: cannot read last profile file");
		return;
	}

	if (vtc_parse(conf, (size_t)n, &res) != VTC_OK) {
		VTW_LOG("preload: parse error %d (line %u)", res.error, (unsigned)res.error_line);
		vtc_wipe(&res, sizeof(res));
		return;
	}
	if (vtc_to_profile(&res, basename_of(path), &prof) != 0) {
		vtc_wipe(&res, sizeof(res));
		return;
	}

	/* A hostname endpoint is loaded with address 0; the last address it
	 * resolved to, when cached for this profile, is handed over at once
	 * (Lockdown after a reboot may leave DNS unreachable), and the worker
	 * still resolves it once the network is up (resolve.c). */
	vtw_endpoint_set_host(path, res.endpoint_is_ip ? "" : res.endpoint_host, res.endpoint_port);
	{
		int rc = vtc_available() == VTC_READY ? vtSetProfile(&prof) : -1;
		VTW_LOG("preload: loaded '%s' (rc 0x%08X), not connecting", prof.name, (unsigned)rc);
		if (rc >= 0 && !res.endpoint_is_ip)
			vtw_endpoint_use_cache();
		else if (rc >= 0)
			VTW_LOG("endpoint: source profile (address literal)");
	}

	vtc_wipe(&prof, sizeof(prof));
	vtc_wipe(&res, sizeof(res));
}

void
vtw_preload_last(void)
{
	char path[PATH_MAX_LEN];
	SceUID blk;
	void *conf = NULL;
	int n, i;

	if (vtc_is_stub())
		return;

	n = read_file(VTW_LAST_PROFILE, path, sizeof(path) - 1);
	if (n <= 0) {
		VTW_LOG("preload: no last profile recorded");
		return;
	}
	path[n] = 0;
	for (i = 0; i < n; i++)   /* one line, cut at CR/LF (names may hold spaces) */
		if (path[i] == '\r' || path[i] == '\n') {
			path[i] = 0;
			break;
		}
	for (i = (int)strlen(path); i > 0 && path[i - 1] == ' '; i--)
		path[i - 1] = 0;
	if (path[0] == 0)
		return;
	vt_path_current(path, sizeof(path));   /* saved under a former folder name */

	/* The parse buffer lives only for this call: a memblock, freed (and
	 * wiped first) here, not 8 KB of SceShell's data kept for ever. The
	 * worker thread runs this, so its small stack never holds the file. */
	blk = sceKernelAllocMemBlock("VtwPreload", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, CONF_MAX, NULL);
	if (blk < 0 || sceKernelGetMemBlockBase(blk, &conf) < 0) {
		VTW_LOG("preload: buffer failed 0x%08X", (unsigned)blk);
		if (blk >= 0)
			sceKernelFreeMemBlock(blk);
		return;
	}
	preload_file(path, (char *)conf);
	vtc_wipe(conf, CONF_MAX);
	sceKernelFreeMemBlock(blk);
}
