/*
 * qr.c - QR decoding with quirc (ISC, third_party/quirc), the
 * relaxed grouping build. Each frame is copied into quirc's buffer, which
 * quirc thresholds in place; a code that fails error correction is tried
 * once more mirrored. Nothing here keeps the payload.
 */
#include <string.h>

#include "qr.h"
#include "quirc.h"
#include "quirc_relaxed.h"
#include "vtconf.h"

static struct quirc *g_q;
static int g_w, g_h;

int qr_open(int w, int h)
{
	if (!g_q)
		g_q = quirc_new();
	if (!g_q || quirc_resize(g_q, w, h) < 0) {
		qr_close();
		return -1;
	}
	g_w = w;
	g_h = h;
	return 0;
}

void qr_close(void)
{
	if (g_q) {
		int w, h;
		uint8_t *img = quirc_begin_relaxed(g_q, &w, &h);
		vtc_wipe(img, (size_t)w * (size_t)h);
		quirc_destroy(g_q);
		g_q = NULL;
	}
}

int qr_scan(const uint8_t *grey, int stride, char *out, int cap, float corners[8])
{
	static struct quirc_code code;
	static struct quirc_data data;
	if (!g_q)
		return 0;
	uint8_t *img = quirc_begin_relaxed(g_q, NULL, NULL);
	for (int y = 0; y < g_h; y++)
		memcpy(img + (size_t)y * (size_t)g_w, grey + (size_t)y * (size_t)stride, (size_t)g_w);
	quirc_end_relaxed(g_q);

	int n = quirc_count(g_q), result = 0;
	for (int i = 0; i < n && result <= 0; i++) {
		quirc_extract_relaxed(g_q, i, &code);
		if (corners)
			for (int k = 0; k < 4; k++) {
				corners[k * 2] = (float)code.corners[k].x;
				corners[k * 2 + 1] = (float)code.corners[k].y;
			}
		quirc_decode_error_t e = quirc_decode(&code, &data);
		if (e == QUIRC_ERROR_DATA_ECC) {
			quirc_flip(&code);
			e = quirc_decode(&code, &data);
		}
		if (e == QUIRC_SUCCESS && data.payload_len > 0 && data.payload_len <= cap) {
			memcpy(out, data.payload, (size_t)data.payload_len);
			result = data.payload_len;
		} else {
			result = -1;
		}
		vtc_wipe(&data, sizeof(data));
		vtc_wipe(&code, sizeof(code));
	}
	return result;
}
