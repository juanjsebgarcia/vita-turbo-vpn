/*
 * draw_host.c - software back end: draws into a memory framebuffer, for
 * screenshots on a desktop and for generating the LiveArea images.
 *
 * Fallback glyphs come from Noto fonts in <font dir>/fallback/ (fetched by
 * `make fallback-fonts`, not shipped): they stand in for the console's
 * system fonts so screenshots in Russian, Japanese, Korean and Chinese
 * show real text. It also records where text and boxes were drawn, so the
 * screenshot tool can report text that runs out of its box or into other
 * text (host_layout_report).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw_backend.h"
#include "draw_host.h"
#include "stb_image_write.h"
#include "stb_truetype.h"

/* defined in stb_image_write.h's implementation, not declared in its header */
unsigned char *stbi_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality);

static uint32_t *g_fb;      /* 0xAARRGGBB, alpha unused */
static int g_w, g_h;
static int g_cx0, g_cy0, g_cx1, g_cy1;

int host_target(int w, int h)
{
	free(g_fb);
	g_fb = calloc((size_t)w * h, sizeof(*g_fb));
	g_w = w;
	g_h = h;
	be_unclip();
	return g_fb ? 0 : -1;
}

const uint32_t *host_pixels(void)
{
	return g_fb;
}

void be_begin(Colour clear)
{
	for (int i = 0; i < g_w * g_h; i++)
		g_fb[i] = clear;
	host_layout_reset();
}

void (*be_trace)(const char *what);

void be_end(void)
{
}

void be_clip(int x, int y, int w, int h)
{
	g_cx0 = x < 0 ? 0 : x;
	g_cy0 = y < 0 ? 0 : y;
	g_cx1 = x + w > g_w ? g_w : x + w;
	g_cy1 = y + h > g_h ? g_h : y + h;
}

void be_unclip(void)
{
	g_cx0 = g_cy0 = 0;
	g_cx1 = g_w;
	g_cy1 = g_h;
}

void be_masks_flushed(void)
{
}

static inline void blend(uint32_t *p, Colour c, unsigned a)
{
	/* a is 0-255 coverage times the colour's own alpha */
	if (a == 0)
		return;
	if (a >= 255) {
		*p = c | 0xFF000000u;
		return;
	}
	uint32_t d = *p;
	unsigned r = ((c >> 16 & 255) * a + (d >> 16 & 255) * (255 - a) + 127) / 255;
	unsigned g = ((c >> 8 & 255) * a + (d >> 8 & 255) * (255 - a) + 127) / 255;
	unsigned b = ((c & 255) * a + (d & 255) * (255 - a) + 127) / 255;
	*p = 0xFF000000u | r << 16 | g << 8 | b;
}

void be_fill(int x, int y, int w, int h, Colour c)
{
	/* a dimmed overlay (dialogs): what was under it no longer counts */
	if (x <= 0 && y <= 0 && x + w >= g_w && y + h >= g_h)
		host_layout_reset();
	unsigned ca = c >> 24;
	int x0 = x < g_cx0 ? g_cx0 : x, y0 = y < g_cy0 ? g_cy0 : y;
	int x1 = x + w > g_cx1 ? g_cx1 : x + w, y1 = y + h > g_cy1 ? g_cy1 : y + h;
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			blend(&g_fb[j * g_w + i], c, ca);
}

void be_mask(Mask *m, int x, int y, Colour c)
{
	unsigned ca = c >> 24;
	for (int j = 0; j < m->h; j++) {
		int py = y + j;
		if (py < g_cy0 || py >= g_cy1)
			continue;
		for (int i = 0; i < m->w; i++) {
			int px = x + i;
			if (px < g_cx0 || px >= g_cx1)
				continue;
			blend(&g_fb[py * g_w + px], c, (m->a[j * m->w + i] * ca + 127) / 255);
		}
	}
}

void *be_load_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
		free(d);
		d = NULL;
	}
	fclose(f);
	if (d && size)
		*size = (size_t)n;
	return d;
}

/* ---- fallback fonts ---------------------------------------------------- */

/* Latin and Cyrillic first, then the CJK regions in the language's order */
enum { FB_LATIN, FB_JP, FB_KR, FB_SC, FB_TC, FB_FACES };

static const char *const g_fb_files[FB_FACES][2] = {
	[FB_LATIN] = { "NotoSans-Regular.ttf", "NotoSans-Bold.ttf" },
	[FB_JP]    = { "NotoSansJP-Regular.otf", "NotoSansJP-Bold.otf" },
	[FB_KR]    = { "NotoSansKR-Regular.otf", "NotoSansKR-Bold.otf" },
	[FB_SC]    = { "NotoSansSC-Regular.otf", "NotoSansSC-Bold.otf" },
	[FB_TC]    = { "NotoSansTC-Regular.otf", "NotoSansTC-Bold.otf" },
};

typedef struct FbFace {
	int tried, ok;
	unsigned char *data;
	stbtt_fontinfo info;
} FbFace;

static FbFace g_faces[FB_FACES][2];
static char g_fb_dir[256];
static int g_fb_order[FB_FACES];

void be_fallback_init(const char *font_dir, int lang)
{
	snprintf(g_fb_dir, sizeof(g_fb_dir), "%sfallback/", font_dir);
	static const int ja[] = { FB_LATIN, FB_JP, FB_SC, FB_TC, FB_KR };
	static const int ko[] = { FB_LATIN, FB_KR, FB_JP, FB_SC, FB_TC };
	static const int zht[] = { FB_LATIN, FB_TC, FB_SC, FB_JP, FB_KR };
	static const int zhs[] = { FB_LATIN, FB_SC, FB_TC, FB_JP, FB_KR };
	const int *o = lang == 0 ? ja : lang == 9 ? ko : lang == 10 ? zht : lang == 11 ? zhs : ja;
	memcpy(g_fb_order, o, sizeof(g_fb_order));
}

void be_fallback_shutdown(void)
{
	for (int i = 0; i < FB_FACES; i++)
		for (int b = 0; b < 2; b++) {
			free(g_faces[i][b].data);
			memset(&g_faces[i][b], 0, sizeof(g_faces[i][b]));
		}
}

static FbFace *fb_face(int face, int bold)
{
	FbFace *f = &g_faces[face][bold];
	if (!f->tried) {
		char path[512];
		f->tried = 1;
		snprintf(path, sizeof(path), "%s%s", g_fb_dir, g_fb_files[face][bold]);
		f->data = be_load_file(path, NULL);
		f->ok = f->data && stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0));
	}
	return f->ok ? f : NULL;
}

int be_fallback_glyph(uint32_t cp, float size, int bold, FbGlyph *g)
{
	for (int i = 0; i < FB_FACES; i++) {
		FbFace *f = fb_face(g_fb_order[i], bold);
		int gi;
		if (!f || !(gi = stbtt_FindGlyphIndex(&f->info, (int)cp)))
			continue;
		float sc = stbtt_ScaleForMappingEmToPixels(&f->info, size);
		int x0, y0, x1, y1, adv, lsb;
		stbtt_GetGlyphBitmapBox(&f->info, gi, sc, sc, &x0, &y0, &x1, &y1);
		stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
		g->w = x1 - x0;
		g->h = y1 - y0;
		g->ox = x0;
		g->oy = y0;
		g->adv = adv * sc;
		g->a = NULL;
		if (g->w > 0 && g->h > 0) {
			g->a = calloc((size_t)g->w * g->h, 1);
			if (!g->a)
				return -1;
			stbtt_MakeGlyphBitmap(&f->info, g->a, g->w, g->h, g->w, sc, sc, gi);
		}
		return 0;
	}
	return -1;
}

/* ---- layout checks ----------------------------------------------------- */

#define MAX_TRACE 512

typedef struct TraceRect {
	float x, y, w, h;
	char s[64];
} TraceRect;

static TraceRect g_texts[MAX_TRACE], g_boxes[MAX_TRACE];
static int g_ntexts, g_nboxes;
static char g_noted[4][64];
static int g_nnoted;

void host_layout_reset(void)
{
	g_ntexts = g_nboxes = 0;
	g_nnoted = 0;
}

void host_layout_problem(const char *what)
{
	if (g_nnoted < 4)
		snprintf(g_noted[g_nnoted++], sizeof(g_noted[0]), "%s", what);
}

void host_trace_box(int x, int y, int w, int h)
{
	if (g_nboxes < MAX_TRACE)
		g_boxes[g_nboxes++] = (TraceRect){ (float)x, (float)y, (float)w, (float)h, "" };
}

void host_trace_text(float x, float y, float w, float h, const char *s)
{
	/* only what the clip lets through */
	float x0 = x > g_cx0 ? x : g_cx0, y0 = y > g_cy0 ? y : g_cy0;
	float x1 = x + w < g_cx1 ? x + w : g_cx1, y1 = y + h < g_cy1 ? y + h : g_cy1;
	if (x1 - x0 < 1 || y1 - y0 < 1 || w < 1 || g_ntexts >= MAX_TRACE)
		return;
	/* text cut by the clip's right edge counts as running out */
	if (x + w > g_cx1 + 0.5f && g_cx1 < g_w)
		x1 = x + w;
	TraceRect *t = &g_texts[g_ntexts++];
	*t = (TraceRect){ x0, y0, x1 - x0, y1 - y0, "" };
	snprintf(t->s, sizeof(t->s), "%s", s);
}

void host_trace_cover(int x, int y, int w, int h)
{
	int k = 0;
	for (int i = 0; i < g_ntexts; i++) {
		const TraceRect *t = &g_texts[i];
		int hit = t->x < x + w && t->x + t->w > x && t->y < y + h && t->y + t->h > y;
		if (!hit)
			g_texts[k++] = *t;
	}
	g_ntexts = k;
}

static int inside(const TraceRect *b, float x, float y)
{
	return x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h;
}

int host_layout_report(const char *name)
{
	int problems = 0;
	for (int i = 0; i < g_nnoted; i++, problems++)
		printf("  ! %s: %s\n", name, g_noted[i]);
	for (int i = 0; i < g_ntexts; i++) {
		const TraceRect *t = &g_texts[i];
		if (t->x < -0.5f || t->x + t->w > g_w + 0.5f) {
			printf("  ! %s: \"%s\" runs off the screen\n", name, t->s);
			problems++;
			continue;
		}
		/* the smallest box the text starts in */
		const TraceRect *box = NULL;
		for (int b = 0; b < g_nboxes; b++)
			if (inside(&g_boxes[b], t->x + 1, t->y + t->h / 2) &&
			    (!box || g_boxes[b].w * g_boxes[b].h < box->w * box->h))
				box = &g_boxes[b];
		if (box && t->x + t->w > box->x + box->w + 0.5f) {
			printf("  ! %s: \"%s\" runs %.0f px out of its box\n", name, t->s,
				t->x + t->w - (box->x + box->w));
			problems++;
		}
		for (int j = i + 1; j < g_ntexts; j++) {
			const TraceRect *u = &g_texts[j];
			float ox = (t->x + t->w < u->x + u->w ? t->x + t->w : u->x + u->w) - (t->x > u->x ? t->x : u->x);
			float oy = (t->y + t->h < u->y + u->h ? t->y + t->h : u->y + u->h) - (t->y > u->y ? t->y : u->y);
			if (ox > 2 && oy > 4) {
				printf("  ! %s: \"%s\" overlaps \"%s\"\n", name, t->s, u->s);
				problems++;
			}
		}
	}
	return problems;
}

int host_write_png(const char *path)
{
	unsigned char *rgb = malloc((size_t)g_w * g_h * 3);
	if (!rgb)
		return -1;
	for (int i = 0; i < g_w * g_h; i++) {
		rgb[i * 3 + 0] = g_fb[i] >> 16 & 255;
		rgb[i * 3 + 1] = g_fb[i] >> 8 & 255;
		rgb[i * 3 + 2] = g_fb[i] & 255;
	}
	int ok = stbi_write_png(path, g_w, g_h, 3, rgb, g_w * 3);
	free(rgb);
	return ok ? 0 : -1;
}

/* ---- 8-bit palette PNG, which LiveArea images must be ------------------ */

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint32_t crc32_png(const unsigned char *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t n)
{
	unsigned char hdr[8];
	put32(hdr, n);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	unsigned char *buf = malloc(n + 4);
	memcpy(buf, type, 4);
	if (n)
		memcpy(buf + 4, data, n);
	unsigned char crc[4];
	put32(crc, crc32_png(buf, n + 4));
	free(buf);
	if (n)
		fwrite(data, 1, n, f);
	fwrite(crc, 1, 4, f);
}

/*
 * Colours are reduced to 5-6-5 bits until they fit in 256; the images are
 * flat, so this is close to lossless in practice.
 */
int host_write_png8(const char *path)
{
	static uint32_t pal[256];
	int npal = 0;
	unsigned char *idx = malloc((size_t)g_w * g_h);
	if (!idx)
		return -1;
	for (int shift = 0; shift <= 3; shift++) {
		uint32_t mask = (0xFFu << shift) & 0xFF;
		mask = mask << 16 | mask << 8 | mask;
		npal = 0;
		int i;
		for (i = 0; i < g_w * g_h; i++) {
			uint32_t c = g_fb[i] & mask;
			int k;
			for (k = 0; k < npal && pal[k] != c; k++)
				;
			if (k == npal) {
				if (npal == 256)
					break;
				pal[npal++] = c;
			}
			idx[i] = (unsigned char)k;
		}
		if (i == g_w * g_h)
			break;
	}
	size_t raw_n = (size_t)(g_w + 1) * g_h;
	unsigned char *raw = malloc(raw_n);
	for (int y = 0; y < g_h; y++) {
		raw[y * (g_w + 1)] = 0;
		memcpy(raw + y * (g_w + 1) + 1, idx + y * g_w, (size_t)g_w);
	}
	int zn;
	unsigned char *z = stbi_zlib_compress(raw, (int)raw_n, &zn, 9);
	FILE *f = fopen(path, "wb");
	if (!f || !z) {
		free(raw);
		free(idx);
		if (f)
			fclose(f);
		return -1;
	}
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	fwrite(sig, 1, 8, f);
	unsigned char ihdr[13];
	put32(ihdr, (uint32_t)g_w);
	put32(ihdr + 4, (uint32_t)g_h);
	ihdr[8] = 8;    /* bit depth */
	ihdr[9] = 3;    /* palette */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	unsigned char plte[768];
	for (int k = 0; k < npal; k++) {
		plte[k * 3 + 0] = pal[k] >> 16 & 255;
		plte[k * 3 + 1] = pal[k] >> 8 & 255;
		plte[k * 3 + 2] = pal[k] & 255;
	}
	chunk(f, "PLTE", plte, (uint32_t)npal * 3);
	chunk(f, "IDAT", z, (uint32_t)zn);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(z);   /* stb_image_write allocates with malloc */
	free(raw);
	free(idx);
	return 0;
}
