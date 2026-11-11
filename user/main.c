/*
 * main.c - vpnturbo.suprx.
 *
 * SceShell's module, listed under *main in config.txt: lent transport
 * threads and the drop alert (shell.c), all on a worker thread that starts
 * its work 10 s after boot. The Settings page is its own module,
 * vpnturbo_settings.suprx under *NPXS10015 (settings_app/), as in Wi-Fi
 * Turbo and Turbo Storage, so SceShell never holds the page's code or its
 * 20-language strings. Loaded anywhere else, this module does nothing.
 *
 * Boot-loop guard: in SceShell the plugin counts its starts in GUARD_PATH
 * and clears the count after GUARD_HEALTHY_US of uptime. If it finds two
 * starts that never got that far, it stays inert for this boot, so a crash
 * of ours during start-up cannot keep the console restarting. Deleting the
 * file re-enables it.
 *
 * In SceShell, module_start does only three things: the log banner, the boot
 * guard (one small file) and starting the worker. Nothing here may block
 * during SceShell's boot. The quick-menu rows are a separate module,
 * vpnturbo_qm.suprx, that QuickMenuReborn loads (quickmenu.c).
 *
 * Every system library is a weak import and so is the kernel module's
 * syscall library: the plugin loads even when the kernel module is missing,
 * and does not call it until it has checked that it is there. taiHEN is
 * the only strong import.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "shell.h"
#include "vt_client.h"
#include "log.h"
#include "vt_paths.h"

#define GUARD_DIR        VT_DATA_DIR
#define GUARD_PATH       GUARD_DIR "/bootguard"
#define GUARD_LIMIT      2
#define GUARD_HEALTHY_US (90 * 1000 * 1000)

static int g_inert;

static int guard_read(void)
{
	char c = 0;
	SceUID fd = sceIoOpen(GUARD_PATH, SCE_O_RDONLY, 0);
	if (fd < 0)
		return 0;
	int n = sceIoRead(fd, &c, 1);
	sceIoClose(fd);
	return n == 1 && c >= '0' && c <= '9' ? c - '0' : 0;
}

static void guard_write(int count)
{
	char c = (char)('0' + (count > 9 ? 9 : count));
	sceIoMkdir(GUARD_DIR, 0777);
	/* The record is one byte, overwritten in place at offset 0, never
	 * truncated: a crash between a truncate and the write would leave an
	 * empty file, which guard_read takes for "no guard" and so lets a start
	 * that crashed count as a clean one. Once the file exists it is never
	 * empty. */
	SceUID fd = sceIoOpen(GUARD_PATH, SCE_O_WRONLY | SCE_O_CREAT, 0666);
	if (fd < 0)
		return;
	sceIoWrite(fd, &c, 1);
	sceIoClose(fd);
}

static int guard_clear_thread(SceSize argc, void *argp)
{
	(void)argc; (void)argp;
	sceKernelDelayThread(GUARD_HEALTHY_US);
	guard_write(0);
	VTW_LOG("boot guard cleared");
	return sceKernelExitDeleteThread(0);
}

/* Returns 1 when the plugin may start in SceShell this boot. */
static int guard_enter(void)
{
	int count = guard_read();
	if (count >= GUARD_LIMIT) {
		VTW_LOG("boot guard: %d starts did not finish; inert until " GUARD_PATH
		        " is deleted", count);
		return 0;
	}
	guard_write(count + 1);
	SceUID th = sceKernelCreateThread("vtw_guard", guard_clear_thread, 0xA0, 0x1000, 0, 0, NULL);
	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);
	return 1;
}

static int in_shell(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	return taiGetModuleInfo("SceShell", &ti) >= 0;
}

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	vtw_log_set_tag("shell");
	if (!in_shell()) {
		/* SceShell's module only: the Settings page is its own module,
		 * vpnturbo_settings.suprx (*NPXS10015). */
		g_inert = 1;
		return SCE_KERNEL_START_SUCCESS;
	}
	VTW_LOG("=== vpnturbo %s%s ===", VT_VERSION_STR,
	        vtc_is_stub() ? " (test build, scripted states)" : "");

	if (!guard_enter()) {
		g_inert = 1;
		return SCE_KERNEL_START_SUCCESS;
	}
	vtw_shell_start();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (g_inert)
		return SCE_KERNEL_STOP_SUCCESS;
	vtw_shell_stop();
	return SCE_KERNEL_STOP_SUCCESS;
}
