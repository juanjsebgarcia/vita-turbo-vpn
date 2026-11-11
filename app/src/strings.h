/*
 * strings.h - every piece of UI text, by id.
 *
 * The ids and the English text live together in strings_en.def. Each other
 * language is src/lang/<code>.def, the same X(id, text) lines (see
 * strings.c); an entry it leaves out falls back to English. Placeholders
 * are {0} and {1}; a translation may move them but keeps both.
 */
#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>

typedef enum StrId {
#define X(id, en) id,
#include "strings_en.def"
#undef X
	S_COUNT
} StrId;

/* Languages, numbered as the system's language setting (SCE_SYSTEM_PARAM_LANG_*) */
enum {
	LANG_JAPANESE = 0,
	LANG_ENGLISH_US = 1,
	LANG_FRENCH = 2,
	LANG_SPANISH = 3,
	LANG_GERMAN = 4,
	LANG_ITALIAN = 5,
	LANG_DUTCH = 6,
	LANG_PORTUGUESE_PT = 7,
	LANG_RUSSIAN = 8,
	LANG_KOREAN = 9,
	LANG_CHINESE_T = 10,
	LANG_CHINESE_S = 11,
	LANG_FINNISH = 12,
	LANG_SWEDISH = 13,
	LANG_DANISH = 14,
	LANG_NORWEGIAN = 15,
	LANG_POLISH = 16,
	LANG_PORTUGUESE_BR = 17,
	LANG_ENGLISH_GB = 18,
	LANG_TURKISH = 19,
	LANG_COUNT
};

void str_set_language(int lang);
/* The language in use, and the short code of a language (its file name in
 * src/lang/, "en" for English US); -1 or NULL when unknown */
int str_language(void);
const char *str_lang_code(int lang);
int str_lang_from_code(const char *code);
const char *tr(StrId id);
/* tr(id) with {0} and {1} replaced; returns out */
const char *trf(char *out, size_t n, StrId id, const char *a0, const char *a1);

#endif
