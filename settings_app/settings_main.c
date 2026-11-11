/*
 * settings_main.c - vpnturbo_settings.suprx: the Settings → Network → VPN
 * Settings page, loaded into the Settings app only (*NPXS10015 in
 * config.txt). A module of its own, as in Wi-Fi Turbo and Turbo Storage, so
 * SceShell never holds the page's code or its strings.
 *
 * Every system library is a weak import and so is the kernel module's
 * syscall library: the page loads even when the kernel module is missing,
 * and says so. taiHEN is the only strong import.
 */
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "settings.h"
#include "vpnturbo.h"
#include "log.h"

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	vtw_log_set_tag("settings");
	VTW_LOG("=== vpnturbo_settings %s ===", VT_VERSION_STR);
	vtw_settings_install();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	vtw_settings_release();
	return SCE_KERNEL_STOP_SUCCESS;
}
