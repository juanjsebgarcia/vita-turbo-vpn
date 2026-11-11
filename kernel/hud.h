/*
 * hud.h - the VPN HUD: text blitter and layout. Copied from
 * vita-turbo-wifi kernel/hud.h; the hook and settings it
 * had are in overlay.c here (shared with the status-bar padlock).
 */
#ifndef VPNTURBO_HUD_H
#define VPNTURBO_HUD_H

#include <stdint.h>

/* What the HUD shows. Plain 32-bit fields only: hud_update compares it
 * byte for byte to see whether the text must be rebuilt. */
typedef struct HudInfo {
	int32_t  state;            /* VT_STATE_* */
	int32_t  lang;             /* Vita system-language index */
	int32_t  extended;         /* 0 = the state line only, 1 = TUNN and EXIT too */
	uint32_t tunnel_address;   /* host order, 0 = none */
	uint32_t exit_ip;          /* host order, 0 = no check or it failed */
} HudInfo;

/* Rebuild the text if `in` differs from what is built. Cheap when nothing
 * changed; safe from any thread (a concurrent caller simply skips). Never
 * call it from the display hook: it formats text. */
void hud_update(const HudInfo *in);

/* Blit the built text into a 32 bpp framebuffer in the corner given
 * (VT_CORNER_*). The caller has checked the buffer and opened the DACR. */
void hud_render(uint32_t *fb, int pitch, int w, int h, int corner);

/* One line of text centred on the screen, large, on a black box (the
 * restart banner). Same caller duties as hud_render. */
void hud_banner(uint32_t *fb, int pitch, int w, int h, const char *text);

#endif /* VPNTURBO_HUD_H */
