/*
 * vt_client.h - the SceShell/Settings plugin's view of the kernel module's
 * control API (syscall library SceVpnTurbo).
 *
 * Two parts:
 *   1. The control API itself. It now comes straight from the canonical
 *      shared header include/vpnturbo.h, which carries VT_STATE_LOCKED,
 *      vtTransportRx/Tx and VT_LEND_RX_BUF_SIZE (the former "provisional"
 *      additions). The old in-file mirror and the -DVT_USE_SHARED_HEADER
 *      switch are gone.
 *   2. The vtc_* wrapper the plugin actually calls. It is implemented twice:
 *      vt_client.c talks to the kernel module and is harmless when the module
 *      is missing; vt_client_stub.c returns scripted states so the UI can be
 *      exercised with no kernel module (CMake option VT_CLIENT_STUB).
 */
#ifndef VT_CLIENT_H
#define VT_CLIENT_H

#include <stdint.h>

#include "vpnturbo.h"

/* Module name the plugin looks for before making any call (the name in the
 * kernel module's yml, not its file name). */
#ifndef VT_KERNEL_MODULE_NAME
#define VT_KERNEL_MODULE_NAME "vpnturbo_kernel"
#endif

/* ---- 2. what the plugin calls ------------------------------------------ */

/* vtc_available() */
enum {
	VTC_READY      = 0,   /* module loaded, API version matches */
	VTC_NO_MODULE  = -1,  /* kernel module not loaded: nothing is called */
	VTC_OLD_MODULE = -2,  /* loaded, but another API version */
};

/* Probe once (cheap to call again). Never calls into a missing module. */
int vtc_available(void);

/* 1 in the scripted (stub) build. */
int vtc_is_stub(void);

/* Status, connect/disconnect the last-used profile, pause and resume.
 * All return 0 or a negative code; with no module they return VTC_NO_MODULE
 * without calling anything. vtc_get_status fills `s` with zeros on failure. */
int vtc_get_status(VtStatus *s);
int vtc_connect(void);
int vtc_disconnect(void);
int vtc_pause(uint32_t seconds);
int vtc_resume(void);

/* Overlay settings: status-bar padlock and HUD. vtc_set_overlay
 * also tells the module this process (SceShell or Settings) is a screen it
 * may draw on; vtc_overlay_register does only that. Same return codes. */
int vtc_get_overlay(VtOverlay *o);
int vtc_set_overlay(const VtOverlay *o);
int vtc_overlay_register(void);

/* The two lent-thread calls. */
int vtc_lend_rx(void *buf, uint32_t size);
int vtc_lend_tx(void *buf, uint32_t size);

/* "Wanted": the user asked for the tunnel and has not disconnected
 * (connecting, up, stalled, error, paused or locked). Drives the toggles. */
static inline int vtc_wanted(uint32_t state)
{
	return state != VT_STATE_IDLE;
}

#endif /* VT_CLIENT_H */
