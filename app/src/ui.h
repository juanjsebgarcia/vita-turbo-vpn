/*
 * ui.h - the design's building blocks: palette, text styles, header,
 * footer hints, tiles, rich text, the pipe diagram, and touch targets.
 *
 * Sizes: text at least 16 px, body 22 px,
 * values 18 px (26 px when the value is the point), status words 52-60 px,
 * touch targets 64 px tall.
 */
#ifndef UI_H
#define UI_H

#include "draw.h"
#include "strings.h"

#define SCREEN_W 960
#define SCREEN_H 544

/* Palette, "OLED pipe" */
#define C_BG          RGBHEX(0x000000)
#define C_TEXT        RGBHEX(0xf2f5f0)
#define C_MUTED       RGBHEX(0xa3aca5)
/* The two state colours can be overridden at build time to try palettes
 * (make palettes); the defaults are the design's. */
#ifndef PAL_GOOD
#define PAL_GOOD      0xb8f53d
#endif
#ifndef PAL_BAD
#define PAL_BAD       0xdc1c2c   /* Signal red, the brand colour */
#endif
#ifndef PAL_BAD_EDGE
#define PAL_BAD_EDGE  0x5c1218
#endif
#define C_LIME        RGBHEX(PAL_GOOD)
#define C_ORANGE      RGBHEX(PAL_BAD)
/*
 * Brand colour, "Signal red" as on the release splash. It marks
 * branding only, never state: lime and orange keep meaning protected and
 * trouble. How far it reaches is a build-time choice (make red-options):
 *   0  none: the mark lights lime when protected (the design as mocked up)
 *   1  A: LiveArea images and the header mark
 *   2  B: A, plus the focus highlight and the About screen
 *   3  C: B, plus a thin red rule under the header
 */
#ifndef PAL_BRAND
#define PAL_BRAND     0xdc1c2c
#endif
#ifndef BRAND_LEVEL
#define BRAND_LEVEL   1
#endif
#define C_BRAND       RGBHEX(PAL_BRAND)
#define C_FOCUS       (BRAND_LEVEL >= 2 ? C_BRAND : C_LIME)
#define C_RULE        RGBHEX(0x1c211e)   /* hairlines, tile borders */
#define C_CHIP        RGBHEX(0x2a302c)
#define C_WARN_EDGE   RGBHEX(PAL_BAD_EDGE)
#define C_DISC        RGBHEX(0x3a403c)
#define C_DASH        RGBHEX(0x4a514c)
#define C_TILE        RGBHEX(0x070908)
#define C_BTN_CROSS   RGBHEX(0x7aa7ff)
#define C_BTN_TRI     RGBHEX(0x4fd1a1)
#define C_BTN_SQUARE  RGBHEX(0xf08bd6)
#define C_BTN_CIRCLE  RGBHEX(0xff6b6b)

/* Text styles */
extern const TextStyle ST_MARK, ST_TITLE, ST_CHIP, ST_CHIP_WARN, ST_EYEBROW,
	ST_STATE, ST_STATE_S, ST_SUB, ST_SUB_MUTED, ST_NLAB, ST_SLAB_MONO, ST_SLAB,
	ST_TUBE, ST_TUBE_MUTED, ST_BIG, ST_MONO26, ST_MONO18, ST_MONO16, ST_OK,
	ST_HINT, ST_NAME, ST_DETAIL, ST_ST, ST_OPT_NAME, ST_VERDICT, ST_TEXT22,
	ST_TEXT20, ST_TEXT18, ST_HEAD_RIGHT, ST_KEY;

/* A copy of a style in another colour */
TextStyle st_col(const TextStyle *st, Colour c);

/* ---- touch targets ----------------------------------------------------- */

typedef struct Hit {
	float x, y, w, h;
	int action, arg;
} Hit;

void ui_hits_clear(void);
void ui_hit(float x, float y, float w, float h, int action, int arg);
/* The last target registered under the point, or NULL */
const Hit *ui_hit_at(float x, float y);

/* ---- rich text --------------------------------------------------------- */

typedef struct Span {
	const TextStyle *st;
	const char *s;
} Span;

enum { ALIGN_LEFT, ALIGN_CENTRE, ALIGN_RIGHT };

float spans_width(const Span *sp, int n);
/* One line of spans on a shared baseline, line box of the first span's height */
void spans_draw(const Span *sp, int n, float x, float top);
/*
 * Wrap spans into lines of at most w px; each line is line_h tall. Draws
 * when draw is set. Returns the height used.
 */
float spans_para(const Span *sp, int n, float x, float top, float w, float line_h,
	int align, int draw);
/*
 * Split a translated template into spans: literal text in base, {0} and
 * {1} replaced by args in their own styles. buf holds the pieces.
 */
int spans_template(const char *tmpl, const TextStyle *base, const char *a0,
	const TextStyle *s0, const char *a1, const TextStyle *s1,
	Span *out, int max, char *buf, size_t bufn);

/* s shortened with an ellipsis to fit w px; returns out */
const char *ui_fit(const TextStyle *st, const char *s, float w, char *out, size_t n);

/* ---- frame pieces ------------------------------------------------------ */

/* Header with the mark and chips on the right */
typedef struct Chip {
	const char *text;
	int warn;
} Chip;
void ui_header_mark(int lit, const Chip *chips, int n);
/* Header with a screen title and spans on the right */
void ui_header_title(const char *title, const Span *right, int n);

/* Face buttons, and Start (drawn as a key cap with its name) */
enum { BTN_CROSS, BTN_TRIANGLE, BTN_SQUARE, BTN_CIRCLE, BTN_START };

typedef struct Hint {
	int button;
	const char *label;
	int action, arg;
	int right;     /* placed in the right-hand group */
	int dim;       /* drawn muted: not available now */
} Hint;

/* The footer bar of hints, each a 64 px touch target */
void ui_footer(const Hint *h, int n);
/* Hint geometry: the same layout, drawn anywhere (dialogs) */
float ui_hint_width(const Hint *h);
void ui_hint_draw(const Hint *h, float x, float y);
extern int ui_enter_circle;   /* the console confirms with circle */
int ui_button_icon(int button);
Colour ui_button_colour(int button);

void ui_tile(float x, float y, float w, float h, int warn);
/* A focused card: 2 px border in the focus colour */
void ui_card(float x, float y, float w, float h, int focused);

/* Toast and dialog overlays */
void ui_toast(const char *text);

/* ---- the pipe ---------------------------------------------------------- */

enum { PIPE_NODE, PIPE_LINE, PIPE_TUBE };
enum { DISC_OFF, DISC_ON, DISC_BAD };
enum { TUBE_SOLID, TUBE_DASHED };

typedef struct PipeItem {
	int kind;
	float width;           /* 0 = share the rest */
	/* node */
	int disc;              /* DISC_* */
	int icon;              /* -1 = none */
	Colour icon_colour;
	const char *label;
	Colour label_colour;
	/* line and tube */
	const char *slab;      /* caption under it */
	const TextStyle *slab_st;
	int tube;              /* TUBE_* */
	const char *tube_text;
	int tube_icon;         /* -1 = none */
} PipeItem;

typedef struct PipeGeom {
	float disc, tube_h, tube_top, line_top, label_top;
} PipeGeom;

extern const PipeGeom PIPE_LARGE, PIPE_COMPACT;

/* Colour lit discs and the solid tube in fill, with the tube's text in
 * text, instead of lime (LiveArea art); 0, 0 restores lime */
void ui_pipe_accent(Colour fill, Colour text);

/* Draw a pipe across x..x+w from top; fills xs[i] with each item's left edge */
void ui_pipe(const PipeItem *items, int n, float x, float top, float w,
	const PipeGeom *g, float *xs);

#endif
