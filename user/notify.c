/*
 * notify.c - system notification for a tunnel drop.
 *
 * Uses the system's notification library (sceNotificationUtilSendNotification),
 * which SceShell does not load by itself. It is loaded only when the first
 * notification is due, long after boot: through the system module loader,
 * or from vs0: if that is refused. The import is weak and the call is made
 * only after the library is in the process.
 *
 * Sending is a request to the shell, so it runs on its own thread: if it
 * ever blocks, only notifications stop, not the drop watcher.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/notificationutil.h>
#include <psp2/sysmodule.h>
#include <taihen.h>

#include "notify.h"
#include "i18n.h"
#include "log.h"

#define NOTIF_MODULE_PATH "vs0:sys/external/notification_util.suprx"

static SceUID g_sema = -1;
static SceUID g_thread = -1;
static volatile int g_run = 0;

/* Sending a system notification from SceShell crashed it
 * (3.60, right after the library loaded). Off until a working
 * method is found; build with -DVTW_NOTIFY=1 to try one. */
#ifndef VTW_NOTIFY
#define VTW_NOTIFY 0
#endif

#if VTW_NOTIFY
static int g_ready = 0;
/* The only use of the string table in SceShell's module: compiled only when
 * notifications are on, so the table stays out of SceShell otherwise. */
static int lib_loaded(void)
{
	tai_module_info_t ti;
	ti.size = sizeof(ti);
	return taiGetModuleInfo("SceNotificationUtil", &ti) >= 0;
}

static void load_lib(void)
{
	if (lib_loaded()) {
		g_ready = 1;
		return;
	}
	int r = sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL);
	if (r < 0 || !lib_loaded()) {
		SceUID m = sceKernelLoadStartModule(NOTIF_MODULE_PATH, 0, NULL, 0, NULL, NULL);
		VTW_LOG("notify: sysmodule 0x%08X, direct load 0x%08X", (unsigned)r, (unsigned)m);
	}
	g_ready = lib_loaded();
	VTW_LOG("notify: library %s", g_ready ? "ready" : "NOT available");
}

static int sender(SceSize args, void *argp)
{
	(void)args; (void)argp;
	int tried = 0;
	while (g_run) {
		if (sceKernelWaitSema(g_sema, 1, NULL) < 0 || !g_run)
			break;
		if (!tried) {
			load_lib();
			tried = 1;
		}
		if (!g_ready)
			continue;
		SceWChar16 text[128];
		vtw_to_u16(vtw_i18n(VTW_I18N_NOTE_DROP), text, 128);
		int r = sceNotificationUtilSendNotification(text);
		VTW_LOG("notify: drop notification sent, rc 0x%08X", (unsigned)r);
	}
	return 0;
}


#endif /* VTW_NOTIFY */

void vtw_notify_init(void)
{
	if (!VTW_NOTIFY)
		return;
	g_sema = sceKernelCreateSema("VtwNotify", 0, 0, 8, NULL);
	if (g_sema < 0) {
		VTW_LOG("notify: semaphore failed 0x%08X", (unsigned)g_sema);
		return;
	}
	g_run = 1;
#if VTW_NOTIFY
	g_thread = sceKernelCreateThread("VtwNotify", sender, 0x60, 0x2000, 0, 0, NULL);
#endif
	if (g_thread < 0 || sceKernelStartThread(g_thread, 0, NULL) < 0) {
		VTW_LOG("notify: thread failed 0x%08X", (unsigned)g_thread);
		g_run = 0;
	}
}

void vtw_notify_drop(void)
{
	if (!VTW_NOTIFY) {
		VTW_LOG("drop: notification disabled in this build");
		return;
	}
	if (g_run && g_sema >= 0)
		sceKernelSignalSema(g_sema, 1);
}

void vtw_notify_fini(void)
{
	g_run = 0;
	if (g_sema >= 0) {
		sceKernelSignalSema(g_sema, 1);
		if (g_thread >= 0) {
			sceKernelWaitThreadEnd(g_thread, NULL, NULL);
			sceKernelDeleteThread(g_thread);
			g_thread = -1;
		}
		sceKernelDeleteSema(g_sema);
		g_sema = -1;
	}
}
