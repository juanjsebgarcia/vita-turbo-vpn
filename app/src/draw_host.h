/*
 * draw_host.h - the software back end's own calls (host builds only).
 */
#ifndef DRAW_HOST_H
#define DRAW_HOST_H

#include <stdint.h>

/* Allocate a w x h framebuffer to draw into */
int host_target(int w, int h);
const uint32_t *host_pixels(void);
/* Write the framebuffer as an RGB PNG */
int host_write_png(const char *path);
/* Write it as an 8-bit palette PNG (the format LiveArea needs) */
int host_write_png8(const char *path);

/*
 * Layout checks: draw.c reports every text run and box it draws; after a
 * frame, host_layout_report prints text that runs off the screen, out of
 * the box it starts in, or into other text, and returns how many.
 */
void host_layout_reset(void);
void host_trace_box(int x, int y, int w, int h);
void host_trace_text(float x, float y, float w, float h, const char *s);
/* A layout problem the drawing code noticed itself, listed with the rest */
void host_layout_problem(const char *what);
/* An opaque shape drawn over earlier text (a toast): that text no longer counts */
void host_trace_cover(int x, int y, int w, int h);
int host_layout_report(const char *name);

#endif
