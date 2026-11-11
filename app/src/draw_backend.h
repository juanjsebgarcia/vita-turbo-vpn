/*
 * draw_backend.h - what each platform provides to draw.c.
 *
 * Shapes, icons and glyphs are rasterised once in shared code into coverage
 * masks, cached, and handed to the platform to blend in a colour. The
 * platform keeps its own copy (a texture atlas on the console) in the
 * fields marked "backend".
 */
#ifndef DRAW_BACKEND_H
#define DRAW_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include "draw.h"

typedef struct Mask {
	uint32_t key[6];
	int w, h;           /* size of a[] */
	int ox, oy;         /* offset of a[0] from the drawing position */
	uint8_t *a;         /* coverage, 0-255, w*h */
	struct Mask *next;  /* cache chain */
	/* backend */
	int bx, by;         /* position in the atlas */
	uint32_t bgen;      /* atlas generation it was uploaded in, 0 = never */
	float adv;          /* fallback glyphs: advance in px */
} Mask;

void be_begin(Colour clear);
void be_end(void);
extern void (*be_trace)(const char *what);   /* trace hook, NULL when off */
void be_fill(int x, int y, int w, int h, Colour c);
void be_mask(Mask *m, int x, int y, Colour c);
void be_clip(int x, int y, int w, int h);
void be_unclip(void);
/* The mask cache was emptied: forget every Mask pointer held */
void be_masks_flushed(void);

/* Read a whole file into malloc'd memory; NULL on failure */
void *be_load_file(const char *path, size_t *size);

/*
 * Fallback glyphs, for characters the bundled fonts do not have (Cyrillic,
 * kana, Han, Hangul). The console uses the system fonts; the host uses
 * font files from <font dir>/fallback/ when they are there (screenshots).
 * lang is the system language number, which picks the regional forms of
 * Han characters.
 */
typedef struct FbGlyph {
	int w, h;           /* size of a[] */
	int ox, oy;         /* offset of a[0] from the pen on the baseline */
	float adv;          /* advance in px */
	uint8_t *a;         /* coverage, malloc'd, w*h; the caller frees it */
} FbGlyph;

void be_fallback_init(const char *font_dir, int lang);
void be_fallback_shutdown(void);
/* Rasterise cp at size px to the em; bold for the display faces. 0 on
 * success, -1 when no fallback font has the character. */
int be_fallback_glyph(uint32_t cp, float size, int bold, FbGlyph *g);

#endif
