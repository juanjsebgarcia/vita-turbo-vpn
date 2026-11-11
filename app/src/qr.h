/*
 * qr.h - finding and decoding a QR code in camera frames,
 * with quirc's relaxed finder grouping (qr/quirc_relaxed.h),
 * which dense profile codes need.
 */
#ifndef QR_H
#define QR_H

#include <stdint.h>

/* For frames of w x h; 0, or -1 if out of memory */
int qr_open(int w, int h);
/* Frees quirc and wipes its copy of the last frame */
void qr_close(void);

/*
 * Look for a code in a grey frame (the camera's luma plane). Returns the
 * payload length with the bytes in out, 0 if no code was seen, or -1 if a
 * code was seen but could not be read. corners (may be NULL) gets the
 * code's four corners, x then y, in frame pixels, whenever one was seen.
 * The caller wipes out when done: it may hold a private key.
 */
int qr_scan(const uint8_t *grey, int stride, char *out, int cap, float corners[8]);

#endif
