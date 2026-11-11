/*
 * hud.c - draw the VPN HUD over the live framebuffer.
 *
 * Copied from vita-turbo-wifi kernel/hud.c,
 * so a fix to one HUD's blitter can be carried to the other as the same
 * patch. What changed from the copy:
 *   - the sceDisplaySetFrameBuf hook, the DACR open/restore, the
 *     plausibility guard, the arming and the system-process filter moved to
 *     overlay.c, which shares the one hook with the status-bar padlock
 *     (one hook, two optional overlays). This file keeps the
 *     text blitter, the formatting helpers and the corner anchoring;
 *   - the data shown is the tunnel state, TUNN: and EXIT:, taken
 *     from the module's own state (no user-supplied WqHudState);
 *   - the lines are built only when what they show changes (hud_update,
 *     called from outside the display hook), into the back copy of a double
 *     buffer; hud_render, the only part the hook runs, blits the front copy;
 *   - the glyph scale follows the framebuffer: 2x from 720 px wide, 1x
 *     below, so a game's 480x272 buffer is not half covered;
 *   - draw_text stops at the line buffer's length.
 */
#include <string.h>

#include "hud.h"
#include "hud_i18n.h"
#include "font8x8.h"

#define VPNTURBO_NO_PROTOTYPES
#include "../include/vpnturbo.h"   /* VT_STATE_*, VT_CORNER_* */

#define HUD_MARGIN_X   24         /* at 2x; half at 1x                        */
#define HUD_MARGIN_Y   24
#define HUD_FG         0xFFFFFFFFu /* opaque white (ABGR8888)                 */
#define HUD_SHADOW     0xFF000000u /* opaque black drop shadow                */

/* ---- framebuffer text blitter ------------------------------------------ */

static inline void put_px(uint32_t *fb, int pitch, int w, int h,
                          int x, int y, uint32_t color)
{
	if (x < 0 || y < 0 || x >= w || y >= h)
		return;
	fb[y * pitch + x] = color;
}

static void draw_char(uint32_t *fb, int pitch, int w, int h,
                      int ox, int oy, char ch, uint32_t color, int scale)
{
	if ((unsigned char)ch < WQ_FONT_FIRST || (unsigned char)ch > WQ_FONT_LAST)
		return;
	const unsigned char *g = wq_font8x8[(unsigned char)ch - WQ_FONT_FIRST];
	for (int row = 0; row < 8; row++) {
		unsigned char bits = g[row];
		for (int col = 0; col < 8; col++) {
			if (!(bits & (1u << col)))
				continue;
			for (int sy = 0; sy < scale; sy++)
				for (int sx = 0; sx < scale; sx++)
					put_px(fb, pitch, w, h,
					       ox + col * scale + sx,
					       oy + row * scale + sy, color);
		}
	}
}

static void draw_text(uint32_t *fb, int pitch, int w, int h,
                      int ox, int oy, const char *s, int max, int scale)
{
	int x = ox;
	for (int i = 0; i < max && s[i]; i++) {
		/* 1px drop shadow for legibility over any background. */
		draw_char(fb, pitch, w, h, x + 1, oy + 1, s[i], HUD_SHADOW, scale);
		draw_char(fb, pitch, w, h, x, oy, s[i], HUD_FG, scale);
		x += (8 * scale);
	}
}

/* Tiny, allocation-free formatting helpers (no libc printf in the hook). */
static char *put_str(char *p, char *end, const char *s)
{
	while (*s && p < end - 1) *p++ = *s++;
	return p;
}
static char *put_uint(char *p, char *end, unsigned v)
{
	char tmp[12];
	int n = 0;
	if (!v) tmp[n++] = '0';
	while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
	while (n && p < end - 1) *p++ = tmp[--n];
	return p;
}
static char *put_ip(char *p, char *end, uint32_t a)   /* host order */
{
	if (!a)
		return put_str(p, end, "--");
	for (int s = 24; s >= 0; s -= 8) {
		p = put_uint(p, end, (a >> s) & 0xFF);
		if (s && p < end - 1) *p++ = '.';
	}
	return p;
}

static const char *state_name(int lang, int st)
{
	switch (st) {
	case VT_STATE_UP:          return hud_tr(lang, HTR_ACTIVE);
	case VT_STATE_HANDSHAKING:
	case VT_STATE_STALLED:     return hud_tr(lang, HTR_CONNECTING);  /* still retrying */
	case VT_STATE_LOCKED:      return hud_tr(lang, HTR_BLOCKED);
	case VT_STATE_PAUSED:      return hud_tr(lang, HTR_PAUSED);
	default:                   return hud_tr(lang, HTR_INACTIVE);    /* idle, error */
	}
}

/* ---- the built text (double-buffered so the blit never reads a torn one) */

#define HUD_MAX_LINES  3
#define HUD_LINE_CHARS 32

typedef struct {
	int  n, maxlen;
	char line[HUD_MAX_LINES][HUD_LINE_CHARS];
} HudText;

static HudText g_text[2];
static volatile int g_front;
static volatile int g_built;
static volatile int g_busy;
static HudInfo g_shown;

static void end_line(HudText *t, char *p)
{
	*p = 0;
	int len = (int)(p - t->line[t->n]);
	if (len > t->maxlen) t->maxlen = len;
	t->n++;
}

void hud_update(const HudInfo *in)
{
	if (g_built && memcmp(in, &g_shown, sizeof(*in)) == 0)
		return;
	if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE))
		return;                         /* another frame is building it */

	int back = g_front ^ 1;
	HudText *t = &g_text[back];
	char *p, *end;

	t->n = 0;
	t->maxlen = 0;

	/* line 1: "VPN <STATE>" (an acronym: universal) */
	p = t->line[t->n]; end = p + HUD_LINE_CHARS;
	p = put_str(p, end, "VPN ");
	p = put_str(p, end, state_name(in->lang, in->state));
	end_line(t, p);

	if (in->extended) {
		/* line 2: the tunnel address in use; not while the tunnel is idle */
		if (in->state != VT_STATE_IDLE) {
			p = t->line[t->n]; end = p + HUD_LINE_CHARS;
			p = put_str(p, end, "TUNN: ");
			p = put_ip(p, end, in->tunnel_address);
			end_line(t, p);
		}

		/* line 3: the exit address from the app's last check;
		 * "--" until there is one */
		p = t->line[t->n]; end = p + HUD_LINE_CHARS;
		p = put_str(p, end, "EXIT: ");
		p = put_ip(p, end, in->exit_ip);
		end_line(t, p);
	}

	__atomic_store_n(&g_front, back, __ATOMIC_RELEASE);
	g_shown = *in;
	g_built = 1;
	__atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}

void hud_render(uint32_t *fb, int pitch, int w, int h, int corner)
{
	if (!g_built)
		return;
	const HudText *t = &g_text[__atomic_load_n(&g_front, __ATOMIC_ACQUIRE)];
	int scale = w >= 720 ? 2 : 1;
	int n = t->n > HUD_MAX_LINES ? HUD_MAX_LINES : t->n;
	int maxlen = t->maxlen > HUD_LINE_CHARS - 1 ? HUD_LINE_CHARS - 1 : t->maxlen;

	/* Anchor the block. Glyphs are 8 px wide at 1x, so a column is 8*scale;
	 * a line is one glyph tall plus the inter-line gap. */
	int line_h = (8 * scale) + 2 * scale;
	int box_w  = maxlen * 8 * scale;
	int box_h  = n * line_h;
	int right  = (corner == VT_CORNER_TOP_RIGHT || corner == VT_CORNER_BOTTOM_RIGHT);
	int bottom = (corner == VT_CORNER_BOTTOM_LEFT || corner == VT_CORNER_BOTTOM_RIGHT);
	int mx = HUD_MARGIN_X * scale / 2, my = HUD_MARGIN_Y * scale / 2;
	int x = right  ? w - mx - box_w : mx;
	int y = bottom ? h - my - box_h : my;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	for (int i = 0; i < n; i++, y += line_h)
		draw_text(fb, pitch, w, h, x, y, t->line[i], HUD_LINE_CHARS, scale);
}

void hud_banner(uint32_t *fb, int pitch, int w, int h, const char *text)
{
	int scale = w >= 720 ? 3 : 1, n = 0;
	while (text[n] && n < 40)
		n++;
	int tw = n * 8 * scale, th = 8 * scale, pad = 6 * scale;
	int x = (w - tw) / 2, y = (h - th) / 2;
	if (x < pad)
		x = pad;
	for (int yy = y - pad; yy < y + th + pad; yy++)
		for (int xx = x - pad; xx < x + tw + pad; xx++)
			put_px(fb, pitch, w, h, xx, yy, HUD_SHADOW);
	draw_text(fb, pitch, w, h, x, y, text, n, scale);
}
