/*
 * vtconf.h - .conf profile parser for vita-turbo-vpn.
 *
 * Reads a standard .conf (the format of the reference tools) into a VtcResult, then into the VtProfile the kernel module
 * takes. Portable C99: no allocation, no I/O, no locale; the caller owns
 * every buffer. See README.md for the behaviour in full.
 */
#ifndef VTCONF_H
#define VTCONF_H

#include <stddef.h>
#include <stdint.h>

#define VPNTURBO_NO_PROTOTYPES
#include "../include/vpnturbo.h"

#define VTC_LINE_MAX   2048 /* bytes of a line once comments and whitespace are removed */
#define VTC_HOST_MAX   256  /* endpoint host, NUL included (DNS names are at most 253) */
#define VTC_URL_MAX    160  /* check URL, NUL included */
#define VTC_ITEM_MAX   48   /* key or value quoted in an error or note, NUL included */
#define VTC_MAX_NOTES  16
#define VTC_NAME_BASE_MAX 64 /* host part of an automatic file name */

/* Parse errors. vtc_error_text() gives the wording. */
enum {
	VTC_OK = 0,
	VTC_E_ARG,             /* NULL argument */
	VTC_E_NUL,             /* NUL byte in the text */
	VTC_E_LINE_TOO_LONG,
	VTC_E_OUTSIDE_SECTION, /* key before [Interface] or [Peer] */
	VTC_E_BAD_SECTION,     /* [Something] else */
	VTC_E_NO_EQUALS,       /* not "key = value" */
	VTC_E_UNKNOWN_KEY,
	VTC_E_FORK_KEY,        /* obfuscation key from a non-standard fork */
	VTC_E_WRONG_SECTION,   /* a known key in the other section */
	VTC_E_EMPTY_VALUE,
	VTC_E_BAD_KEY,         /* not 44 characters of base64 for 32 bytes */
	VTC_E_ZERO_KEY,        /* private or public key all zero */
	VTC_E_BAD_PORT,        /* ListenPort */
	VTC_E_BAD_FWMARK,
	VTC_E_BAD_ADDRESS,
	VTC_E_BAD_DNS,
	VTC_E_BAD_MTU,
	VTC_E_BAD_BOOL,        /* SaveConfig */
	VTC_E_BAD_ALLOWED,
	VTC_E_TOO_MANY_ALLOWED,
	VTC_E_BAD_ENDPOINT,
	VTC_E_ENDPOINT_IPV6,
	VTC_E_BAD_KEEPALIVE,
	VTC_E_MULTIPLE_PEERS,
	VTC_E_NO_PRIVATE_KEY,
	VTC_E_NO_PEER,
	VTC_E_NO_PUBLIC_KEY,
	VTC_E_NO_ENDPOINT,
	VTC_E_NO_ADDRESS,      /* no IPv4 Address */
	VTC_E_NO_ALLOWED,      /* no IPv4 AllowedIPs */
	VTC_E_COUNT
};

/* Notes: lines or entries that were accepted but not (fully) used. */
enum {
	VTC_N_ADDRESS_IPV6 = 1,
	VTC_N_ADDRESS_EXTRA,   /* IPv4 address after the first */
	VTC_N_DNS_IPV6,
	VTC_N_DNS_SEARCH,      /* search domain */
	VTC_N_DNS_EXTRA,       /* IPv4 server after the second */
	VTC_N_ALLOWED_IPV6,
	VTC_N_ALLOWED_HOST_BITS, /* host bits set; masked off */
	VTC_N_ALLOWED_DUPLICATE,
	VTC_N_ALLOWED_REMOVE,  /* "-prefix" entry: removes an earlier one */
	VTC_N_FWMARK,
	VTC_N_HOOK,            /* PreUp, PostUp, PreDown, PostDown */
	VTC_N_SAVECONFIG,
	VTC_N_TABLE,           /* Table other than off or auto */
	VTC_N_MTU_CLAMPED,
	VTC_N_DIRECTIVE_UNKNOWN,
	VTC_N_DIRECTIVE_BAD,
	VTC_N_COUNT
};

/* Connection check from "# vita-turbo-vpn: check = ..." (no presets). */
enum {
	VTC_CHECK_NONE = 0,   /* no directive: the app's global setting applies */
	VTC_CHECK_GENERIC,
	VTC_CHECK_URL,        /* check_url holds an https:// URL */
};

typedef struct VtcNote {
	uint32_t line;
	uint16_t code;              /* VTC_N_* */
	char     item[VTC_ITEM_MAX]; /* the entry or key concerned */
} VtcNote;

typedef struct VtcResult {
	int      error;             /* VTC_OK or VTC_E_* */
	uint32_t error_line;        /* 1-based; 0 = the file as a whole */
	char     error_item[VTC_ITEM_MAX]; /* key or entry named in the error; never key material */

	/* [Interface] */
	uint8_t  private_key[VT_KEY_LEN];
	uint32_t address;           /* first IPv4 Address */
	uint8_t  address_prefix;
	uint8_t  table_off;         /* Table = off */
	uint16_t mtu;               /* 0 = default */
	uint16_t listen_port;       /* 0 = random */
	uint8_t  dns_count;
	uint32_t dns[2];

	/* [Peer] */
	uint8_t  peer_public[VT_KEY_LEN];
	uint8_t  preshared[VT_KEY_LEN];
	uint8_t  has_preshared;
	uint8_t  allowed_count;
	uint8_t  full_tunnel;       /* IPv4 AllowedIPs cover every address */
	VtPrefix allowed[VT_MAX_ALLOWED];
	uint16_t keepalive_s;
	uint16_t endpoint_port;
	uint8_t  endpoint_is_ip;    /* host is a dotted quad, endpoint_ip set */
	uint32_t endpoint_ip;
	char     endpoint_host[VTC_HOST_MAX];

	/* our directive */
	uint8_t  check;             /* VTC_CHECK_* */
	uint32_t check_line;
	char     check_url[VTC_URL_MAX];

	uint16_t note_count;
	uint16_t notes_dropped;     /* notes beyond VTC_MAX_NOTES */
	VtcNote  notes[VTC_MAX_NOTES];
} VtcResult;

/*
 * Parses len bytes of text (no NUL terminator needed; a NUL byte inside is
 * an error). Returns res->error. On failure the key fields are wiped and
 * error, error_line and error_item say why; nothing else is meaningful.
 * Call vtc_wipe(res, sizeof *res) when done with a result.
 */
int vtc_parse(const char *text, size_t len, VtcResult *res);

/*
 * Fills a VtProfile from a successful result. name is the file name, with
 * or without ".conf" (removed if present), truncated to fit on a UTF-8
 * character boundary. For a hostname endpoint endpoint_ip is left 0 for
 * the app to fill after resolving. Returns 0, or -1 if res holds an error.
 */
int vtc_to_profile(const VtcResult *res, const char *name, VtProfile *out);

/* Zeroes memory in a way the compiler may not remove (keys). */
void vtc_wipe(void *p, size_t n);

/* English wording, for logs and as the fallback UI text. */
const char *vtc_error_text(int code);
const char *vtc_note_text(int code);

/*
 * "Line 7: unknown key `Jc'"-style message into buf (always terminated if
 * cap > 0). Returns the length the full message needs, like snprintf.
 */
size_t vtc_format_error(const VtcResult *res, char *buf, size_t cap);
size_t vtc_format_note(const VtcNote *note, char *buf, size_t cap);

/*
 * Duplicate check: CRLF becomes LF and one trailing newline is
 * ignored; everything else is compared byte for byte.
 * vtc_normalise writes the normalised text to out (which may equal in;
 * it needs len bytes) and returns its length. It is also the input to the
 * profiles.meta hash.
 */
size_t vtc_normalise(const char *in, size_t len, char *out);
int vtc_same_text(const char *a, size_t alen, const char *b, size_t blen);

/*
 * Automatic file name for a QR import: "<host>.conf", or
 * "<host>-2.conf", "<host>-3.conf", ... while exists() says the name is
 * taken. exists() gets the bare file name and returns nonzero if taken.
 * The host is cut to VTC_NAME_BASE_MAX characters, so a buffer of
 * VTC_FILE_NAME_MAX always suffices. Returns 0; -1 for a NULL argument,
 * an empty host or a buffer too small; -2 if every suffix up to -999 is
 * taken.
 */
#define VTC_FILE_NAME_MAX (VTC_NAME_BASE_MAX + 4 + 5 + 1) /* base, "-999", ".conf", NUL */
typedef int (*vtc_exists_fn)(void *ctx, const char *file_name);
int vtc_qr_file_name(const char *host, vtc_exists_fn exists, void *ctx, char *out, size_t cap);

/* 1 if the prefixes together cover every IPv4 address. */
int vtc_covers_all(const VtPrefix *list, unsigned count);

/*
 * Subnet clash with the Wi-Fi subnet. Returns VTC_CLASH_NONE or
 * a kind, and the first offending prefix's index in *index (may be NULL).
 *   INSIDE:   an AllowedIPs prefix lies within the local subnet (or equals
 *             it): those addresses are both "local" and "behind the tunnel".
 *   SHADOWED: a split tunnel's prefix contains the local subnet, so part of
 *             the remote range is unreachable while on this network.
 * A full tunnel containing the local subnet is not a clash: that is the
 * "allow local network" exception. Table = off profiles add no
 * routes and never clash.
 */
enum { VTC_CLASH_NONE = 0, VTC_CLASH_INSIDE, VTC_CLASH_SHADOWED };
int vtc_subnet_clash(const VtProfile *p, uint32_t local_net, uint8_t local_prefix, unsigned *index);

#endif
