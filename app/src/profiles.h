/*
 * profiles.h - the profile list as it is on the storages, and
 * the file side of QR import.
 *
 * Shared by the real back end, which points it at the console's profiles
 * folders, and by the mock, which can point it at a folder on a computer.
 * Each profile is parsed with the shared parser (conf/); what the
 * screens need is kept, and every buffer that held a key is wiped as soon
 * as the profile has been read.
 */
#ifndef PROFILES_H
#define PROFILES_H

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "../../include/vt_paths.h"

/* The storages, in the order they are read */
enum { PF_UX0, PF_UMA0, PF_IMC0, PF_UR0, PF_NSTORES };

/* "ux0", "uma0", ... */
extern const char *const PF_STORE_NAME[PF_NSTORES];

/* The profiles folder on each storage, as the console names it */
#define PF_DATA_DIR    VT_PROFILE_DIR
#define PF_FOLDER      PF_DATA_DIR "/profiles"

/* Largest profile read; a .conf file is a few hundred bytes */
#define PF_TEXT_MAX    16384
/* Largest .zip read (it is read into memory whole) */
#define PF_ZIP_MAX     (8 * 1024 * 1024)

/*
 * Where each storage's profiles folder is on this system, without a
 * trailing slash; NULL = that storage is not there. The default is the
 * console's own: "ux0:data/vita-turbo-vpn/profiles" and so on.
 */
void pf_set_dirs(const char *const dirs[PF_NSTORES]);

/*
 * Read every folder again. wlan_ip and wlan_prefix are the Wi-Fi address
 * and prefix length for the subnet-clash warning; prefix 0 when Wi-Fi
 * is not connected, which skips the check.
 */
void pf_scan(uint32_t wlan_ip, uint8_t wlan_prefix);
int pf_count(void);
const BkProfile *pf_get(int index);
/* The storages profiles were found on, a set of BK_STORE_* */
unsigned pf_storages(void);

/*
 * Parse a profile's text into the fields a BkProfile shows. On a parse
 * error p->warnings has BK_WARN_INVALID and p->invalid_reason the parser's
 * message, and it returns -1. Nothing of the key material is kept.
 */
int pf_fill(const char *text, size_t len, uint32_t wlan_ip, uint8_t wlan_prefix, BkProfile *p);

/*
 * Where a listed profile's text is: the file, or the zip it is in with
 * *zip_index its entry (-1 = a loose file). NULL for a bad index.
 */
const char *pf_source(int index, int *zip_index);
/* The listed profile read from real (and zip_index), or -1 */
int pf_find(const char *real, int zip_index);
/*
 * Read a profile's text from where pf_source said into buf (cap bytes):
 * its length, or -1. Uses no state of the list, so another thread may call
 * it while the list is read again. The caller wipes buf after use.
 */
long pf_read_source(const char *real, int zip_index, char *buf, size_t cap);

/* QR import (backend.h, bk_import_text) against the folders above */
int pf_import(const char *text, size_t len, BkImport *out);

/* Create the ux0 profiles folder if it is missing */
void pf_make_folder(void);

#endif
