/*
 * camera_vita.c - the rear camera through SceCamera, as proven by the QR
 * QR test app: 640x480 at 30 frames a second in YUV420
 * planar, written straight into textures, so the luma plane is both the
 * decoder's input and the grey live view. The CPU runs at 444 MHz while
 * the camera is open, as decoding a dense code takes most of a frame, and
 * goes back to its previous speed when it closes.
 */
#include <psp2/camera.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <string.h>
#include <vita2d.h>

#include "camera.h"

#define CAM_W 640
#define CAM_H 480

static vita2d_texture *g_tex[3];   /* Y, U, V */
static int g_open;
static int g_clock;                /* CPU speed before, MHz; 0 = unchanged */

static void tex_free(void)
{
	vita2d_wait_rendering_done();
	for (int i = 0; i < 3; i++) {
		if (g_tex[i]) {
			/* the frame shows the code, which holds a private key */
			memset(vita2d_texture_get_datap(g_tex[i]), 0,
				vita2d_texture_get_stride(g_tex[i]) * vita2d_texture_get_height(g_tex[i]));
			vita2d_free_texture(g_tex[i]);
		}
		g_tex[i] = NULL;
	}
}

int cam_open(void)
{
	SceCameraInfo info;
	if (g_open)
		return 0;
	g_tex[0] = vita2d_create_empty_texture_format(CAM_W, CAM_H, SCE_GXM_TEXTURE_FORMAT_U8_R111);
	g_tex[1] = vita2d_create_empty_texture_format(CAM_W / 2, CAM_H / 2, SCE_GXM_TEXTURE_FORMAT_U8_R111);
	g_tex[2] = vita2d_create_empty_texture_format(CAM_W / 2, CAM_H / 2, SCE_GXM_TEXTURE_FORMAT_U8_R111);
	if (!g_tex[0] || !g_tex[1] || !g_tex[2]) {
		tex_free();
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	info.priority = SCE_CAMERA_PRIORITY_SHARE;
	info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
	info.resolution = SCE_CAMERA_RESOLUTION_640_480;
	info.framerate = 30;
	info.sizeIBase = CAM_W * CAM_H;
	info.sizeUBase = CAM_W * CAM_H / 4;
	info.sizeVBase = CAM_W * CAM_H / 4;
	info.pIBase = vita2d_texture_get_datap(g_tex[0]);
	info.pUBase = vita2d_texture_get_datap(g_tex[1]);
	info.pVBase = vita2d_texture_get_datap(g_tex[2]);
	int r = sceCameraOpen(SCE_CAMERA_DEVICE_BACK, &info);
	if (r < 0) {
		tex_free();
		return r;
	}
	r = sceCameraStart(SCE_CAMERA_DEVICE_BACK);
	if (r < 0) {
		sceCameraClose(SCE_CAMERA_DEVICE_BACK);
		tex_free();
		return r;
	}
	g_open = 1;
	int mhz = scePowerGetArmClockFrequency();
	if (mhz > 0 && mhz < 444) {
		g_clock = mhz;
		scePowerSetArmClockFrequency(444);
	}
	return 0;
}

void cam_close(void)
{
	if (!g_open)
		return;
	sceCameraStop(SCE_CAMERA_DEVICE_BACK);
	sceCameraClose(SCE_CAMERA_DEVICE_BACK);
	g_open = 0;
	tex_free();
	if (g_clock) {
		scePowerSetArmClockFrequency(g_clock);
		g_clock = 0;
	}
}

const uint8_t *cam_frame(int *w, int *h, int *stride)
{
	SceCameraRead rd;
	if (!g_open)
		return NULL;
	/* scanning is hands-off: keep the Vita from dimming and sleeping */
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	memset(&rd, 0, sizeof(rd));
	rd.size = sizeof(rd);
	rd.mode = 0;   /* wait for the next frame */
	if (sceCameraRead(SCE_CAMERA_DEVICE_BACK, &rd) < 0)
		return NULL;
	*w = CAM_W;
	*h = CAM_H;
	*stride = (int)vita2d_texture_get_stride(g_tex[0]);
	return vita2d_texture_get_datap(g_tex[0]);
}

void cam_draw(float x, float y, float w, float h)
{
	if (g_open && g_tex[0])
		vita2d_draw_texture_scale(g_tex[0], x, y, w / CAM_W, h / CAM_H);
}
