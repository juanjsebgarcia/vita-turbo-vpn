/*
 * hud_i18n.c - ASCII HUD label translations (see hud_i18n.h).
 *
 * Copied from vita-turbo-wifi kernel/hud_i18n.c; only
 * the words changed (this HUD's tunnel states).
 *
 * Machine-quality, ASCII only (accents dropped so the 8x8 font can draw them),
 * short to fit the HUD line. Non-Latin-script languages (Japanese, Russian,
 * Korean, Chinese) fall back to English - review/refine by native speakers.
 * Column order: HTR_ACTIVE, HTR_INACTIVE, HTR_CONNECTING, HTR_BLOCKED,
 * HTR_PAUSED, HTR_RESTARTING. Row index = Vita system-language id.
 */
#include "hud_i18n.h"

#define WT_HUD_LANG_COUNT 20

static const char *const T[WT_HUD_LANG_COUNT][HTR_COUNT] = {
/* 0  JA (non-Latin -> EN) */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 1  EN-US              */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 2  FR                 */ {"ACTIF","INACTIF","CONNEXION","BLOQUE","EN PAUSE","REDEMARRAGE..."},
/* 3  ES                 */ {"ACTIVO","INACTIVO","CONECTANDO","BLOQUEADO","EN PAUSA","REINICIANDO..."},
/* 4  DE                 */ {"AKTIV","INAKTIV","VERBINDEN","GESPERRT","PAUSIERT","NEUSTART..."},
/* 5  IT                 */ {"ATTIVO","INATTIVO","CONNESSIONE","BLOCCATO","IN PAUSA","RIAVVIO..."},
/* 6  NL                 */ {"ACTIEF","INACTIEF","VERBINDEN","GEBLOKKEERD","GEPAUZEERD","HERSTARTEN..."},
/* 7  PT-PT              */ {"ATIVO","INATIVO","A LIGAR","BLOQUEADO","EM PAUSA","A REINICIAR..."},
/* 8  RU (non-Latin->EN) */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 9  KO (non-Latin->EN) */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 10 ZH-T (non-Latin)   */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 11 ZH-S (non-Latin)   */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 12 FI                 */ {"KAYTOSSA","POIS","YHDISTAA","ESTETTY","TAUOLLA","UUDELLEENKAYNNISTYS..."},
/* 13 SV                 */ {"AKTIV","INAKTIV","ANSLUTER","BLOCKERAD","PAUSAD","STARTAR OM..."},
/* 14 DA                 */ {"AKTIV","INAKTIV","FORBINDER","BLOKERET","PA PAUSE","GENSTARTER..."},
/* 15 NO                 */ {"AKTIV","INAKTIV","KOBLER TIL","BLOKKERT","PAUSET","STARTER PA NYTT..."},
/* 16 PL                 */ {"AKTYWNY","NIEAKTYWNY","LACZENIE","ZABLOKOWANY","WSTRZYMANO","PONOWNE URUCHAMIANIE..."},
/* 17 PT-BR              */ {"ATIVO","INATIVO","CONECTANDO","BLOQUEADO","PAUSADO","REINICIANDO..."},
/* 18 EN-GB              */ {"ACTIVE","INACTIVE","CONNECTING","BLOCKED","PAUSED","RESTARTING..."},
/* 19 TR                 */ {"AKTIF","PASIF","BAGLANIYOR","ENGELLI","DURAKLATILDI","YENIDEN BASLATILIYOR..."},
};

const char *hud_tr(int lang, int tok)
{
	if (tok < 0 || tok >= HTR_COUNT)
		return "";
	if (lang < 0 || lang >= WT_HUD_LANG_COUNT)
		lang = 1;                      /* English(US) fallback                  */
	return T[lang][tok];
}
