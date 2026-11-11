#ifndef VPNTURBO_HUD_I18N_H
#define VPNTURBO_HUD_I18N_H

/*
 * HUD label localisation (kernel-side, ASCII only).
 *
 * Copied from vita-turbo-wifi kernel/hud_i18n.h; the
 * tokens are this project's (the tunnel states), the rules are
 * unchanged:
 *
 * The HUD renders with an 8x8 ASCII font, so translations MUST be ASCII:
 * drop accents/diacritics (e.g. "conexion" not "conexión"). Scripts the font
 * cannot draw at all (Japanese, Korean, Chinese, Russian/Cyrillic) fall back to
 * English - do NOT put non-ASCII bytes in the table.
 *
 * Only genuine WORDS are translated; universal acronyms stay as-is in every
 * language (VPN, TUNN, EXIT). Keep translations SHORT - the HUD line has limited width.
 *
 * lang is the Vita system-language index (the shell plugin's vtw_i18n_lang(),
 * passed to the kernel in VtOverlay.lang).
 */

enum hud_tr_tok {
	HTR_ACTIVE = 0,   /* tunnel up                 */
	HTR_INACTIVE,     /* tunnel off                */
	HTR_CONNECTING,   /* handshaking / retrying    */
	HTR_BLOCKED,      /* resume lockdown     */
	HTR_PAUSED,       /* user pause          */
	HTR_RESTARTING,   /* the restart banner  */
	HTR_COUNT
};

/* Return the ASCII label for token `tok` in language `lang`, English fallback
 * for any non-Latin or unknown language. Never returns NULL. */
const char *hud_tr(int lang, int tok);

#endif /* VPNTURBO_HUD_I18N_H */
