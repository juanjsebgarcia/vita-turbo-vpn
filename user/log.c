/*
 * log.c - see log.h. An open-append-close logger, so nothing stays open.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdint.h>

#include "log.h"

static const char *g_tag = "?";

void vtw_log_set_tag(const char *tag)
{
	g_tag = tag ? tag : "?";
}

static int put_uint(char *buf, int cap, int pos, unsigned v, unsigned base,
                    int upper, int width, int zero)
{
	const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[16];
	int n = 0;

	if (!v) tmp[n++] = '0';
	while (v && n < (int)sizeof(tmp)) { tmp[n++] = digs[v % base]; v /= base; }
	for (int pad = width - n; pad > 0 && pos < cap - 1; pad--)
		buf[pos++] = zero ? '0' : ' ';
	while (n && pos < cap - 1) buf[pos++] = tmp[--n];
	return pos;
}

static int put_fmt(char *buf, int cap, const char *fmt, va_list ap)
{
	int pos = 0;

	for (const char *f = fmt; *f && pos < cap - 1; f++) {
		if (*f != '%') { buf[pos++] = *f; continue; }
		f++;
		int zero = 0, width = 0;
		if (*f == '0') { zero = 1; f++; }
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		switch (*f) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s) s = "(null)";
			while (*s && pos < cap - 1) buf[pos++] = *s++;
			break;
		}
		case 'd': {
			int v = va_arg(ap, int);
			if (v < 0 && pos < cap - 1) { buf[pos++] = '-'; v = -v; }
			pos = put_uint(buf, cap, pos, (unsigned)v, 10, 0, width, zero);
			break;
		}
		case 'u':
			pos = put_uint(buf, cap, pos, va_arg(ap, unsigned), 10, 0, width, zero);
			break;
		case 'x':
			pos = put_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 0, width, zero);
			break;
		case 'X':
			pos = put_uint(buf, cap, pos, va_arg(ap, unsigned), 16, 1, width, zero);
			break;
		case 'c':
			buf[pos++] = (char)va_arg(ap, int);
			break;
		case '%':
			buf[pos++] = '%';
			break;
		case 0:
			buf[pos] = 0;
			return pos;
		default:
			buf[pos++] = '%';
			if (pos < cap - 1) buf[pos++] = *f;
			break;
		}
	}
	buf[pos] = 0;
	return pos;
}

void vtw_log(const char *fmt, ...)
{
	char line[224];
	int pos = 0;
	uint64_t t = (uint64_t)sceKernelGetProcessTimeWide();

	line[pos++] = '[';
	for (const char *s = g_tag; *s && pos < 16; s++) line[pos++] = *s;
	line[pos++] = ']';
	line[pos++] = ' ';
	pos = put_uint(line, sizeof(line), pos, (unsigned)(t / 1000000u), 10, 0, 0, 0);
	line[pos++] = '.';
	pos = put_uint(line, sizeof(line), pos, (unsigned)(t % 1000000u / 1000u), 10, 0, 3, 1);
	line[pos++] = ' ';

	va_list ap;
	va_start(ap, fmt);
	pos += put_fmt(line + pos, (int)sizeof(line) - pos - 1, fmt, ap);
	va_end(ap);
	line[pos++] = '\n';

	sceIoMkdir("ur0:data", 0777);
	sceIoMkdir(VTW_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(VTW_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0)
		return;
	if (sceIoLseek(fd, 0, SCE_SEEK_END) > VTW_LOG_MAX_BYTES) {
		sceIoClose(fd);
		sceIoRemove(VTW_LOG_PATH_OLD);
		sceIoRename(VTW_LOG_PATH, VTW_LOG_PATH_OLD);
		fd = sceIoOpen(VTW_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return;
	}
	sceIoWrite(fd, line, pos);
	sceIoClose(fd);
}
