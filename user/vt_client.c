/*
 * vt_client.c - vtc_* over the kernel module's syscall library.
 *
 * The library is a weak import, so this plugin still loads when the kernel
 * module is missing (the Settings page and quick menu then say "VPN module
 * not loaded"). A weak import that was never linked must not be called, so
 * nothing below calls the module until vtc_available() has found it by
 * name and checked its API version. Kernel modules are loaded before
 * SceShell and the Settings app start, so a module that is present was
 * linked when this plugin loaded.
 */
#include <psp2/vshbridge.h>
#include <string.h>

#include "vt_client.h"
#include "vtc_ext.h"
#include "log.h"

static int g_avail = 1;   /* 1 = not probed yet */

int vtc_is_stub(void)
{
	return 0;
}

int vtc_available(void)
{
	if (g_avail != 1)
		return g_avail;

	int search[4] = { 0, 0, 0, 0 };
	SceUID mod = _vshKernelSearchModuleByName(VT_KERNEL_MODULE_NAME, search);
	if (mod < 0) {
		g_avail = VTC_NO_MODULE;
		VTW_LOG("kernel module '%s' not found (0x%08X): staying inert",
		        VT_KERNEL_MODULE_NAME, (unsigned)mod);
		return g_avail;
	}

	VtStatus st;
	memset(&st, 0, sizeof(st));
	st.size = sizeof(st);
	int r = vtGetStatus(&st);
	if (r < 0 || st.api_version != VT_API_VERSION) {
		g_avail = VTC_OLD_MODULE;
		VTW_LOG("kernel module found but API %u (want %u), rc 0x%08X",
		        (unsigned)st.api_version, (unsigned)VT_API_VERSION, (unsigned)r);
		return g_avail;
	}
	g_avail = VTC_READY;
	VTW_LOG("kernel module ready, API %u", (unsigned)st.api_version);
	return g_avail;
}

int vtc_get_status(VtStatus *s)
{
	memset(s, 0, sizeof(*s));
	int a = vtc_available();
	if (a != VTC_READY)
		return a;
	s->size = sizeof(*s);
	int r = vtGetStatus(s);
	if (r < 0) {
		memset(s, 0, sizeof(*s));
		return r;
	}
	return 0;
}

#define GUARDED(call) do { int a_ = vtc_available(); if (a_ != VTC_READY) return a_; return (call); } while (0)

int vtc_connect(void)                         { GUARDED(vtConnect()); }
int vtc_disconnect(void)                      { GUARDED(vtDisconnect()); }
int vtc_pause(uint32_t seconds)               { GUARDED(vtPause(seconds)); }
int vtc_resume(void)                          { GUARDED(vtResume()); }
int vtc_lend_rx(void *buf, uint32_t size)     { GUARDED(vtTransportRx(buf, size)); }
int vtc_lend_tx(void *buf, uint32_t size)     { GUARDED(vtTransportTx(buf, size)); }

int vtc_set_overlay(const VtOverlay *o)       { GUARDED(vtSetOverlay(o)); }
int vtc_overlay_register(void)                { GUARDED(vtSetOverlay(NULL)); }

int vtc_get_overlay(VtOverlay *o)
{
	memset(o, 0, sizeof(*o));
	int a = vtc_available();
	if (a != VTC_READY)
		return a;
	int r = vtGetOverlay(o);
	if (r < 0)
		memset(o, 0, sizeof(*o));
	return r < 0 ? r : 0;
}

int vtc_set_lockdown(int on)
{
	VtStatus st;
	int r = vtc_get_status(&st);
	if (r < 0)
		return r;
	if (!vtc_has_lockdown(&st))
		return VTC_OLD_MODULE;
	VtOptions o;
	memset(&o, 0, sizeof(o));
	o.size = sizeof(o);
	o.kill_switch = st.kill_switch;
	o.allow_local = st.allow_local;
	VT_OPT_LOCKDOWN(&o) = (uint8_t)(on ? VT_LOCKDOWN_ON : VT_LOCKDOWN_OFF);
	return vtSetOptions(&o);
}

int vtc_set_sendcore(int fast)
{
	VtStatus st;
	int r = vtc_get_status(&st);
	if (r < 0)
		return r;
	if (!vtc_has_sendcore(&st))
		return VTC_OLD_MODULE;
	VtOptions o;
	memset(&o, 0, sizeof(o));
	o.size = sizeof(o);
	o.kill_switch = st.kill_switch;
	o.allow_local = st.allow_local;
	VT_OPT_SENDCORE(&o) = (uint16_t)(fast ? VT_SENDCORE_GAME : VT_SENDCORE_SYSTEM);
	return vtSetOptions(&o);
}
