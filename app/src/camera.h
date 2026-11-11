/*
 * camera.h - the rear camera for QR import: 640x480,
 * YUV420 planar, whose luma plane is the grey image the decoder wants.
 * On the host there is no camera: the preview is a placeholder.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

/* 0, or a negative error from the system */
int cam_open(void);
/* Stops the camera and wipes the frame it held */
void cam_close(void);
/* Waits for the next frame and returns its luma plane; NULL if none */
const uint8_t *cam_frame(int *w, int *h, int *stride);
/* Draws the live view scaled into the box */
void cam_draw(float x, float y, float w, float h);

#endif
