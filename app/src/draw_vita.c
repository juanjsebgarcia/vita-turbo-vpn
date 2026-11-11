/*
 * draw_vita.c - console back end, on vita2d.
 *
 * Rectangles are drawn directly. Masks (corners, icons, glyphs) are copied
 * once into a 1024x1024 atlas texture, white with the coverage as alpha,
 * and drawn tinted. When the atlas fills up it is emptied at the start of
 * the next frame, after the GPU has finished with it, and refilled as
 * masks are drawn again.
 *
 * Characters the bundled fonts lack (Cyrillic, kana, Han, Hangul) come from
 * the console's own system fonts through ScePvf, the library vita2d's pvf
 * text uses: the Latin set (which also has Cyrillic) and the Japanese,
 * Korean and Chinese sets, tried in an order that follows the system
 * language so Han characters take their regional forms. Each glyph is
 * rasterised once into a coverage mask and cached like any other.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/pvf.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>

#include "draw_backend.h"

#define ATLAS 1024

static vita2d_texture *g_atlas;
static uint32_t *g_px;
static unsigned g_stride;     /* in pixels */
static uint32_t g_gen = 1;
static int g_shelf_x, g_shelf_y, g_shelf_h;
static int g_full;

static unsigned col(Colour c)
{
	return RGBA8(c >> 16 & 255, c >> 8 & 255, c & 255, c >> 24);
}

static void atlas_reset(void)
{
	g_shelf_x = g_shelf_y = g_shelf_h = 0;
	g_gen++;
	if (g_gen == 0)
		g_gen = 1;
	g_full = 0;
}

static int atlas_init(void)
{
	g_atlas = vita2d_create_empty_texture_format(ATLAS, ATLAS, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
	if (!g_atlas)
		return -1;
	vita2d_texture_set_filters(g_atlas, SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
	g_px = vita2d_texture_get_datap(g_atlas);
	g_stride = vita2d_texture_get_stride(g_atlas) / 4;
	atlas_reset();
	return 0;
}

/* Place a mask in the atlas; 0 on success */
static int atlas_put(Mask *m)
{
	if (m->w > ATLAS || m->h > ATLAS)
		return -1;
	if (g_shelf_x + m->w > ATLAS) {
		g_shelf_y += g_shelf_h + 1;
		g_shelf_x = 0;
		g_shelf_h = 0;
	}
	if (g_shelf_y + m->h > ATLAS) {
		g_full = 1;
		return -1;
	}
	m->bx = g_shelf_x;
	m->by = g_shelf_y;
	m->bgen = g_gen;
	for (int y = 0; y < m->h; y++) {
		uint32_t *row = g_px + (size_t)(m->by + y) * g_stride + m->bx;
		const uint8_t *a = m->a + y * m->w;
		for (int x = 0; x < m->w; x++)
			row[x] = (uint32_t)a[x] << 24 | 0x00FFFFFFu;
	}
	g_shelf_x += m->w + 1;
	if (m->h > g_shelf_h)
		g_shelf_h = m->h;
	return 0;
}

void be_begin(Colour clear)
{
	if (!g_atlas)
		atlas_init();
	if (g_full) {
		vita2d_wait_rendering_done();
		atlas_reset();
	}
	vita2d_start_drawing();
	vita2d_set_clear_color(col(clear));
	vita2d_clear_screen();
}

/* Set by the app while it traces the frames after a pause */
void (*be_trace)(const char *what);

void be_end(void)
{
	vita2d_end_drawing();
	if (be_trace) {
		/* split the frame's end: a hang in the finish is the GPU on our
		 * scene; one in the swap is the display path */
		be_trace("  scene ended");
		vita2d_wait_rendering_done();
		be_trace("  gpu finished");
	}
	vita2d_swap_buffers();
	if (be_trace)
		be_trace("  swapped");
}

void be_fill(int x, int y, int w, int h, Colour c)
{
	vita2d_draw_rectangle(x, y, w, h, col(c));
}

void be_mask(Mask *m, int x, int y, Colour c)
{
	if (!g_atlas)
		return;
	if (m->bgen != g_gen && atlas_put(m) != 0)
		return;
	vita2d_draw_texture_tint_part(g_atlas, x, y, m->bx, m->by, m->w, m->h, col(c));
}

void be_clip(int x, int y, int w, int h)
{
	vita2d_set_clip_rectangle(x, y, x + w, y + h);
	vita2d_enable_clipping();
}

void be_unclip(void)
{
	vita2d_disable_clipping();
}

void be_masks_flushed(void)
{
	/* the masks are gone; their atlas space can be reused next frame */
	g_full = 1;
}

void *be_load_file(const char *path, size_t *size)
{
	SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	SceOff n = sceIoLseek(fd, 0, SCE_SEEK_END);
	sceIoLseek(fd, 0, SCE_SEEK_SET);
	void *d = n > 0 ? malloc((size_t)n) : NULL;
	if (d && sceIoRead(fd, d, (SceSize)n) != (int)n) {
		free(d);
		d = NULL;
	}
	sceIoClose(fd);
	if (d && size)
		*size = (size_t)n;
	return d;
}

/* ---- system fonts for fallback glyphs ---------------------------------- */

enum { PF_LATIN, PF_LATIN_BOLD, PF_J, PF_K, PF_C, PF_COUNT };

static ScePvfLibId g_pvf;
static ScePvfFontId g_pf[PF_COUNT];
static float g_pf_size[PF_COUNT];      /* char size last set, points */
static int g_pf_order[4];              /* CJK sets after Latin, by language */
static float g_pt_per_px = 72.0f / 128.0f;

static ScePvfPointer pvf_alloc(ScePvfPointer user, ScePvfU32 n)
{
	(void)user;
	return memalign(4, (n + 3) & ~3u);
}

static ScePvfPointer pvf_realloc(ScePvfPointer user, ScePvfPointer p, ScePvfU32 n)
{
	(void)user;
	return realloc(p, (n + 3) & ~3u);
}

static void pvf_free(ScePvfPointer user, ScePvfPointer p)
{
	(void)user;
	free(p);
}

static ScePvfFontId pvf_open(int language, int style)
{
	ScePvfFontStyleInfo want;
	ScePvfError err = 0;
	memset(&want, 0, sizeof(want));
	want.languageCode = (ScePvfU16)language;
	want.style = (ScePvfU16)style;
	ScePvfFontIndex i = scePvfFindOptimumFont(g_pvf, &want, &err);
	if (err != 0 || i < 0)
		return NULL;
	ScePvfFontId f = scePvfOpen(g_pvf, i, 0, &err);
	return err == 0 ? f : NULL;
}

static void pvf_size(int k, float pt)
{
	if (g_pf_size[k] != pt) {
		scePvfSetCharSize(g_pf[k], pt, pt);
		g_pf_size[k] = pt;
	}
}

void be_fallback_init(const char *font_dir, int lang)
{
	(void)font_dir;
	ScePvfInitRec init;
	ScePvfError err = 0;
	memset(&init, 0, sizeof(init));
	init.maxNumFonts = PF_COUNT;
	init.allocFunc = pvf_alloc;
	init.reallocFunc = pvf_realloc;
	init.freeFunc = pvf_free;
	g_pvf = scePvfNewLib(&init, &err);
	if (err != 0 || !g_pvf) {
		g_pvf = NULL;
		return;
	}
	/* as vita2d: 128 dpi, so a char size of 72/128 pt is 1 px */
	scePvfSetEM(g_pvf, 72.0f / (10.125f * 128.0f));
	scePvfSetResolution(g_pvf, 128.0f, 128.0f);
	g_pf[PF_LATIN] = pvf_open(SCE_PVF_LANGUAGE_LATIN, SCE_PVF_STYLE_REGULAR);
	g_pf[PF_LATIN_BOLD] = pvf_open(SCE_PVF_LANGUAGE_LATIN, SCE_PVF_STYLE_BOLD);
	g_pf[PF_J] = pvf_open(SCE_PVF_LANGUAGE_J, SCE_PVF_DEFAULT_STYLE_CODE);
	g_pf[PF_K] = pvf_open(SCE_PVF_LANGUAGE_K, SCE_PVF_DEFAULT_STYLE_CODE);
	g_pf[PF_C] = pvf_open(SCE_PVF_LANGUAGE_C, SCE_PVF_DEFAULT_STYLE_CODE);

	static const int ja[] = { PF_J, PF_C, PF_K, -1 };
	static const int ko[] = { PF_K, PF_J, PF_C, -1 };
	static const int zh[] = { PF_C, PF_J, PF_K, -1 };
	const int *o = lang == 9 ? ko : (lang == 10 || lang == 11) ? zh : ja;
	memcpy(g_pf_order, o, sizeof(g_pf_order));

	/* Check the pixel size against a CJK character, which is one em wide */
	static const struct { int font; ScePvfCharCode c; } probe[] = {
		{ PF_J, 0x4E2D }, { PF_C, 0x4E2D }, { PF_K, 0xAC00 },
	};
	for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
		ScePvfCharInfo ci;
		if (!g_pf[probe[i].font])
			continue;
		pvf_size(probe[i].font, 32.0f * g_pt_per_px);
		if (scePvfGetCharInfo(g_pf[probe[i].font], probe[i].c, &ci) < 0)
			continue;
		float px = ci.glyphMetrics.horizontalAdvance64 / 64.0f;
		if (px > 16.0f && px < 64.0f)
			g_pt_per_px *= 32.0f / px;
		break;
	}
}

void be_fallback_shutdown(void)
{
	for (int i = 0; i < PF_COUNT; i++)
		if (g_pf[i]) {
			if (i != PF_LATIN_BOLD || g_pf[i] != g_pf[PF_LATIN])
				scePvfClose(g_pf[i]);
			g_pf[i] = NULL;
		}
	if (g_pvf)
		scePvfDoneLib(g_pvf);
	g_pvf = NULL;
}

static int pvf_render(int k, uint32_t cp, float size, FbGlyph *g)
{
	ScePvfFontId f = g_pf[k];
	ScePvfCharInfo ci;
	ScePvfIrect r;
	if (!f || cp > 0xFFFF || !scePvfIsElement(f, (ScePvfCharCode)cp))
		return -1;
	pvf_size(k, size * g_pt_per_px);
	if (scePvfGetCharInfo(f, (ScePvfCharCode)cp, &ci) < 0 ||
	    scePvfGetCharImageRect(f, (ScePvfCharCode)cp, &r) < 0)
		return -1;
	/* 2 px of room on each side; the pen sits on whole pixels */
	int bx = (ci.glyphMetrics.horizontalBearingX64 + 32) >> 6;
	int by = (ci.glyphMetrics.horizontalBearingY64 + 32) >> 6;
	g->w = r.width + 4;
	g->h = r.height + 4;
	g->ox = bx - 2;
	g->oy = -by - 2;
	g->adv = ci.glyphMetrics.horizontalAdvance64 / 64.0f;
	g->a = NULL;
	if (r.width == 0 || r.height == 0)
		return 0;
	g->a = calloc((size_t)g->w * g->h, 1);
	if (!g->a)
		return -1;
	ScePvfUserImageBufferRec im;
	memset(&im, 0, sizeof(im));
	im.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
	im.xPos64 = (2 - bx) << 6;
	im.yPos64 = (2 + by) << 6;
	im.rect.width = (ScePvfU16)g->w;
	im.rect.height = (ScePvfU16)g->h;
	im.bytesPerLine = (ScePvfU16)g->w;
	im.buffer = g->a;
	if (scePvfGetCharGlyphImage(f, (ScePvfCharCode)cp, &im) < 0) {
		free(g->a);
		g->a = NULL;
		return -1;
	}
	return 0;
}

int be_fallback_glyph(uint32_t cp, float size, int bold, FbGlyph *g)
{
	if (!g_pvf)
		return -1;
	if (pvf_render(bold && g_pf[PF_LATIN_BOLD] ? PF_LATIN_BOLD : PF_LATIN, cp, size, g) == 0)
		return 0;
	for (int i = 0; g_pf_order[i] >= 0; i++)
		if (pvf_render(g_pf_order[i], cp, size, g) == 0)
			return 0;
	return -1;
}
