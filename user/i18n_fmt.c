/*
 * i18n_fmt.c - the language and the formatting helpers, without the string
 * table: SceShell's module links only this (the table lives in i18n.c, linked
 * by the Settings page and the quick-menu rows), as Wi-Fi Turbo keeps its
 * Settings page out of SceShell.
 *
 * The language comes from the /CONFIG/SYSTEM/language registry key (no init
 * gate, readable in SceShell and in the Settings app), with
 * sceAppUtilSystemParamGetInt as a fallback, and English(US) otherwise, as
 * in Wi-Fi Turbo.
 * Stubs (weak, resident in both processes): SceRegistryMgr, SceAppUtil.
 */
#include <stdarg.h>

#include "i18n.h"

extern int sceRegMgrGetKeyInt(const char *category, const char *name, int *value);
extern int sceAppUtilSystemParamGetInt(int paramId, int *value);

#define VTW_SYSTEM_PARAM_ID_LANG 1

int vtw_i18n_lang(void)
{
	int lang = -1;

	if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "language", &lang) >= 0 &&
	    lang >= 0 && lang < VTW_LANG_COUNT)
		return lang;

	/* Only works once the host has called sceAppUtilInit(). */
	lang = -1;
	if (sceAppUtilSystemParamGetInt(VTW_SYSTEM_PARAM_ID_LANG, &lang) >= 0 &&
	    lang >= 0 && lang < VTW_LANG_COUNT)
		return lang;

	return VTW_LANG_ENGLISH_US;
}

/* ---- pattern filling ---------------------------------------------------- */

int vtw_format(char *out, int cap, const char *pattern, ...)
{
	va_list ap;
	int n = 0;

	if (out == 0 || cap <= 0)
		return 0;
	va_start(ap, pattern);
	for (const char *p = pattern; p && *p && n < cap - 1; p++) {
		if (p[0] == '%' && p[1] == 's') {
			const char *a = va_arg(ap, const char *);
			while (a && *a && n < cap - 1)
				out[n++] = *a++;
			p++;
		} else if (p[0] == '%' && p[1] == 'u') {
			char tmp[12];
			int k = 0;
			unsigned v = va_arg(ap, unsigned);
			do { tmp[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < (int)sizeof(tmp));
			while (k && n < cap - 1)
				out[n++] = tmp[--k];
			p++;
		} else {
			out[n++] = *p;
		}
	}
	va_end(ap);
	out[n] = 0;
	return n;
}

/* ---- UTF-8 -> UTF-16 ---------------------------------------------------- */

int vtw_to_u16(const char *utf8, unsigned short *out, int out_max)
{
	const unsigned char *s = (const unsigned char *)utf8;
	int n = 0;

	if (out == 0 || out_max <= 0)
		return 0;
	if (s == 0) {
		out[0] = 0;
		return 0;
	}
	while (*s) {
		unsigned int cp;
		unsigned char c = *s;

		if (c < 0x80) {
			cp = c;
			s += 1;
		} else if ((c & 0xE0) == 0xC0) {
			if ((s[1] & 0xC0) != 0x80) { s += 1; continue; }
			cp = ((unsigned int)(c & 0x1F) << 6) | (unsigned int)(s[1] & 0x3F);
			s += 2;
		} else if ((c & 0xF0) == 0xE0) {
			if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) { s += 1; continue; }
			cp = ((unsigned int)(c & 0x0F) << 12) |
			     ((unsigned int)(s[1] & 0x3F) << 6) | (unsigned int)(s[2] & 0x3F);
			s += 3;
		} else if ((c & 0xF8) == 0xF0) {
			/* outside the BMP: dropped (the strings never need it) */
			if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
				s += 1;
				continue;
			}
			s += 4;
			continue;
		} else {
			s += 1;
			continue;
		}
		if (n >= out_max - 1)
			break;
		out[n++] = (unsigned short)cp;
	}
	out[n] = 0;
	return n;
}
