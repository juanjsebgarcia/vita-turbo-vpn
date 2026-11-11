/*
 * status.h - the tunnel's state in words, shared by the Settings page and
 * the quick menu so both always say the same thing.
 */
#ifndef VTW_STATUS_H
#define VTW_STATUS_H

#include "vt_client.h"

/* The translated state word's id (VTW_I18N_ST_*) for a status read with
 * result `rc` from vtc_get_status(). */
int vtw_state_word(int rc, const VtStatus *st);

/* Read the status and build the status line: "<nickname>: <state>", or the
 * state alone when there is no nickname or no module. Returns the
 * vtc_get_status() result; `st` holds the status read. */
int vtw_status_line(VtStatus *st, char *out, int cap);

/* The state word alone ("Connected", "Paused, 3 min left", ...), and the
 * profile name alone ("" when none); both fill *st as vtw_status_line does. */
int vtw_state_text(VtStatus *st, char *out, int cap);
int vtw_profile_text(VtStatus *st, char *out, int cap);

/* The same two from a status already read (rc = vtc_get_status() result),
 * so a caller can read once and format off the UI thread. state_text_of
 * returns the state word's id. */
int  vtw_state_text_of(int rc, const VtStatus *st, char *out, int cap);
void vtw_profile_text_of(int rc, const VtStatus *st, char *out, int cap);

/* The quick menu's short form of a state word (VTW_I18N_QM_ST_*, at most
 * 14 columns: tools/gen_i18n.py checks), for a VTW_I18N_ST_* id; and the
 * short state text from a status already read. Returns the full word's
 * id (VTW_I18N_ST_*), as vtw_state_text_of does, for vtw_state_tone(). The
 * Settings page keeps the full words. */
int vtw_state_short_id(int word_id);
int vtw_state_short_text_of(int rc, const VtStatus *st, char *out, int cap);

/* How a state word should look where a surface tints something by state
 * (the quick menu's status icon): normal, trouble (Signal red) or idle
 * (dimmed). */
enum { VTW_TONE_NORMAL = 0, VTW_TONE_TROUBLE, VTW_TONE_IDLE };
int vtw_state_tone(int word_id);

#endif /* VTW_STATUS_H */
