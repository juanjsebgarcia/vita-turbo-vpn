/*
 * ui.c - the design's building blocks (see ui.h). Measurements are the
 * design's, converted to positions.
 */
#include <string.h>

#include "ui.h"

#ifdef HOST_BUILD
#include "draw_host.h"
#endif

#define BLACK RGBHEX(0x000000)

const TextStyle ST_MARK       = { FONT_DISPLAY,      0, 24, 0,     C_TEXT };
const TextStyle ST_TITLE      = { FONT_DISPLAY,      0, 24, 0,     C_TEXT };
const TextStyle ST_CHIP       = { FONT_MONO,         0, 16, 0,     C_MUTED };
const TextStyle ST_CHIP_WARN  = { FONT_MONO,         0, 16, 0,     C_ORANGE };
const TextStyle ST_EYEBROW    = { FONT_DISPLAY,      1, 16, 0.12f, C_MUTED };
const TextStyle ST_STATE      = { FONT_DISPLAY_BOLD, 1, 60, 0,     C_TEXT };
const TextStyle ST_STATE_S    = { FONT_DISPLAY_BOLD, 1, 52, 0,     C_TEXT };
const TextStyle ST_SUB        = { FONT_TEXT,         0, 22, 0,     C_TEXT };
const TextStyle ST_SUB_MUTED  = { FONT_TEXT,         0, 22, 0,     C_MUTED };
const TextStyle ST_NLAB       = { FONT_DISPLAY,      1, 16, 0.1f,  C_MUTED };
const TextStyle ST_SLAB_MONO  = { FONT_MONO,         0, 16, 0,     C_MUTED };
const TextStyle ST_SLAB       = { FONT_TEXT,         0, 18, 0,     C_MUTED };
const TextStyle ST_TUBE       = { FONT_DISPLAY_BOLD, 1, 16, 0.12f, BLACK };
const TextStyle ST_TUBE_MUTED = { FONT_DISPLAY_BOLD, 1, 16, 0.12f, C_MUTED };
const TextStyle ST_BIG        = { FONT_MONO_MEDIUM,  0, 26, 0,     C_TEXT };
const TextStyle ST_MONO26     = { FONT_MONO,         0, 26, 0,     C_TEXT };
const TextStyle ST_MONO18     = { FONT_MONO,         0, 18, 0,     C_TEXT };
const TextStyle ST_MONO16     = { FONT_MONO,         0, 16, 0,     C_TEXT };
const TextStyle ST_OK         = { FONT_TEXT,         0, 20, 0,     C_LIME };
const TextStyle ST_HINT       = { FONT_TEXT,         0, 22, 0,     C_TEXT };
const TextStyle ST_NAME       = { FONT_DISPLAY,      0, 26, 0,     C_TEXT };
const TextStyle ST_DETAIL     = { FONT_TEXT,         0, 18, 0,     C_MUTED };
const TextStyle ST_ST         = { FONT_TEXT,         0, 20, 0,     C_TEXT };
const TextStyle ST_OPT_NAME   = { FONT_DISPLAY,      0, 28, 0,     C_TEXT };
const TextStyle ST_VERDICT    = { FONT_DISPLAY_BOLD, 1, 18, 0.12f, C_TEXT };
const TextStyle ST_TEXT22     = { FONT_TEXT,         0, 22, 0,     C_TEXT };
const TextStyle ST_TEXT20     = { FONT_TEXT,         0, 20, 0,     C_TEXT };
const TextStyle ST_TEXT18     = { FONT_TEXT,         0, 18, 0,     C_TEXT };
const TextStyle ST_HEAD_RIGHT = { FONT_TEXT,         0, 20, 0,     C_MUTED };
const TextStyle ST_KEY        = { FONT_TEXT,         0, 20, 0,     C_MUTED };

TextStyle st_col(const TextStyle *st, Colour c)
{
	TextStyle s = *st;
	s.colour = c;
	return s;
}

/* ---- touch targets ----------------------------------------------------- */

#define MAX_HITS 64
static Hit g_hits[MAX_HITS];
static int g_nhits;

void ui_hits_clear(void)
{
	g_nhits = 0;
}

void ui_hit(float x, float y, float w, float h, int action, int arg)
{
	if (g_nhits < MAX_HITS)
		g_hits[g_nhits++] = (Hit){ x, y, w, h, action, arg };
}

const Hit *ui_hit_at(float x, float y)
{
	for (int i = g_nhits - 1; i >= 0; i--) {
		const Hit *h = &g_hits[i];
		if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h)
			return h;
	}
	return NULL;
}

/* ---- rich text --------------------------------------------------------- */

typedef struct Tok {
	const TextStyle *st;
	char s[80];
	float w;
	float sp;      /* width of the space before it; 0 = joined to the previous */
	int brk;       /* a line may break before it although joined (CJK) */
} Tok;

#define MAX_TOKS 256
static Tok g_toks[MAX_TOKS];

/*
 * Chinese and Japanese text has no spaces: a line may break between any
 * two of its characters, except before closing punctuation or after
 * opening punctuation.
 */
static int is_cjk(uint32_t c)
{
	return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
		(c >= 0xFF00 && c <= 0xFFEF);
}

static int no_break_before(uint32_t c)
{
	static const uint32_t list[] = {
		0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF09, 0x300D, 0x300F, 0x3011, 0xFF01, 0xFF1F,
		0xFF1A, 0xFF1B, 0x30FC, 0x30FB, 0x2026, 0x3009, 0x300B, 0x3015,
		0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087,
		0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7,
		')', ']', ',', '.', ':', ';', '!', '?',
	};
	for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
		if (list[i] == c)
			return 1;
	return 0;
}

static int no_break_after(uint32_t c)
{
	return c == 0xFF08 || c == 0x300C || c == 0x300E || c == 0x3010 || c == 0x3008 ||
		c == 0x300A || c == 0x3014 || c == '(' || c == '[';
}

static uint32_t first_char(const char *s)
{
	return utf8_next(&s);
}

static uint32_t last_char(const char *s)
{
	uint32_t c = 0;
	while (*s)
		c = utf8_next(&s);
	return c;
}

static int tokenize(const Span *sp, int n)
{
	int nt = 0;
	float pending = 0;
	int first = 1;
	for (int i = 0; i < n; i++) {
		const char *s = sp[i].s;
		float space = text_width(sp[i].st, " ");
		while (s && *s && nt < MAX_TOKS) {
			if (*s == ' ') {
				pending = space;
				s++;
				continue;
			}
			Tok *t = &g_toks[nt++];
			size_t len = 0;
			const char *q = s;
			if (is_cjk(utf8_next(&q))) {
				len = (size_t)(q - s);      /* one CJK character */
			} else {
				while (s[len] && s[len] != ' ' && len < sizeof(t->s) - 4) {
					q = s + len;
					if (is_cjk(utf8_next(&q)))
						break;
					len = (size_t)(q - s);
				}
			}
			memcpy(t->s, s, len);
			t->s[len] = 0;
			s += len;
			t->st = sp[i].st;
			t->w = text_width(t->st, t->s);
			t->sp = first ? 0 : pending;
			/* glued to the previous token, but never a zero-width break */
			if (!first && pending == 0)
				t->sp = 0;
			t->brk = 0;
			if (!first && t->sp == 0) {
				uint32_t a = last_char(g_toks[nt - 2].s), b = first_char(t->s);
				t->brk = (is_cjk(a) || is_cjk(b)) && !no_break_before(b) && !no_break_after(a);
			}
			pending = 0;
			first = 0;
		}
	}
	return nt;
}

static void draw_line(int a, int b, float x, float baseline, float w, int align)
{
	float lw = 0;
	for (int i = a; i < b; i++)
		lw += (i > a ? g_toks[i].sp : 0) + g_toks[i].w;
	float px = align == ALIGN_RIGHT ? x + w - lw : align == ALIGN_CENTRE ? x + (w - lw) / 2 : x;
	for (int i = a; i < b; i++) {
		if (i > a)
			px += g_toks[i].sp;
		draw_text_baseline(g_toks[i].st, px, baseline, g_toks[i].s);
		px += g_toks[i].w;
	}
}

float spans_para(const Span *sp, int n, float x, float top, float w, float line_h,
	int align, int draw)
{
	if (n <= 0)
		return 0;
	int nt = tokenize(sp, n);
	float h0 = text_height(sp[0].st), a0 = text_ascent(sp[0].st);
	float y = top;
	int start = 0;
	while (start < nt) {
		float lw = g_toks[start].w;
		int i = start + 1;
		for (; i < nt; i++) {
			float add = g_toks[i].sp + g_toks[i].w;
			if (lw + add > w + 0.5f) {
				if (g_toks[i].sp > 0 || g_toks[i].brk)
					break;
				/* a glued piece: break before its whole group if possible */
				int j = i;
				while (j > start && g_toks[j].sp == 0 && !g_toks[j].brk)
					j--;
				if (j > start) {
					i = j;
					break;
				}
			}
			lw += add;
		}
		if (draw)
			draw_line(start, i, x, y + (line_h - h0) / 2 + a0, w, align);
		y += line_h;
		start = i;
	}
	return y - top;
}

float spans_width(const Span *sp, int n)
{
	int nt = tokenize(sp, n);
	float w = 0;
	for (int i = 0; i < nt; i++)
		w += (i ? g_toks[i].sp : 0) + g_toks[i].w;
	return w;
}

void spans_draw(const Span *sp, int n, float x, float top)
{
	if (n > 0)
		spans_para(sp, n, x, top, 1e6f, text_height(sp[0].st), ALIGN_LEFT, 1);
}

int spans_template(const char *tmpl, const TextStyle *base, const char *a0,
	const TextStyle *s0, const char *a1, const TextStyle *s1,
	Span *out, int max, char *buf, size_t bufn)
{
	int n = 0;
	size_t o = 0;
	const char *p = tmpl;
	while (*p && n < max) {
		if (p[0] == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}') {
			out[n].st = p[1] == '0' ? s0 : s1;
			out[n].s = p[1] == '0' ? a0 : a1;
			if (out[n].s && *out[n].s)
				n++;
			p += 3;
			continue;
		}
		const char *q = p;
		while (*q && !(q[0] == '{' && (q[1] == '0' || q[1] == '1') && q[2] == '}'))
			q++;
		size_t len = (size_t)(q - p);
		if (o + len + 1 > bufn) {
#ifdef HOST_BUILD
			host_layout_problem("text cut short: its template buffer is too small");
#endif
			break;
		}
		memcpy(buf + o, p, len);
		buf[o + len] = 0;
		out[n].st = base;
		out[n].s = buf + o;
		n++;
		o += len + 1;
		p = q;
	}
	return n;
}

const char *ui_fit(const TextStyle *st, const char *s, float w, char *out, size_t n)
{
	size_t len = strlen(s);
	if (len >= n)
		len = n - 1;
	memcpy(out, s, len);
	out[len] = 0;
	if (text_width(st, out) <= w)
		return out;
	static const char ell[] = "\xE2\x80\xA6";
	while (len > 0) {
		/* step back one UTF-8 character */
		do
			len--;
		while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
		if (len + sizeof(ell) > n)
			continue;
		memcpy(out + len, ell, sizeof(ell));
		if (text_width(st, out) <= w)
			break;
	}
	return out;
}

/* ---- header and footer ------------------------------------------------- */

#define HEADER_H 60
#define FOOTER_Y 471
#define MARGIN   28

static void header_rule(void)
{
#if BRAND_LEVEL >= 3
	draw_rect(0, HEADER_H - 1, SCREEN_W, 2, C_BRAND);
#else
	draw_rect(0, HEADER_H, SCREEN_W, 1, C_RULE);
#endif
}

void ui_header_mark(int lit, const Chip *chips, int n)
{
#if BRAND_LEVEL >= 1
	(void)lit;   /* the mark is branding; the status word carries the state */
	draw_icon(ICON_MARK, MARGIN, 16, 28, 28, C_BRAND);
#else
	draw_icon(ICON_MARK, MARGIN, 16, 28, 28, lit ? C_LIME : C_MUTED);
#endif
	draw_text(&ST_MARK, 68, (HEADER_H - text_height(&ST_MARK)) / 2, tr(S_PROJECT_NAME));
	float x = SCREEN_W - MARGIN;
	for (int i = n - 1; i >= 0; i--) {
		const TextStyle *st = chips[i].warn ? &ST_CHIP_WARN : &ST_CHIP;
		float tw = text_width(st, chips[i].text);
		float cw = tw + 22, ch = text_height(st) + 10;
		x -= cw;
		float y = (HEADER_H - ch) / 2;
		draw_rrect_stroke(x, y, cw, ch, 4, 1, chips[i].warn ? C_WARN_EDGE : C_CHIP);
		draw_text(st, x + 11, y + 5, chips[i].text);
		x -= 8;
	}
	header_rule();
}

void ui_header_title(const char *title, const Span *right, int n)
{
	draw_text(&ST_TITLE, MARGIN, (HEADER_H - text_height(&ST_TITLE)) / 2, title);
	if (n > 0) {
		float w = spans_width(right, n);
		spans_draw(right, n, SCREEN_W - MARGIN - w, (HEADER_H - text_height(right[0].st)) / 2);
	}
	header_rule();
}

/* BTN_CROSS means "confirm" and BTN_CIRCLE "back": on a console set to
 * confirm with circle the symbols swap with the buttons (main_vita.c). */
int ui_enter_circle;

static int physical(int b)
{
	if (ui_enter_circle && (b == BTN_CROSS || b == BTN_CIRCLE))
		return b == BTN_CROSS ? BTN_CIRCLE : BTN_CROSS;
	return b;
}

int ui_button_icon(int b)
{
	static const int icons[] = { ICON_BTN_CROSS, ICON_BTN_TRIANGLE, ICON_BTN_SQUARE, ICON_BTN_CIRCLE };
	return icons[physical(b) & 3];
}

Colour ui_button_colour(int b)
{
	static const Colour cols[] = { C_BTN_CROSS, C_BTN_TRI, C_BTN_SQUARE, C_BTN_CIRCLE };
	return cols[physical(b) & 3];
}

static const TextStyle ST_KEYCAP = { FONT_DISPLAY, 0, 16, 0.06f, C_MUTED };

/* Width of the button's symbol: the face symbols are 22 px; Start is a key cap */
static float symbol_width(const Hint *h)
{
	if (h->button == BTN_START)
		return text_width(&ST_KEYCAP, tr(S_KEY_START)) + 14;
	return 22;
}

float ui_hint_width(const Hint *h)
{
	return 16 + symbol_width(h) + 10 + text_width(&ST_HINT, h->label) + 16;
}

void ui_hint_draw(const Hint *h, float x, float y)
{
	float sw = symbol_width(h);
	if (h->button == BTN_START) {
		draw_rrect_stroke(x + 16, y + 19, sw, 26, 5, 1, C_MUTED);
		draw_text(&ST_KEYCAP, x + 16 + 7, y + 19 + (26 - text_height(&ST_KEYCAP)) / 2, tr(S_KEY_START));
	} else {
		draw_icon(ui_button_icon(h->button), x + 16, y + 21, 22, 22,
			h->dim ? C_DISC : ui_button_colour(h->button));
	}
	TextStyle st = h->dim ? st_col(&ST_HINT, C_DISC) : ST_HINT;
	draw_text(&st, x + 16 + sw + 10, y + (64 - text_height(&st)) / 2, h->label);
	ui_hit(x, y, ui_hint_width(h), 64, h->action, h->arg);
}

void ui_footer(const Hint *h, int n)
{
	draw_rect(0, FOOTER_Y, SCREEN_W, 1, C_RULE);
	float y = FOOTER_Y + 1 + 4;
	float x = 14;
	for (int i = 0; i < n; i++)
		if (!h[i].right) {
			ui_hint_draw(&h[i], x, y);
			x += ui_hint_width(&h[i]) + 4;
		}
#ifdef HOST_BUILD
	float left_end = x;
#endif
	x = SCREEN_W - 14;
	for (int i = n - 1; i >= 0; i--)
		if (h[i].right) {
			x -= ui_hint_width(&h[i]);
			ui_hint_draw(&h[i], x, y);
			x -= 4;
		}
#ifdef HOST_BUILD
	if (left_end + 12 > x + 4)
		host_layout_problem("footer hints run into each other");
#endif
}

void ui_tile(float x, float y, float w, float h, int warn)
{
	draw_rrect(x, y, w, h, 6, C_TILE);
	draw_rrect_stroke(x, y, w, h, 6, 1, warn ? C_WARN_EDGE : C_RULE);
}

void ui_card(float x, float y, float w, float h, int focused)
{
	draw_rrect(x, y, w, h, 6, C_TILE);
	if (focused)
		draw_rrect_stroke(x, y, w, h, 6, 2, C_FOCUS);
	else
		draw_rrect_stroke(x, y, w, h, 6, 1, C_RULE);
}

void ui_toast(const char *text)
{
	float w = text_width(&ST_TEXT20, text) + 40, h = 48;
	float x = (SCREEN_W - w) / 2, y = FOOTER_Y - h - 12;
	draw_rrect(x, y, w, h, 6, RGBHEX(0x101412));
	draw_rrect_stroke(x, y, w, h, 6, 1, C_CHIP);
	draw_text(&ST_TEXT20, x + 20, y + (h - text_height(&ST_TEXT20)) / 2, text);
}

/* ---- the pipe ---------------------------------------------------------- */

static Colour g_accent = 0, g_accent_text = 0;

void ui_pipe_accent(Colour fill, Colour text)
{
	g_accent = fill;
	g_accent_text = text;
}

const PipeGeom PIPE_LARGE   = { 56, 30, 13, 27, 62 };
const PipeGeom PIPE_COMPACT = { 50, 26, 12, 24, 56 };

static void icon_size(int icon, float *w, float *h)
{
	switch (icon) {
	case ICON_VITA:
		*w = 28;
		*h = 18;
		break;
	default:
		*w = *h = 24;
		break;
	}
}

static void tube_content(const PipeItem *it, float x, float y, float w, float h)
{
	TextStyle solid = st_col(&ST_TUBE, g_accent_text ? g_accent_text : ST_TUBE.colour);
	const TextStyle *st = it->tube == TUBE_SOLID ? &solid : &ST_TUBE_MUTED;
	float tw = text_width(st, it->tube_text);
	float iw = 0, ih = 0;
	if (it->tube_icon == ICON_LOCK) {
		iw = 14;
		ih = 16;
	} else if (it->tube_icon >= 0) {
		iw = ih = 14;
	}
	float total = tw + (iw > 0 ? iw + 8 : 0);
	float cx = x + (w - total) / 2;
	if (iw > 0) {
		draw_icon(it->tube_icon, cx, y + (h - ih) / 2, iw, ih, st->colour);
		cx += iw + 8;
	}
	draw_text(st, cx, y + (h - text_height(st)) / 2, it->tube_text);
}

void ui_pipe(const PipeItem *items, int n, float x, float top, float w,
	const PipeGeom *g, float *xs)
{
	float fixed = 0;
	int flex = 0;
	for (int i = 0; i < n; i++) {
		if (items[i].width > 0)
			fixed += items[i].width;
		else
			flex++;
	}
	float share = flex ? (w - fixed) / flex : 0;
	float px = x;
	float slab_top = top + g->tube_top + g->tube_h + 8;
	for (int i = 0; i < n; i++) {
		const PipeItem *it = &items[i];
		float iw = it->width > 0 ? it->width : share;
		if (xs)
			xs[i] = px;
		switch (it->kind) {
		case PIPE_NODE: {
			float dx = px + (iw - g->disc) / 2;
			Colour ring = it->disc == DISC_ON ? (g_accent ? g_accent : C_LIME) : it->disc == DISC_BAD ? C_ORANGE : C_DISC;
			draw_ring(dx + g->disc / 2, top + g->disc / 2, g->disc / 2, 2, ring);
			if (it->icon >= 0) {
				float icw, ich;
				icon_size(it->icon, &icw, &ich);
				draw_icon(it->icon, dx + (g->disc - icw) / 2, top + (g->disc - ich) / 2,
					icw, ich, it->icon_colour);
			}
			if (it->label) {
				TextStyle st = st_col(&ST_NLAB, it->label_colour ? it->label_colour : C_MUTED);
				draw_text(&st, px + (iw - text_width(&st, it->label)) / 2, top + g->label_top, it->label);
			}
			break;
		}
		case PIPE_LINE:
			draw_rect(px, top + g->line_top, iw, 2, C_DISC);
			break;
		case PIPE_TUBE:
			if (it->tube == TUBE_SOLID)
				draw_rrect(px, top + g->tube_top, iw, g->tube_h, g->tube_h / 2, g_accent ? g_accent : C_LIME);
			else
				draw_rrect_dashed(px, top + g->tube_top, iw, g->tube_h, g->tube_h / 2, 2, 6, 6, C_DASH);
			tube_content(it, px, top + g->tube_top, iw, g->tube_h);
			break;
		}
		if (it->kind != PIPE_NODE && it->slab) {
			const TextStyle *st = it->slab_st ? it->slab_st : &ST_SLAB_MONO;
			draw_text(st, px + (iw - text_width(st, it->slab)) / 2, slab_top, it->slab);
		}
		px += iw;
	}
}
