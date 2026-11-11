/*
 * status.c - see status.h.
 */
#include "status.h"
#include "i18n.h"

int vtw_state_word(int rc, const VtStatus *st)
{
	if (rc == VTC_OLD_MODULE)
		return VTW_I18N_ST_OLD_MODULE;
	if (rc < 0)
		return VTW_I18N_ST_NO_MODULE;

	switch (st->state) {
	case VT_STATE_IDLE:
		if (st->hold == VT_HOLD_LOCKDOWN_MODE)
			return VTW_I18N_ST_LOCKDOWN;   /* off, and Lockdown blocks everything */
		return st->profile_name[0] ? VTW_I18N_ST_OFF : VTW_I18N_ST_NO_PROFILE;
	case VT_STATE_HANDSHAKING:
		return VTW_I18N_ST_CONNECTING;
	case VT_STATE_UP:
		return VTW_I18N_ST_CONNECTED;
	case VT_STATE_PAUSED:
		return st->pause_left_s ? VTW_I18N_ST_PAUSED_LEFT : VTW_I18N_ST_PAUSED;
	case VT_STATE_LOCKED:
		return VTW_I18N_ST_BLOCKED;
	case VT_STATE_STALLED:
	case VT_STATE_ERROR:
	default:
		/* Never claim protection for a state we do not know. */
		return st->kill_switch || VT_ST_LOCKDOWN(st) ? VTW_I18N_ST_KILL_HOLDING :
		       VTW_I18N_ST_NOT_PROTECTED;
	}
}

int vtw_status_line(VtStatus *st, char *out, int cap)
{
	char word[96];
	int rc = vtc_get_status(st);
	int id = vtw_state_word(rc, st);

	if (id == VTW_I18N_ST_PAUSED_LEFT)
		vtw_format(word, sizeof(word), vtw_i18n(id), (unsigned)((st->pause_left_s + 59) / 60));
	else
		vtw_format(word, sizeof(word), vtw_i18n(id));

	char name[VT_NAME_MAX];
	int k;
	for (k = 0; k < VT_NAME_MAX - 1 && st->profile_name[k]; k++)
		name[k] = st->profile_name[k];
	name[k] = 0;

	int n;
	if (rc >= 0 && name[0] && id != VTW_I18N_ST_NO_PROFILE)
		n = vtw_format(out, cap, vtw_i18n(VTW_I18N_FMT_NAME_STATE), name, word);
	else
		n = vtw_format(out, cap, "%s", word);

	if (vtc_is_stub() && n < cap - 2)
		vtw_format(out + n, cap - n, " %s", vtw_i18n(VTW_I18N_ST_TEST_BUILD));
	return rc;
}

int vtw_state_text_of(int rc, const VtStatus *st, char *out, int cap)
{
	int id = vtw_state_word(rc, st);
	int n;
	if (id == VTW_I18N_ST_PAUSED_LEFT)
		n = vtw_format(out, cap, vtw_i18n(id), (unsigned)((st->pause_left_s + 59) / 60));
	else
		n = vtw_format(out, cap, vtw_i18n(id));
	if (vtc_is_stub() && n < cap - 2)
		vtw_format(out + n, cap - n, " %s", vtw_i18n(VTW_I18N_ST_TEST_BUILD));
	return id;
}

int vtw_state_short_id(int word_id)
{
	switch (word_id) {
	case VTW_I18N_ST_OFF:            return VTW_I18N_QM_ST_OFF;
	case VTW_I18N_ST_CONNECTING:     return VTW_I18N_QM_ST_CONNECTING;
	case VTW_I18N_ST_CONNECTED:      return VTW_I18N_QM_ST_CONNECTED;
	case VTW_I18N_ST_NOT_PROTECTED:  return VTW_I18N_QM_ST_NOT_PROTECTED;
	case VTW_I18N_ST_KILL_HOLDING:   return VTW_I18N_QM_ST_KILL_HOLDING;
	case VTW_I18N_ST_BLOCKED:        return VTW_I18N_QM_ST_BLOCKED;
	case VTW_I18N_ST_LOCKDOWN:       return VTW_I18N_QM_ST_LOCKDOWN;
	case VTW_I18N_ST_PAUSED:         return VTW_I18N_QM_ST_PAUSED;
	case VTW_I18N_ST_PAUSED_LEFT:    return VTW_I18N_QM_ST_PAUSED_LEFT;
	case VTW_I18N_ST_NO_MODULE:      return VTW_I18N_QM_ST_NO_MODULE;
	case VTW_I18N_ST_OLD_MODULE:     return VTW_I18N_QM_ST_OLD_MODULE;
	case VTW_I18N_ST_NO_PROFILE:     return VTW_I18N_QM_ST_NO_PROFILE;
	default:                         return VTW_I18N_QM_ST_NOT_PROTECTED;
	}
}

int vtw_state_short_text_of(int rc, const VtStatus *st, char *out, int cap)
{
	int id = vtw_state_word(rc, st);
	int sid = vtw_state_short_id(id);
	int n;
	if (sid == VTW_I18N_QM_ST_PAUSED_LEFT)
		n = vtw_format(out, cap, vtw_i18n(sid), (unsigned)((st->pause_left_s + 59) / 60));
	else
		n = vtw_format(out, cap, vtw_i18n(sid));
	if (vtc_is_stub() && n < cap - 2)
		vtw_format(out + n, cap - n, " %s", vtw_i18n(VTW_I18N_ST_TEST_BUILD));
	return id;
}

int vtw_state_text(VtStatus *st, char *out, int cap)
{
	int rc = vtc_get_status(st);
	vtw_state_text_of(rc, st, out, cap);
	return rc;
}

void vtw_profile_text_of(int rc, const VtStatus *st, char *out, int cap)
{
	int k = 0;
	if (rc >= 0)
		for (; k < cap - 1 && k < VT_NAME_MAX - 1 && st->profile_name[k]; k++)
			out[k] = st->profile_name[k];
	out[k] = 0;
}

int vtw_profile_text(VtStatus *st, char *out, int cap)
{
	int rc = vtc_get_status(st);
	vtw_profile_text_of(rc, st, out, cap);
	return rc;
}

int vtw_state_tone(int word_id)
{
	switch (word_id) {
	case VTW_I18N_ST_CONNECTED:
	case VTW_I18N_ST_CONNECTING:
	case VTW_I18N_ST_PAUSED:
	case VTW_I18N_ST_PAUSED_LEFT:
		return VTW_TONE_NORMAL;
	case VTW_I18N_ST_NOT_PROTECTED:
	case VTW_I18N_ST_KILL_HOLDING:
	case VTW_I18N_ST_BLOCKED:
	case VTW_I18N_ST_LOCKDOWN:
		return VTW_TONE_TROUBLE;
	default:   /* off, no profile, no module, other version */
		return VTW_TONE_IDLE;
	}
}
