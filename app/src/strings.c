/*
 * strings.c - the string tables and placeholder substitution.
 *
 * English is strings_en.def; every other language is src/lang/<code>.def,
 * with the same X(id, text) lines, in the system's language order. An entry
 * a language leaves out uses English. tools/check_strings.py checks the
 * tables against English (ids, placeholders).
 */
#include <string.h>

#include "strings.h"

#define X(id, text) [id] = text,
static const char *const g_en[S_COUNT] = {
#include "strings_en.def"
};

static const char *const g_ja[S_COUNT] = {
#include "lang/ja.def"
};

static const char *const g_fr[S_COUNT] = {
#include "lang/fr.def"
};

static const char *const g_es[S_COUNT] = {
#include "lang/es.def"
};

static const char *const g_de[S_COUNT] = {
#include "lang/de.def"
};

static const char *const g_it[S_COUNT] = {
#include "lang/it.def"
};

static const char *const g_nl[S_COUNT] = {
#include "lang/nl.def"
};

static const char *const g_pt[S_COUNT] = {
#include "lang/pt.def"
};

static const char *const g_ru[S_COUNT] = {
#include "lang/ru.def"
};

static const char *const g_ko[S_COUNT] = {
#include "lang/ko.def"
};

static const char *const g_zht[S_COUNT] = {
#include "lang/zht.def"
};

static const char *const g_zhs[S_COUNT] = {
#include "lang/zhs.def"
};

static const char *const g_fi[S_COUNT] = {
#include "lang/fi.def"
};

static const char *const g_sv[S_COUNT] = {
#include "lang/sv.def"
};

static const char *const g_da[S_COUNT] = {
#include "lang/da.def"
};

static const char *const g_no[S_COUNT] = {
#include "lang/no.def"
};

static const char *const g_pl[S_COUNT] = {
#include "lang/pl.def"
};

static const char *const g_ptbr[S_COUNT] = {
#include "lang/ptbr.def"
};

static const char *const g_engb[S_COUNT] = {
#include "lang/engb.def"
};

static const char *const g_tr[S_COUNT] = {
#include "lang/tr.def"
};
#undef X

static const struct {
	const char *code;
	const char *const *table;
} g_langs[LANG_COUNT] = {
	[LANG_JAPANESE] = { "ja", g_ja },
	[LANG_ENGLISH_US] = { "en", g_en },
	[LANG_FRENCH] = { "fr", g_fr },
	[LANG_SPANISH] = { "es", g_es },
	[LANG_GERMAN] = { "de", g_de },
	[LANG_ITALIAN] = { "it", g_it },
	[LANG_DUTCH] = { "nl", g_nl },
	[LANG_PORTUGUESE_PT] = { "pt", g_pt },
	[LANG_RUSSIAN] = { "ru", g_ru },
	[LANG_KOREAN] = { "ko", g_ko },
	[LANG_CHINESE_T] = { "zht", g_zht },
	[LANG_CHINESE_S] = { "zhs", g_zhs },
	[LANG_FINNISH] = { "fi", g_fi },
	[LANG_SWEDISH] = { "sv", g_sv },
	[LANG_DANISH] = { "da", g_da },
	[LANG_NORWEGIAN] = { "no", g_no },
	[LANG_POLISH] = { "pl", g_pl },
	[LANG_PORTUGUESE_BR] = { "ptbr", g_ptbr },
	[LANG_ENGLISH_GB] = { "engb", g_engb },
	[LANG_TURKISH] = { "tr", g_tr },
};

static const char *const *g_table = g_en;
static int g_lang = LANG_ENGLISH_US;

void str_set_language(int lang)
{
	if (lang < 0 || lang >= LANG_COUNT)
		lang = LANG_ENGLISH_US;
	g_lang = lang;
	g_table = g_langs[lang].table;
}

int str_language(void)
{
	return g_lang;
}

const char *str_lang_code(int lang)
{
	return lang >= 0 && lang < LANG_COUNT ? g_langs[lang].code : NULL;
}

int str_lang_from_code(const char *code)
{
	for (int i = 0; i < LANG_COUNT; i++)
		if (!strcmp(code, g_langs[i].code))
			return i;
	return -1;
}

const char *tr(StrId id)
{
	if ((unsigned)id >= (unsigned)S_COUNT)
		return "";
	const char *s = g_table[id];
	return s ? s : g_en[id] ? g_en[id] : "";
}

const char *trf(char *out, size_t n, StrId id, const char *a0, const char *a1)
{
	const char *s = tr(id);
	size_t o = 0;
	if (!n)
		return out;
	while (*s && o + 1 < n) {
		if (s[0] == '{' && (s[1] == '0' || s[1] == '1') && s[2] == '}') {
			const char *a = s[1] == '0' ? a0 : a1;
			while (a && *a && o + 1 < n)
				out[o++] = *a++;
			s += 3;
		} else {
			out[o++] = *s++;
		}
	}
	out[o] = 0;
	return out;
}
