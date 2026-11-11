/*
 * main_vita.c - the console's main loop: vita2d, buttons, the front touch
 * screen and the system language, fed to the platform-independent app.
 */
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <string.h>
#include <vita2d.h>

#include "app.h"
#include "draw.h"
#include "ui.h"
#include "strings.h"

#define FONT_DIR "app0:fonts/"

static const struct {
	unsigned sce;
	uint32_t in;
} g_map[] = {
	{ SCE_CTRL_CROSS, IN_CROSS },
	{ SCE_CTRL_CIRCLE, IN_CIRCLE },
	{ SCE_CTRL_TRIANGLE, IN_TRIANGLE },
	{ SCE_CTRL_SQUARE, IN_SQUARE },
	{ SCE_CTRL_UP, IN_UP },
	{ SCE_CTRL_DOWN, IN_DOWN },
	{ SCE_CTRL_LEFT, IN_LEFT },
	{ SCE_CTRL_RIGHT, IN_RIGHT },
	{ SCE_CTRL_LTRIGGER, IN_L },
	{ SCE_CTRL_RTRIGGER, IN_R },
	{ SCE_CTRL_START, IN_START },
	{ SCE_CTRL_SELECT, IN_SELECT },
};

static uint64_t now_ms(void)
{
	return sceKernelGetProcessTimeWide() / 1000;
}

static int system_language(void)
{
	SceAppUtilInitParam init;
	SceAppUtilBootParam boot;
	memset(&init, 0, sizeof(init));
	memset(&boot, 0, sizeof(boot));
	sceAppUtilInit(&init, &boot);
	int lang = LANG_ENGLISH_US;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter) >= 0)
		ui_enter_circle = enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
	return lang;
}

int main(void)
{
	vita2d_init();
	vita2d_set_vblank_wait(1);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	int lang = system_language();
	str_set_language(lang);
	draw_set_language(str_language());

	if (draw_init(FONT_DIR) != 0) {
		/* nothing to draw text with: leave rather than show a blank app */
		vita2d_fini();
		sceKernelExitProcess(0);
		return 0;
	}
	app_init(now_ms());

	unsigned prev = 0;
	int touching = 0;
	float tx = 0, ty = 0;
	for (;;) {
		SceCtrlData pad;
		memset(&pad, 0, sizeof(pad));
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned down = pad.buttons & ~prev;
		prev = pad.buttons;

		AppInput in;
		memset(&in, 0, sizeof(in));
		for (size_t i = 0; i < sizeof(g_map) / sizeof(g_map[0]); i++)
			if (down & g_map[i].sce) {
				uint32_t b = g_map[i].in;
				if (ui_enter_circle && (b == IN_CROSS || b == IN_CIRCLE))
					b = b == IN_CROSS ? IN_CIRCLE : IN_CROSS;   /* confirm/back */
				in.pressed |= b;
			}

		/* a tap is a touch released; the panel is 1920x1088 for a 960x544 screen */
		SceTouchData touch;
		memset(&touch, 0, sizeof(touch));
		sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
		if (touch.reportNum > 0) {
			touching = 1;
			tx = touch.report[0].x / 2.0f;
			ty = touch.report[0].y / 2.0f;
		} else if (touching) {
			touching = 0;
			in.tap = 1;
			in.tx = tx;
			in.ty = ty;
		}

		app_frame(now_ms(), &in);
	}
	return 0;
}
