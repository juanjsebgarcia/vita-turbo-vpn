/*
 * vtc_ext.h - vtc_* additions (Lockdown mode),
 * kept beside ../vt_client.h. Same return codes as the other vtc_* calls.
 */
#ifndef VTC_EXT_H
#define VTC_EXT_H

#include "vt_client.h"

/* Lockdown mode on or off (vtSetOptions with VT_LOCKDOWN_ON/OFF; the kill
 * switch and allow local are passed back as the status reports them, and
 * the module ignores them from the Settings app anyway). */
int vtc_set_lockdown(int on);

/* 1 if the loaded module understands Lockdown (VT_FEAT_LOCKDOWN). */
static inline int vtc_has_lockdown(const VtStatus *st)
{
	return (VT_ST_FEATURES(st) & VT_FEAT_LOCKDOWN) != 0;
}

/* Faster sending on or off (vtSetOptions with VT_SENDCORE_GAME/SYSTEM;
 * the Settings app may change it, as Lockdown). */
int vtc_set_sendcore(int fast);

/* 1 if the loaded module has the faster-sending setting (VT_FEAT_SENDCORE). */
static inline int vtc_has_sendcore(const VtStatus *st)
{
	return (VT_ST_FEATURES(st) & VT_FEAT_SENDCORE) != 0;
}

#endif /* VTC_EXT_H */
