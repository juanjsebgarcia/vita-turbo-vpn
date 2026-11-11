/*
 * camera_host.c - no camera on a computer: no frames, and a placeholder
 * where the live view goes, for screenshots.
 */
#include <stddef.h>

#include "camera.h"
#include "draw.h"

int cam_open(void)
{
	return 0;
}

void cam_close(void)
{
}

const uint8_t *cam_frame(int *w, int *h, int *stride)
{
	(void)w;
	(void)h;
	(void)stride;
	return NULL;
}

void cam_draw(float x, float y, float w, float h)
{
	/* a dim grey field with a faint grid, standing for the camera image */
	draw_rect(x, y, w, h, RGBHEX(0x16191a));
	for (float gx = x + 32; gx < x + w; gx += 32)
		draw_rect(gx, y, 1, h, RGBHEX(0x1e2224));
	for (float gy = y + 32; gy < y + h; gy += 32)
		draw_rect(x, gy, w, 1, RGBHEX(0x1e2224));
}
