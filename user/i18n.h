/*
 * i18n.h - translations for every string the plugin shows: the
 * Settings page, the quick-menu rows and the drop notification.
 *
 * Same scheme as the other Vita Turbo plugins: one table indexed by string id
 * and Vita system language, English(US) as the fallback, the language read
 * from the system registry each time a string is looked up, so a language
 * change shows the next time a page or the quick menu is opened.
 * The strings live in tools/gen_i18n.py; settings_app/i18n_ids.h and
 * i18n_table.h are generated from it. The quick-menu module builds with
 * VTW_I18N_QM_ONLY and holds only the first VTW_I18N_QM_COUNT ids.
 */
#ifndef VTW_I18N_H
#define VTW_I18N_H

/* Matches SCE_SYSTEM_PARAM_LANG_* and /CONFIG/SYSTEM/language. */
enum vtw_lang_id {
	VTW_LANG_JAPANESE = 0, VTW_LANG_ENGLISH_US = 1, VTW_LANG_FRENCH, VTW_LANG_SPANISH,
	VTW_LANG_GERMAN, VTW_LANG_ITALIAN, VTW_LANG_DUTCH, VTW_LANG_PORTUGUESE_PT,
	VTW_LANG_RUSSIAN, VTW_LANG_KOREAN, VTW_LANG_CHINESE_T, VTW_LANG_CHINESE_S,
	VTW_LANG_FINNISH, VTW_LANG_SWEDISH, VTW_LANG_DANISH, VTW_LANG_NORWEGIAN,
	VTW_LANG_POLISH, VTW_LANG_PORTUGUESE_BR, VTW_LANG_ENGLISH_GB, VTW_LANG_TURKISH,
	VTW_LANG_COUNT
};

#include "i18n_ids.h"

#define VTW_I18N_ENUM(name) VTW_I18N_##name,
enum vtw_i18n_id { VTW_I18N_IDS(VTW_I18N_ENUM) VTW_I18N_COUNT };
#undef VTW_I18N_ENUM

int vtw_i18n_lang(void);
const char *vtw_i18n(int id);                 /* never NULL, English fallback */
const char *vtw_i18n_for(int id, int lang);

/* Fill a translated pattern: each %s takes the next string argument and
 * each %u the next unsigned, in order. Always NUL-terminates. Returns the
 * length written. */
int vtw_format(char *out, int cap, const char *pattern, ...);

/* UTF-8 to UTF-16 (BMP only), NUL-terminated; returns code units written. */
int vtw_to_u16(const char *utf8, unsigned short *out, int out_max);

#endif /* VTW_I18N_H */
