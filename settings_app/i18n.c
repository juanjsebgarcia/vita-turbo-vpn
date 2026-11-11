/*
 * i18n.c - translation lookup (the string table). See user/i18n.h; the
 * language and the formatting helpers are in user/i18n_fmt.c.
 *
 * The quick-menu module builds with VTW_I18N_QM_ONLY and keeps only its own
 * rows (the first VTW_I18N_QM_COUNT ids), so it does not carry the Settings
 * page's text into SceShell; an id past them reads as "".
 */
#include "i18n.h"

#include "i18n_table.h"

const char *vtw_i18n_for(int id, int lang)
{
	const char *s;

	if (id < 0 || id >= VTW_I18N_ROWS)
		return "";
	if (lang < 0 || lang >= VTW_LANG_COUNT)
		lang = VTW_LANG_ENGLISH_US;
	s = g_tbl[id][lang];
	if (s == 0)
		s = g_tbl[id][VTW_LANG_ENGLISH_US];
	return s ? s : "";
}

const char *vtw_i18n(int id)
{
	return vtw_i18n_for(id, vtw_i18n_lang());
}
