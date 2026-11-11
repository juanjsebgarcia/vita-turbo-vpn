/*
 * draw.c - shapes, icons and text, rasterised to cached coverage masks.
 *
 * Rounded rectangles are drawn as four cached corner masks plus plain
 * rectangles, so a card of any size costs four small masks. Icons are
 * flattened vector paths. Text uses stb_truetype with four subpixel
 * positions per glyph. Metrics follow the browser's: the line box is the
 * rounded ascent plus the rounded descent, with no line gap.
 *
 * A character the style's font does not have comes from the platform's
 * fallback (draw_backend.h): the system fonts on the console. Capitals for
 * labels cover Latin, Cyrillic and Turkish dotted I.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"
#include "draw_backend.h"
#include "stb_truetype.h"
#ifdef HOST_BUILD
#include "draw_host.h"
#endif

#define DRAW_PI 3.14159265358979f

/* ---- mask cache -------------------------------------------------------- */

#define CACHE_BUCKETS  2048
#define CACHE_MAX_BYTES (6u << 20)

static Mask *g_cache[CACHE_BUCKETS];
static size_t g_cache_bytes;

enum { K_CORNER = 1, K_DISC, K_RING, K_DASHED, K_ICON, K_GLYPH, K_FALLBACK };

static uint32_t key_hash(const uint32_t *k)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < 6; i++) {
		h ^= k[i];
		h *= 16777619u;
	}
	return h;
}

static void cache_flush(void)
{
	for (int i = 0; i < CACHE_BUCKETS; i++) {
		Mask *m = g_cache[i];
		while (m) {
			Mask *n = m->next;
			free(m->a);
			free(m);
			m = n;
		}
		g_cache[i] = NULL;
	}
	g_cache_bytes = 0;
	be_masks_flushed();
}

static Mask *cache_find(const uint32_t *k)
{
	for (Mask *m = g_cache[key_hash(k) % CACHE_BUCKETS]; m; m = m->next)
		if (!memcmp(m->key, k, sizeof(m->key)))
			return m;
	return NULL;
}

/* A new zeroed mask, added to the cache under key k */
static Mask *cache_new(const uint32_t *k, int w, int h)
{
	Mask *m = calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	m->a = calloc((size_t)w * h, 1);
	if (!m->a) {
		free(m);
		return NULL;
	}
	memcpy(m->key, k, sizeof(m->key));
	m->w = w;
	m->h = h;
	uint32_t b = key_hash(k) % CACHE_BUCKETS;
	m->next = g_cache[b];
	g_cache[b] = m;
	g_cache_bytes += sizeof(*m) + (size_t)w * h;
	return m;
}

static inline uint8_t cov8(float c)
{
	if (c <= 0.0f)
		return 0;
	if (c >= 1.0f)
		return 255;
	return (uint8_t)(c * 255.0f + 0.5f);
}

static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static inline uint32_t fx(float v)
{
	return (uint32_t)(int32_t)lroundf(v * 64.0f);
}

/* ---- shapes ------------------------------------------------------------ */

/* Signed distance from p to a rounded box centred on the origin */
static float sd_rbox(float px, float py, float hw, float hh, float r)
{
	float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;
	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

/*
 * One corner (0 tl, 1 tr, 2 bl, 3 br) of a rounded rectangle of radius r:
 * filled when stroke is 0, else a border of that width inside the edge.
 */
static Mask *corner_mask(float r, float stroke, int corner)
{
	uint32_t k[6] = { K_CORNER, fx(r), fx(stroke), (uint32_t)corner, 0, 0 };
	Mask *m = cache_find(k);
	if (m)
		return m;
	int n = (int)ceilf(r);
	m = cache_new(k, n, n);
	if (!m)
		return NULL;
	/* the corner of a 2n x 2n box, its centre at (n, n) */
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			float px = (corner & 1) ? x + 0.5f : x + 0.5f - n;
			float py = (corner & 2) ? y + 0.5f : y + 0.5f - n;
			float c = clamp01(0.5f - sd_rbox(px, py, n, n, r));
			if (stroke > 0) {
				float ri = r - stroke > 0 ? r - stroke : 0;
				c *= 1.0f - clamp01(0.5f - sd_rbox(px, py, n - stroke, n - stroke, ri));
			}
			m->a[y * n + x] = cov8(c);
		}
	return m;
}

void draw_rect(float x, float y, float w, float h, Colour c)
{
	int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
	int x1 = (int)lroundf(x + w), y1 = (int)lroundf(y + h);
	if (x1 > x0 && y1 > y0)
		be_fill(x0, y0, x1 - x0, y1 - y0, c);
}

static void rrect(float x, float y, float w, float h, float r, float stroke, Colour c)
{
	int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
	int x1 = (int)lroundf(x + w), y1 = (int)lroundf(y + h);
	int W = x1 - x0, H = y1 - y0;
	if (W <= 0 || H <= 0)
		return;
#ifdef HOST_BUILD
	host_trace_box(x0, y0, W, H);
	if (stroke <= 0 && (c >> 24) == 0xFF)
		host_trace_cover(x0, y0, W, H);
#endif
	if (r > W / 2.0f)
		r = W / 2.0f;
	if (r > H / 2.0f)
		r = H / 2.0f;
	int n = (int)ceilf(r);
	if (n > 0) {
		Mask *m;
		if ((m = corner_mask(r, stroke, 0)))
			be_mask(m, x0, y0, c);
		if ((m = corner_mask(r, stroke, 1)))
			be_mask(m, x1 - n, y0, c);
		if ((m = corner_mask(r, stroke, 2)))
			be_mask(m, x0, y1 - n, c);
		if ((m = corner_mask(r, stroke, 3)))
			be_mask(m, x1 - n, y1 - n, c);
	}
	if (stroke <= 0) {
		if (W - 2 * n > 0) {
			be_fill(x0 + n, y0, W - 2 * n, n, c);
			be_fill(x0 + n, y1 - n, W - 2 * n, n, c);
		}
		if (H - 2 * n > 0)
			be_fill(x0, y0 + n, W, H - 2 * n, c);
	} else {
		int s = (int)lroundf(stroke);
		if (W - 2 * n > 0) {
			be_fill(x0 + n, y0, W - 2 * n, s, c);
			be_fill(x0 + n, y1 - s, W - 2 * n, s, c);
		}
		if (H - 2 * n > 0) {
			be_fill(x0, y0 + n, s, H - 2 * n, c);
			be_fill(x1 - s, y0 + n, s, H - 2 * n, c);
		}
	}
}

void draw_rrect(float x, float y, float w, float h, float r, Colour c)
{
	rrect(x, y, w, h, r, 0, c);
}

void draw_rrect_stroke(float x, float y, float w, float h, float r, float stroke, Colour c)
{
	rrect(x, y, w, h, r, stroke, c);
}

/*
 * Dashes along the centre line of a rounded border. Each pixel of the ring
 * is placed at its distance along the perimeter, measured clockwise from the
 * top-left end of the top edge, and kept if that falls in a dash.
 */
void draw_rrect_dashed(float x, float y, float w, float h, float r, float stroke,
	float dash, float gap, Colour c)
{
	int x0 = (int)lroundf(x), y0 = (int)lroundf(y);
	int W = (int)lroundf(x + w) - x0, H = (int)lroundf(y + h) - y0;
	if (W <= 0 || H <= 0)
		return;
#ifdef HOST_BUILD
	host_trace_box(x0, y0, W, H);
#endif
	uint32_t k[6] = { K_DASHED, (uint32_t)(W << 16 | H), fx(r), fx(stroke), fx(dash), fx(gap) };
	Mask *m = cache_find(k);
	if (!m && (m = cache_new(k, W, H))) {
		float hw = W / 2.0f, hh = H / 2.0f;
		float rc = r - stroke / 2;               /* radius of the centre line */
		float sw = W - 2 * r, sh = H - 2 * r;    /* straight edge lengths */
		float arc = DRAW_PI / 2 * rc;
		float per = 2 * sw + 2 * sh + 4 * arc;
		/* fit a whole number of periods, as browsers do */
		int np = (int)lroundf(per / (dash + gap));
		float scale = np > 0 ? per / (np * (dash + gap)) : 1;
		float d = dash * scale, p = (dash + gap) * scale;
		for (int py = 0; py < H; py++)
			for (int px = 0; px < W; px++) {
				float cx = px + 0.5f - hw, cy = py + 0.5f - hh;
				float c1 = clamp01(0.5f - sd_rbox(cx, cy, hw, hh, r));
				float ri = r - stroke > 0 ? r - stroke : 0;
				c1 *= 1.0f - clamp01(0.5f - sd_rbox(cx, cy, hw - stroke, hh - stroke, ri));
				if (c1 <= 0)
					continue;
				/* position along the perimeter */
				float ax = cx + sw / 2, bx = cx - sw / 2;   /* relative to arc centres */
				float ay = cy + sh / 2, by = cy - sh / 2;
				float t;
				if (cy < 0 && fabsf(cx) <= sw / 2)
					t = cx + sw / 2;
				else if (cx > 0 && fabsf(cy) <= sh / 2)
					t = sw + arc + (cy + sh / 2);
				else if (cy > 0 && fabsf(cx) <= sw / 2)
					t = sw + 2 * arc + sh + (sw / 2 - cx);
				else if (cx < 0 && fabsf(cy) <= sh / 2)
					t = 2 * sw + 3 * arc + sh + (sh / 2 - cy);
				else if (cx > 0 && cy < 0)
					t = sw + rc * atan2f(bx, -ay);
				else if (cx > 0)
					t = sw + arc + sh + rc * atan2f(by, bx);
				else if (cy > 0)
					t = 2 * sw + 2 * arc + sh + rc * atan2f(-ax, by);
				else
					t = 2 * sw + 3 * arc + 2 * sh + rc * atan2f(-ay, -ax);
				float f = fmodf(t + p * 0.5f - d * 0.5f, p);
				if (f < 0)
					f += p;
				if (f < d)
					m->a[py * W + px] = cov8(c1);
			}
	}
	if (m)
		be_mask(m, x0, y0, c);
}

void draw_dashed(float x, float y, float w, float h, float dash, float gap, Colour c)
{
	if (w >= h) {
		for (float p = 0; p < w; p += dash + gap)
			draw_rect(x + p, y, (p + dash > w ? w - p : dash), h, c);
	} else {
		for (float p = 0; p < h; p += dash + gap)
			draw_rect(x, y + p, w, (p + dash > h ? h - p : dash), c);
	}
}

static Mask *disc_mask(float r, float stroke)
{
	uint32_t k[6] = { stroke > 0 ? K_RING : K_DISC, fx(r), fx(stroke), 0, 0, 0 };
	Mask *m = cache_find(k);
	if (m)
		return m;
	int n = (int)ceilf(2 * r);
	m = cache_new(k, n, n);
	if (!m)
		return NULL;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			float dx = x + 0.5f - n / 2.0f, dy = y + 0.5f - n / 2.0f;
			float d = sqrtf(dx * dx + dy * dy);
			float c = clamp01(r - d + 0.5f);
			if (stroke > 0)
				c *= 1.0f - clamp01(r - stroke - d + 0.5f);
			m->a[y * n + x] = cov8(c);
		}
	return m;
}

void draw_circle(float cx, float cy, float r, Colour c)
{
	Mask *m = disc_mask(r, 0);
	if (m)
		be_mask(m, (int)lroundf(cx - m->w / 2.0f), (int)lroundf(cy - m->h / 2.0f), c);
}

void draw_ring(float cx, float cy, float r, float stroke, Colour c)
{
	Mask *m = disc_mask(r, stroke);
	if (m)
		be_mask(m, (int)lroundf(cx - m->w / 2.0f), (int)lroundf(cy - m->h / 2.0f), c);
}

/* ---- icons ------------------------------------------------------------- */

/*
 * Icons are small programs over a view box, as in the design's SVGs:
 * subpaths are collected until a STROKE or FILL paints them.
 */
enum { OP_END, OP_M, OP_L, OP_C, OP_Z, OP_RECT, OP_CIRC, OP_ARC, OP_STROKE, OP_FILL };

typedef struct IconDef {
	float vw, vh;
	const float *ops;
} IconDef;

static const float ic_mark[] = {
	OP_RECT, 1, 1, 20, 20, 4, OP_RECT, 6, 6, 10, 10, 2, OP_STROKE, 2, 0, OP_END };
static const float ic_vita[] = {
	OP_RECT, 1, 1, 24, 14, 7, OP_RECT, 8, 4, 10, 8, 1, OP_STROKE, 1.6f, 0, OP_END };
static const float ic_lock[] = {
	OP_RECT, 1, 6, 10, 7, 1, OP_FILL,
	OP_M, 3, 6, OP_L, 3, 4, OP_ARC, 6, 4, 3, 180, 360, OP_L, 9, 6, OP_STROKE, 1.8f, 0, OP_END };
static const float ic_house[] = {
	OP_M, 4, 18, OP_L, 4, 6, OP_L, 11, 2, OP_L, 18, 6, OP_L, 18, 18,
	OP_M, 8, 18, OP_L, 8, 12, OP_L, 14, 12, OP_L, 14, 18, OP_STROKE, 1.6f, 0, OP_END };
static const float ic_globe[] = {
	OP_CIRC, 11, 11, 9, OP_STROKE, 1.6f, 0,
	OP_M, 2, 11, OP_L, 20, 11,
	OP_M, 11, 2, OP_C, 14, 5, 14, 17, 11, 20,
	OP_M, 11, 2, OP_C, 8, 5, 8, 17, 11, 20, OP_STROKE, 1.4f, 0, OP_END };
static const float ic_check[] = {
	OP_M, 2, 7.5f, OP_L, 5, 10.5f, OP_L, 12, 3.5f, OP_STROKE, 2, 0, OP_END };
static const float ic_warn[] = {
	OP_M, 7, 1.5f, OP_L, 13, 12.5f, OP_L, 1, 12.5f, OP_Z, OP_STROKE, 1.6f, 0,
	OP_M, 7, 5.5f, OP_L, 7, 8.5f, OP_M, 7, 10.3f, OP_L, 7, 10.5f, OP_STROKE, 1.6f, 1, OP_END };
static const float ic_cross_small[] = {
	OP_M, 2, 2, OP_L, 10, 10, OP_M, 10, 2, OP_L, 2, 10, OP_STROKE, 2, 1, OP_END };
static const float ic_break[] = {
	OP_M, 5, 3, OP_L, 13, 15, OP_M, 13, 3, OP_L, 5, 15, OP_STROKE, 2, 1, OP_END };
static const float ic_btn_cross[] = {
	OP_M, 3, 3, OP_L, 13, 13, OP_M, 13, 3, OP_L, 3, 13, OP_STROKE, 2.2f, 1, OP_END };
static const float ic_btn_triangle[] = {
	OP_M, 8, 2.5f, OP_L, 14, 13, OP_L, 2, 13, OP_Z, OP_STROKE, 2, 0, OP_END };
static const float ic_btn_square[] = {
	OP_RECT, 2.5f, 2.5f, 11, 11, 0, OP_STROKE, 2, 0, OP_END };
static const float ic_btn_circle[] = {
	OP_CIRC, 8, 8, 5.5f, OP_STROKE, 2, 0, OP_END };

static const IconDef g_icons[ICON_COUNT] = {
	[ICON_MARK]         = { 22, 22, ic_mark },
	[ICON_VITA]         = { 26, 16, ic_vita },
	[ICON_LOCK]         = { 12, 14, ic_lock },
	[ICON_HOUSE]        = { 22, 22, ic_house },
	[ICON_GLOBE]        = { 22, 22, ic_globe },
	[ICON_CHECK]        = { 14, 14, ic_check },
	[ICON_WARN]         = { 14, 14, ic_warn },
	[ICON_CROSS_SMALL]  = { 12, 12, ic_cross_small },
	[ICON_BREAK]        = { 18, 18, ic_break },
	[ICON_BTN_CROSS]    = { 16, 16, ic_btn_cross },
	[ICON_BTN_TRIANGLE] = { 16, 16, ic_btn_triangle },
	[ICON_BTN_SQUARE]   = { 16, 16, ic_btn_square },
	[ICON_BTN_CIRCLE]   = { 16, 16, ic_btn_circle },
};

#define MAX_PTS  768
#define MAX_SUB  24

typedef struct Flat {
	float x[MAX_PTS], y[MAX_PTS];
	int start[MAX_SUB + 1];   /* first point of each subpath */
	uint8_t closed[MAX_SUB];
	int nsub, npts;
} Flat;

static void flat_point(Flat *f, float x, float y)
{
	if (f->npts < MAX_PTS) {
		f->x[f->npts] = x;
		f->y[f->npts] = y;
		f->npts++;
	}
}

static void flat_sub(Flat *f)
{
	if (f->nsub < MAX_SUB) {
		f->start[f->nsub] = f->npts;
		f->closed[f->nsub] = 0;
		f->nsub++;
	}
}

static void flat_arc(Flat *f, float cx, float cy, float r, float a0, float a1, int steps)
{
	for (int i = 0; i <= steps; i++) {
		float a = (a0 + (a1 - a0) * i / steps) * DRAW_PI / 180.0f;
		flat_point(f, cx + r * cosf(a), cy + r * sinf(a));
	}
}

static float seg_dist2(float px, float py, float ax, float ay, float bx, float by, int cap_a, int cap_b)
{
	float dx = bx - ax, dy = by - ay;
	float l2 = dx * dx + dy * dy;
	float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
	/* a butt cap ends the stroke square at the end point */
	if ((t < 0 && !cap_a) || (t > 1 && !cap_b))
		return 1e9f;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	float ex = ax + t * dx - px, ey = ay + t * dy - py;
	return ex * ex + ey * ey;
}

/* Paint the collected subpaths into the mask (union with what is there) */
static void flat_paint(const Flat *f, Mask *m, float s, float offx, float offy,
	int fill, float width, int round)
{
	for (int py = 0; py < m->h; py++)
		for (int px = 0; px < m->w; px++) {
			float c = 0;
			if (fill) {
				/* 4x4 samples, even-odd */
				int in = 0;
				for (int sy = 0; sy < 4; sy++)
					for (int sx = 0; sx < 4; sx++) {
						float x = ((px + (sx + 0.5f) / 4) - offx) / s;
						float y = ((py + (sy + 0.5f) / 4) - offy) / s;
						int odd = 0;
						for (int i = 0; i < f->nsub; i++) {
							int a = f->start[i], b = f->start[i + 1];
							for (int j = a, k = b - 1; j < b; k = j++)
								if ((f->y[j] > y) != (f->y[k] > y) &&
								    x < (f->x[k] - f->x[j]) * (y - f->y[j]) / (f->y[k] - f->y[j]) + f->x[j])
									odd ^= 1;
						}
						in += odd;
					}
				c = in / 16.0f;
			} else {
				float x = (px + 0.5f - offx) / s, y = (py + 0.5f - offy) / s;
				float best = 1e9f;
				for (int i = 0; i < f->nsub; i++) {
					int a = f->start[i], b = f->start[i + 1];
					int closed = f->closed[i];
					for (int j = a; j + 1 < b; j++) {
						int cap_a = round || closed || j > a;
						int cap_b = round || closed || j + 2 < b;
						float d = seg_dist2(x, y, f->x[j], f->y[j], f->x[j + 1], f->y[j + 1], cap_a, cap_b);
						if (d < best)
							best = d;
					}
					if (closed && b - a > 1) {
						float d = seg_dist2(x, y, f->x[b - 1], f->y[b - 1], f->x[a], f->y[a], 1, 1);
						if (d < best)
							best = d;
					}
				}
				c = clamp01((width / 2 - sqrtf(best)) * s + 0.5f);
			}
			uint8_t v = cov8(c);
			if (v > m->a[py * m->w + px])
				m->a[py * m->w + px] = v;
		}
}

static Mask *icon_mask(int icon, int w, int h)
{
	uint32_t k[6] = { K_ICON, (uint32_t)icon, (uint32_t)w, (uint32_t)h, 0, 0 };
	Mask *m = cache_find(k);
	if (m || icon < 0 || icon >= ICON_COUNT)
		return m;
	m = cache_new(k, w, h);
	if (!m)
		return NULL;
	const IconDef *d = &g_icons[icon];
	/* preserveAspectRatio="xMidYMid meet" */
	float s = w / d->vw < h / d->vh ? w / d->vw : h / d->vh;
	float offx = (w - d->vw * s) / 2, offy = (h - d->vh * s) / 2;

	static Flat f;
	memset(&f, 0, sizeof(f));
	float cx = 0, cy = 0;
	for (const float *op = d->ops; *op != OP_END;) {
		switch ((int)*op) {
		case OP_M:
			flat_sub(&f);
			cx = op[1];
			cy = op[2];
			flat_point(&f, cx, cy);
			op += 3;
			break;
		case OP_L:
			cx = op[1];
			cy = op[2];
			flat_point(&f, cx, cy);
			op += 3;
			break;
		case OP_C:
			for (int i = 1; i <= 16; i++) {
				float t = i / 16.0f, u = 1 - t;
				float x = u * u * u * cx + 3 * u * u * t * op[1] + 3 * u * t * t * op[3] + t * t * t * op[5];
				float y = u * u * u * cy + 3 * u * u * t * op[2] + 3 * u * t * t * op[4] + t * t * t * op[6];
				flat_point(&f, x, y);
			}
			cx = op[5];
			cy = op[6];
			op += 7;
			break;
		case OP_Z:
			if (f.nsub)
				f.closed[f.nsub - 1] = 1;
			op += 1;
			break;
		case OP_RECT: {
			float x = op[1], y = op[2], rw = op[3], rh = op[4], r = op[5];
			flat_sub(&f);
			if (r > 0) {
				flat_arc(&f, x + rw - r, y + r, r, -90, 0, 8);
				flat_arc(&f, x + rw - r, y + rh - r, r, 0, 90, 8);
				flat_arc(&f, x + r, y + rh - r, r, 90, 180, 8);
				flat_arc(&f, x + r, y + r, r, 180, 270, 8);
			} else {
				flat_point(&f, x, y);
				flat_point(&f, x + rw, y);
				flat_point(&f, x + rw, y + rh);
				flat_point(&f, x, y + rh);
			}
			f.closed[f.nsub - 1] = 1;
			op += 6;
			break;
		}
		case OP_CIRC:
			flat_sub(&f);
			flat_arc(&f, op[1], op[2], op[3], 0, 360, 48);
			f.closed[f.nsub - 1] = 1;
			op += 4;
			break;
		case OP_ARC:
			flat_arc(&f, op[1], op[2], op[3], op[4], op[5], 16);
			cx = f.x[f.npts - 1];
			cy = f.y[f.npts - 1];
			op += 6;
			break;
		case OP_STROKE:
		case OP_FILL: {
			int fill = (int)*op == OP_FILL;
			f.start[f.nsub] = f.npts;
			flat_paint(&f, m, s, offx, offy, fill, fill ? 0 : op[1], fill ? 0 : (int)op[2]);
			memset(&f, 0, sizeof(f));
			op += fill ? 1 : 3;
			break;
		}
		default:
			return m;
		}
	}
	return m;
}

void draw_icon(int icon, float x, float y, float w, float h, Colour c)
{
	Mask *m = icon_mask(icon, (int)lroundf(w), (int)lroundf(h));
	if (m)
		be_mask(m, (int)lroundf(x), (int)lroundf(y), c);
}

/* ---- text -------------------------------------------------------------- */

static const char *const g_font_files[FONT_COUNT] = {
	[FONT_DISPLAY]      = "ChakraPetch-SemiBold.ttf",
	[FONT_DISPLAY_BOLD] = "ChakraPetch-Bold.ttf",
	[FONT_TEXT]         = "BarlowSemiCondensed-Regular.ttf",
	[FONT_MONO]         = "MartianMono-StdRg.ttf",
	[FONT_MONO_MEDIUM]  = "MartianMono-StdMd.ttf",
};

typedef struct Font {
	stbtt_fontinfo info;
	unsigned char *data;
	int ascent, descent;   /* font units, descent positive */
	float em;              /* scale for 1 px em */
	int no_cyrillic;       /* lacks Cyrillic: Russian text uses the fallback throughout */
} Font;

static Font g_fonts[FONT_COUNT];

uint32_t utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	int extra = 0;
	if (c >= 0xF0 && c < 0xF8) {
		c &= 0x07;
		extra = 3;
	} else if (c >= 0xE0) {
		c &= 0x0F;
		extra = 2;
	} else if (c >= 0xC0) {
		c &= 0x1F;
		extra = 1;
	} else if (c >= 0x80) {
		c = 0xFFFD;
	}
	while (extra-- > 0) {
		if ((*p & 0xC0) != 0x80) {
			c = 0xFFFD;
			break;
		}
		c = (c << 6) | (*p++ & 0x3F);
	}
	*s = (const char *)p;
	return c;
}

static int g_lang = 1;          /* English (US) */
static int g_missing;
static uint32_t g_missing_last;

#define LANG_RUSSIAN 8
#define LANG_TURKISH 19

void draw_set_language(int lang)
{
	g_lang = lang;
}

int draw_missing_glyphs(uint32_t *last)
{
	int n = g_missing;
	if (last)
		*last = g_missing_last;
	g_missing = 0;
	return n;
}

/*
 * Names that keep the English capital I in Turkish (VITA, not VİTA), as
 * whole words, case-insensitive.
 */
static const char *const g_tr_names[] = { "vita", "kill", "switch", "wi", "fi" };

/* Does the word starting at s (ASCII letters) match a name above? */
static int tr_name_at(const char *s)
{
	size_t n = 0;
	while ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z'))
		n++;
	if ((unsigned char)s[n] >= 0x80)
		return 0;   /* the word goes on in non-ASCII letters: Turkish */
	for (size_t i = 0; i < sizeof(g_tr_names) / sizeof(g_tr_names[0]); i++) {
		const char *w = g_tr_names[i];
		size_t k = 0;
		while (k < n && w[k] && (s[k] | 0x20) == w[k])
			k++;
		if (k == n && !w[k])
			return 1;
	}
	return 0;
}

static uint32_t upper(uint32_t c, int keep_i)
{
	if (c == 'i' && g_lang == LANG_TURKISH && !keep_i)
		return 0x130;                       /* dotted capital I */
	if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
		return c - 0x20;
	if (c == 0xFF)
		return 0x178;
	if (c == 0x131)
		return 'I';                         /* dotless i */
	if (c >= 0x100 && c <= 0x17E && c != 0x130 && c != 0x138 && c != 0x149) {
		/* Latin Extended-A: pairs, capital first; the capital is odd
		 * in 0139-0148 and 0179-017E, even elsewhere */
		int odd_caps = (c >= 0x139 && c <= 0x148) || c >= 0x179;
		if ((c & 1) != (uint32_t)odd_caps)
			return c - 1;
		return c;
	}
	if (c >= 0x430 && c <= 0x44F)           /* Cyrillic */
		return c - 0x20;
	if (c >= 0x450 && c <= 0x45F)
		return c - 0x50;
	return c;
}

int draw_init(const char *font_dir)
{
	for (int i = 0; i < FONT_COUNT; i++) {
		char path[256];
		size_t n;
		strncpy(path, font_dir, sizeof(path) - 64);
		path[sizeof(path) - 64] = 0;
		strcat(path, g_font_files[i]);
		Font *f = &g_fonts[i];
		f->data = be_load_file(path, &n);
		if (!f->data || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)))
			return -1;
		int a, d, g;
		stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
		f->ascent = a;
		f->descent = -d;
		f->em = stbtt_ScaleForMappingEmToPixels(&f->info, 1.0f);
		f->no_cyrillic = stbtt_FindGlyphIndex(&f->info, 0x0416) == 0;
	}
	be_fallback_init(font_dir, g_lang);
	return 0;
}

void draw_shutdown(void)
{
	cache_flush();
	be_fallback_shutdown();
	for (int i = 0; i < FONT_COUNT; i++) {
		free(g_fonts[i].data);
		g_fonts[i].data = NULL;
	}
}

float text_ascent(const TextStyle *st)
{
	const Font *f = &g_fonts[st->font];
	return roundf(f->ascent * f->em * st->size);
}

float text_height(const TextStyle *st)
{
	const Font *f = &g_fonts[st->font];
	return roundf(f->ascent * f->em * st->size) + roundf(f->descent * f->em * st->size);
}

static Mask *glyph_mask(const TextStyle *st, int glyph, int sub)
{
	uint32_t k[6] = { K_GLYPH, st->font, fx(st->size), (uint32_t)glyph, (uint32_t)sub, 0 };
	Mask *m = cache_find(k);
	if (m)
		return m;
	const Font *f = &g_fonts[st->font];
	float sc = f->em * st->size, shift = sub / 4.0f;
	int x0, y0, x1, y1;
	stbtt_GetGlyphBitmapBoxSubpixel(&f->info, glyph, sc, sc, shift, 0, &x0, &y0, &x1, &y1);
	m = cache_new(k, x1 - x0, y1 - y0);
	if (!m)
		return NULL;
	m->ox = x0;
	m->oy = y0;
	if (x1 > x0 && y1 > y0)
		stbtt_MakeGlyphBitmapSubpixel(&f->info, m->a, m->w, m->h, m->w, sc, sc, shift, 0, glyph);
	return m;
}

/* The display faces take the bold fallback */
static int font_bold(int font)
{
	return font == FONT_DISPLAY || font == FONT_DISPLAY_BOLD;
}

/* A character from the platform's fallback fonts, cached; NULL if none has it */
static Mask *fallback_mask(const TextStyle *st, uint32_t c)
{
	int bold = font_bold(st->font);
	uint32_t k[6] = { K_FALLBACK, (uint32_t)bold, fx(st->size), c, 0, 0 };
	Mask *m = cache_find(k);
	if (m)
		return m->adv >= 0 ? m : NULL;
	FbGlyph g;
	memset(&g, 0, sizeof(g));
	if (be_fallback_glyph(c, st->size, bold, &g) != 0) {
		/* remember the miss as an empty mask */
		free(g.a);
		m = cache_new(k, 1, 1);
		if (m)
			m->adv = -1;
		return NULL;
	}
	m = cache_new(k, 1, 1);
	if (!m) {
		free(g.a);
		return NULL;
	}
	if (g.a && g.w > 0 && g.h > 0) {
		free(m->a);
		m->a = g.a;
		m->w = g.w;
		m->h = g.h;
		g_cache_bytes += (size_t)g.w * g.h;
	} else {
		free(g.a);
	}
	m->ox = g.ox;
	m->oy = g.oy;
	m->adv = g.adv;
	return m;
}

/* Characters that are formatting, not glyphs */
static int invisible(uint32_t c)
{
	return c < 0x20 || c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF;
}

/* Lay out (and if draw, draw) s from x on baseline; returns the advance */
static float text_run(const TextStyle *st, float x, float baseline, const char *s, int draw)
{
	const Font *f = &g_fonts[st->font];
	if (!f->data || !s)
		return 0;
	float sc = f->em * st->size, track = st->tracking * st->size, pen = 0;
	int prev = 0;
	/* In Russian, a face without Cyrillic hands its Latin letters and digits
	 * to the fallback too, so one line does not mix two typefaces */
	int all_fallback = g_lang == LANG_RUSSIAN && f->no_cyrillic;
	int keep_i = 0, in_word = 0;
	while (*s) {
		int letter = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
			(unsigned char)*s >= 0x80;
		if (st->upper && g_lang == LANG_TURKISH && letter && !in_word)
			keep_i = tr_name_at(s);
		in_word = letter;
		uint32_t c = utf8_next(&s);
		if (st->upper)
			c = upper(c, keep_i);
		if (invisible(c))
			continue;
		int g = all_fallback ? 0 : stbtt_FindGlyphIndex(&f->info, (int)c);
		if (g == 0 && all_fallback && !fallback_mask(st, c))
			g = stbtt_FindGlyphIndex(&f->info, (int)c);
		if (g == 0) {
			Mask *m = fallback_mask(st, c);
			if (m) {
				if (draw && m->w > 1)
					be_mask(m, (int)lroundf(x + pen) + m->ox, (int)lroundf(baseline) + m->oy,
						st->colour);
				pen += m->adv + track;
				prev = 0;
				continue;
			}
			if (draw) {
				g_missing++;
				g_missing_last = c;
			}
		}
		if (prev)
			pen += stbtt_GetGlyphKernAdvance(&f->info, prev, g) * sc;
		if (draw) {
			float gx = x + pen;
			int ix = (int)floorf(gx);
			int sub = (int)((gx - ix) * 4.0f);
			Mask *m = glyph_mask(st, g, sub);
			if (m && m->w > 1)
				be_mask(m, ix + m->ox, (int)lroundf(baseline) + m->oy, st->colour);
		}
		int adv, lsb;
		stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
		pen += adv * sc + track;
		prev = g;
	}
	return pen;
}

float text_width(const TextStyle *st, const char *s)
{
	return text_run(st, 0, 0, s, 0);
}

float draw_text_baseline(const TextStyle *st, float x, float baseline, const char *s)
{
	float w = text_run(st, x, baseline, s, 1);
#ifdef HOST_BUILD
	host_trace_text(x, baseline - text_ascent(st), w, text_height(st), s);
#endif
	return w;
}

float draw_text(const TextStyle *st, float x, float top, const char *s)
{
	return draw_text_baseline(st, x, top + text_ascent(st), s);
}

/* ---- frame ------------------------------------------------------------- */

void draw_begin(Colour clear)
{
	if (g_cache_bytes > CACHE_MAX_BYTES)
		cache_flush();
	be_begin(clear);
}

void draw_end(void)
{
	be_end();
}

void draw_clip(float x, float y, float w, float h)
{
	be_clip((int)lroundf(x), (int)lroundf(y), (int)lroundf(w), (int)lroundf(h));
}

void draw_unclip(void)
{
	be_unclip();
}
