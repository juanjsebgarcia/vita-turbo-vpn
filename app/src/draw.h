/*
 * draw.h - the app's drawing API: filled and stroked shapes, icons and text.
 *
 * Everything is built in shared code from two primitives that each platform
 * provides (draw_backend.h): fill a rectangle, and blend a coverage mask in
 * one colour. The console draws with vita2d; the host renders into a memory
 * framebuffer for screenshots. Coordinates are in screen pixels, 960x544.
 */
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

/* 0xAARRGGBB */
typedef uint32_t Colour;
#define RGB(r, g, b)      (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBHEX(h)         (0xFF000000u | (uint32_t)(h))
#define WITH_ALPHA(c, a)  (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

/* Fonts, one per face and weight in use */
enum {
	FONT_DISPLAY,       /* Chakra Petch SemiBold: titles, labels */
	FONT_DISPLAY_BOLD,  /* Chakra Petch Bold: status words */
	FONT_TEXT,          /* Barlow Semi Condensed Regular: text */
	FONT_MONO,          /* Martian Mono Regular: addresses */
	FONT_MONO_MEDIUM,   /* Martian Mono Medium: values that are the point */
	FONT_COUNT
};

typedef struct TextStyle {
	uint8_t font;
	uint8_t upper;      /* draw in capitals (labels) */
	float size;         /* em size in px, as CSS font-size */
	float tracking;     /* extra space after each character, in em */
	Colour colour;
} TextStyle;

/* Icons, each drawn from a small vector description */
enum {
	ICON_MARK,          /* app mark, two nested rounded squares */
	ICON_VITA,          /* the console */
	ICON_LOCK,
	ICON_HOUSE,         /* exit server */
	ICON_GLOBE,         /* internet */
	ICON_CHECK,
	ICON_WARN,
	ICON_CROSS_SMALL,   /* tunnel down */
	ICON_BREAK,         /* tunnel broken, migrate screen */
	ICON_BTN_CROSS,     /* face buttons */
	ICON_BTN_TRIANGLE,
	ICON_BTN_SQUARE,
	ICON_BTN_CIRCLE,
	ICON_COUNT
};

/* Load fonts from dir (ending in '/'); 0 on success */
int draw_init(const char *font_dir);
/*
 * The system language (strings.h numbering), set before draw_init: it
 * picks the fallback fonts' regional forms and Turkish capitals.
 */
void draw_set_language(int lang);
/* Characters no font could draw since the last call (host checks) */
int draw_missing_glyphs(uint32_t *last);
void draw_shutdown(void);

void draw_begin(Colour clear);
void draw_end(void);

void draw_rect(float x, float y, float w, float h, Colour c);
void draw_rrect(float x, float y, float w, float h, float r, Colour c);
void draw_rrect_stroke(float x, float y, float w, float h, float r, float stroke, Colour c);
/* Dashed outline along a rounded rectangle, dash and gap in px */
void draw_rrect_dashed(float x, float y, float w, float h, float r, float stroke,
	float dash, float gap, Colour c);
/* Dashed straight line, horizontal or vertical (one of w, h is the thickness) */
void draw_dashed(float x, float y, float w, float h, float dash, float gap, Colour c);
void draw_circle(float cx, float cy, float r, Colour c);
void draw_ring(float cx, float cy, float r, float stroke, Colour c);

/* Draw an icon with its top-left corner at x, y; size is its box in px */
void draw_icon(int icon, float x, float y, float w, float h, Colour c);

/* Text. top is the top of the line box at line-height "normal". */
float text_width(const TextStyle *st, const char *s);
float text_height(const TextStyle *st);   /* normal line height */
float text_ascent(const TextStyle *st);
float draw_text(const TextStyle *st, float x, float top, const char *s);
/* Draw s so its baseline sits at baseline */
float draw_text_baseline(const TextStyle *st, float x, float baseline, const char *s);

/* Clip drawing to a rectangle (one level); draw_unclip() removes it */
void draw_clip(float x, float y, float w, float h);
void draw_unclip(void);

/* Decode one UTF-8 character, advancing *s; returns the code point */
uint32_t utf8_next(const char **s);

#endif
