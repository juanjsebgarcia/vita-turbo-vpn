/* vpn_icons.h - the status bar padlock, generated.
 *
 * The icon comes in two forms, both row-major, ICON_H rows:
 *   _bits[row]   1 bit per pixel, bit 0 = leftmost pixel (the same
 *                convention as the HUD's 8x8 font, widened to 32 bits)
 *   _alpha[]     8-bit coverage, row-major, w * h bytes; blend white over
 *                the framebuffer pixel for smooth edges like the system icons
 * Draw at 1x (no scaling) in white on the status bar. Generated data, not
 * meant to be edited by hand. */
#ifndef VT_VPN_ICONS_H
#define VT_VPN_ICONS_H

#include <stdint.h>

#define VT_ICON_H  22

/* Padlock, bold shackle: 16 x 22 */
#define VT_ICON_PADLOCK_BOLD_W  16
static const uint32_t vt_icon_padlock_bold_bits[VT_ICON_H] = {
    0x000003C0, /* ......####...... */
    0x00000FF0, /* ....########.... */
    0x00001FF8, /* ...##########... */
    0x00001FF8, /* ...##########... */
    0x00003C3C, /* ..####....####.. */
    0x00003C3C, /* ..####....####.. */
    0x00003C3C, /* ..####....####.. */
    0x00003C3C, /* ..####....####.. */
    0x00003C3C, /* ..####....####.. */
    0x00003C3C, /* ..####....####.. */
    0x00007FFE, /* .##############. */
    0x00007FFE, /* .##############. */
    0x0000FFFF, /* ################ */
    0x0000FE7F, /* #######..####### */
    0x0000FC3F, /* ######....###### */
    0x0000FC3F, /* ######....###### */
    0x0000FE7F, /* #######..####### */
    0x0000FE7F, /* #######..####### */
    0x0000FE7F, /* #######..####### */
    0x0000FFFF, /* ################ */
    0x00007FFE, /* .##############. */
    0x00003FFC, /* ..############.. */
};
static const uint8_t vt_icon_padlock_bold_alpha[VT_ICON_H * 16] = {
      0,  0,  0,  0,  5,108,201,249,249,201,108,  5,  0,  0,  0,  0,
      0,  0,  0, 19,199,255,255,255,255,255,255,199, 19,  0,  0,  0,
      0,  0,  1,186,255,255,255,255,255,255,255,255,186,  1,  0,  0,
      0,  0, 72,255,255,255,240,147,147,240,255,255,255, 72,  0,  0,
      0,  0,154,255,255,251, 48,  0,  0, 48,251,255,255,154,  0,  0,
      0,  0,189,255,255,199,  0,  0,  0,  0,199,255,255,189,  0,  0,
      0,  0,191,255,255,191,  0,  0,  0,  0,191,255,255,191,  0,  0,
      0,  0,191,255,255,191,  0,  0,  0,  0,191,255,255,191,  0,  0,
      0,  0,191,255,255,191,  0,  0,  0,  0,191,255,255,191,  0,  0,
      0,  0,191,255,255,191,  0,  0,  0,  0,191,255,255,191,  0,  0,
     37,224,255,255,255,255,255,255,255,255,255,255,255,255,224, 37,
    125,255,255,255,255,255,255,255,255,255,255,255,255,255,255,125,
    128,255,255,255,255,255,255,255,255,255,255,255,255,255,255,128,
    128,255,255,255,255,255,174, 21, 21,174,255,255,255,255,255,128,
    128,255,255,255,255,255, 21,  0,  0, 21,255,255,255,255,255,128,
    128,255,255,255,255,255, 21,  0,  0, 21,255,255,255,255,255,128,
    128,255,255,255,255,255,174,  0,  0,174,255,255,255,255,255,128,
    128,255,255,255,255,255,255,  0,  0,255,255,255,255,255,255,128,
    128,255,255,255,255,255,255,  0,  0,255,255,255,255,255,255,128,
    128,255,255,255,255,255,255,128,128,255,255,255,255,255,255,128,
     97,255,255,255,255,255,255,255,255,255,255,255,255,255,255, 97,
      1, 97,128,128,128,128,128,128,128,128,128,128,128,128, 97,  1,
};

/* Blend one icon, white, into an A8B8G8R8 framebuffer at (ox, oy).
 * Call with the same guards as the HUD blit (sane pitch and size, DACR
 * opened around the call). White is the same in every channel order. */
static inline void vt_icon_draw(uint32_t *fb, int pitch, int fb_w, int fb_h,
                                int ox, int oy, const uint8_t *alpha, int w)
{
    for (int y = 0; y < VT_ICON_H; y++) {
        int py = oy + y;
        if (py < 0 || py >= fb_h)
            continue;
        for (int x = 0; x < w; x++) {
            int px = ox + x;
            uint32_t a = alpha[y * w + x];
            if (!a || px < 0 || px >= fb_w)
                continue;
            uint32_t *p = &fb[py * pitch + px];
            if (a == 255) {
                *p = 0xFFFFFFFFu;
                continue;
            }
            uint32_t d = *p, out = 0xFF000000u;
            for (int s = 0; s < 24; s += 8) {
                uint32_t c = (d >> s) & 0xFF;
                c = (255u * a + c * (255u - a) + 127u) / 255u;
                out |= c << s;
            }
            *p = out;
        }
    }
}

#endif /* VT_VPN_ICONS_H */
