/*
 * vtconf_util.c - messages, the duplicate check, automatic file names and
 * the subnet checks that go with the profile parser.
 */
#include "vtconf.h"

#include <string.h>

/* ---- messages ----------------------------------------------------------- */

static const char *const k_errors[VTC_E_COUNT] = {
	[VTC_OK]                = "no error",
	[VTC_E_ARG]             = "internal error: missing argument",
	[VTC_E_NUL]             = "the file contains a NUL byte; it is not a text profile",
	[VTC_E_LINE_TOO_LONG]   = "line is too long",
	[VTC_E_OUTSIDE_SECTION] = "setting before [Interface] or [Peer]",
	[VTC_E_BAD_SECTION]     = "unknown section; only [Interface] and [Peer] exist",
	[VTC_E_NO_EQUALS]       = "line is not \"key = value\"",
	[VTC_E_UNKNOWN_KEY]     = "unknown key",
	[VTC_E_FORK_KEY]        = "key from a non-standard protocol variant; a standard server would not understand this profile",
	[VTC_E_WRONG_SECTION]   = "key is in the wrong section",
	[VTC_E_EMPTY_VALUE]     = "key has no value",
	[VTC_E_BAD_KEY]         = "key is not 44 characters of base64 (32 bytes)",
	[VTC_E_ZERO_KEY]        = "key is all zeros",
	[VTC_E_BAD_PORT]        = "ListenPort is not a number from 0 to 65535",
	[VTC_E_BAD_FWMARK]      = "FwMark is neither off nor a number from 0 to 0xffffffff",
	[VTC_E_BAD_ADDRESS]     = "Address is not an IP address with an optional /prefix",
	[VTC_E_BAD_DNS]         = "DNS server is not a valid IP address",
	[VTC_E_BAD_MTU]         = "MTU is not a number from 576 to 65535",
	[VTC_E_BAD_BOOL]        = "SaveConfig is neither true nor false",
	[VTC_E_BAD_ALLOWED]     = "AllowedIPs entry is not an IP address with an optional /prefix",
	[VTC_E_TOO_MANY_ALLOWED] = "too many IPv4 AllowedIPs entries (at most 32)",
	[VTC_E_BAD_ENDPOINT]    = "Endpoint is not host:port with a port from 1 to 65535",
	[VTC_E_ENDPOINT_IPV6]   = "IPv6 endpoints are not supported yet",
	[VTC_E_BAD_KEEPALIVE]   = "PersistentKeepalive is neither off nor a number from 0 to 65535",
	[VTC_E_MULTIPLE_PEERS]  = "more than one [Peer]; only one is supported yet",
	[VTC_E_NO_PRIVATE_KEY]  = "no PrivateKey in [Interface]",
	[VTC_E_NO_PEER]         = "no [Peer] section",
	[VTC_E_NO_PUBLIC_KEY]   = "the [Peer] has no PublicKey",
	[VTC_E_NO_ENDPOINT]     = "the [Peer] has no Endpoint",
	[VTC_E_NO_ADDRESS]      = "no IPv4 Address in [Interface]",
	[VTC_E_NO_ALLOWED]      = "the [Peer] has no IPv4 AllowedIPs",
};

static const char *const k_notes[VTC_N_COUNT] = {
	[VTC_N_ADDRESS_IPV6]      = "IPv6 address ignored",
	[VTC_N_ADDRESS_EXTRA]     = "extra IPv4 address ignored; the first is used",
	[VTC_N_DNS_IPV6]          = "IPv6 DNS server ignored",
	[VTC_N_DNS_SEARCH]        = "DNS search domain ignored",
	[VTC_N_DNS_EXTRA]         = "DNS server ignored; the first two are used",
	[VTC_N_ALLOWED_IPV6]      = "IPv6 AllowedIPs entry ignored",
	[VTC_N_ALLOWED_HOST_BITS] = "AllowedIPs entry has host bits set; they are ignored",
	[VTC_N_ALLOWED_DUPLICATE] = "AllowedIPs entry listed twice",
	[VTC_N_ALLOWED_REMOVE]    = "AllowedIPs removal entry applied",
	[VTC_N_FWMARK]            = "FwMark ignored; it has no meaning on a Vita",
	[VTC_N_HOOK]              = "script ignored; scripts cannot run on a Vita",
	[VTC_N_SAVECONFIG]        = "SaveConfig ignored",
	[VTC_N_TABLE]             = "Table ignored; routes are added as usual",
	[VTC_N_MTU_CLAMPED]       = "MTU lowered to 1440, the most Wi-Fi allows",
	[VTC_N_DIRECTIVE_UNKNOWN] = "unknown vita-turbo-vpn setting ignored",
	[VTC_N_DIRECTIVE_BAD]     = "check must be generic or an https:// URL; ignored",
};

const char *vtc_error_text(int code)
{
	if (code < 0 || code >= VTC_E_COUNT)
		return "unknown error";
	return k_errors[code];
}

const char *vtc_note_text(int code)
{
	if (code <= 0 || code >= VTC_N_COUNT)
		return "note";
	return k_notes[code];
}

/* Bounded string builder that still counts what did not fit. */
typedef struct Out {
	char *buf;
	size_t cap, len;
} Out;

static void put(Out *o, const char *s)
{
	for (; *s; s++, o->len++)
		if (o->len + 1 < o->cap)
			o->buf[o->len] = *s;
}

static void put_uint(Out *o, uint32_t v)
{
	char d[11];
	int i = 10;

	d[i] = '\0';
	do {
		d[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(o, d + i);
}

static size_t done(Out *o)
{
	if (o->cap)
		o->buf[o->len < o->cap ? o->len : o->cap - 1] = '\0';
	return o->len;
}

static size_t format(char *buf, size_t cap, uint32_t line, const char *item, const char *text)
{
	Out o = { buf, buf ? cap : 0, 0 };

	if (line) {
		put(&o, "Line ");
		put_uint(&o, line);
		put(&o, ": ");
	}
	if (item && item[0]) {
		put(&o, "`");
		put(&o, item);
		put(&o, "': ");
	}
	put(&o, text);
	return done(&o);
}

size_t vtc_format_error(const VtcResult *res, char *buf, size_t cap)
{
	if (!res)
		return format(buf, cap, 0, NULL, vtc_error_text(VTC_E_ARG));
	return format(buf, cap, res->error_line, res->error_item, vtc_error_text(res->error));
}

size_t vtc_format_note(const VtcNote *n, char *buf, size_t cap)
{
	if (!n)
		return format(buf, cap, 0, NULL, vtc_error_text(VTC_E_ARG));
	return format(buf, cap, n->line, n->item, vtc_note_text(n->code));
}

/* ---- duplicate check ---------------------------------------------- */

/* End of the text once one trailing newline (LF or CRLF) is dropped. */
static size_t trimmed_len(const char *s, size_t n)
{
	if (n && s[n - 1] == '\n') {
		n--;
		if (n && s[n - 1] == '\r')
			n--;
	}
	return n;
}

size_t vtc_normalise(const char *in, size_t len, char *out)
{
	size_t i, o = 0;

	if (!in || !out)
		return 0;
	len = trimmed_len(in, len);
	for (i = 0; i < len; i++) {
		if (in[i] == '\r' && i + 1 < len && in[i + 1] == '\n')
			continue;
		out[o++] = in[i];
	}
	return o;
}

/* Next byte of the normalised text, or -1 at the end. */
static int next_norm(const char *s, size_t n, size_t *i)
{
	if (*i < n && s[*i] == '\r' && *i + 1 < n && s[*i + 1] == '\n')
		(*i)++;
	if (*i >= n)
		return -1;
	return (unsigned char)s[(*i)++];
}

int vtc_same_text(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t i = 0, j = 0;
	int ca, cb;

	if (!a || !b)
		return 0;
	alen = trimmed_len(a, alen);
	blen = trimmed_len(b, blen);
	do {
		ca = next_norm(a, alen, &i);
		cb = next_norm(b, blen, &j);
		if (ca != cb)
			return 0;
	} while (ca >= 0);
	return 1;
}

/* ---- automatic file names ----------------------------------------- */

static int name_char(unsigned char c)
{
	return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ||
	       c == '.' || c == '-' || c == '_';
}

int vtc_qr_file_name(const char *host, vtc_exists_fn exists, void *ctx, char *out, size_t cap)
{
	char base[VTC_NAME_BASE_MAX + 1];
	size_t n = 0, i;
	uint32_t k;

	if (!host || !exists || !out || cap < VTC_FILE_NAME_MAX)
		return -1;
	/* The parser only lets through names made of these characters; anything
	 * else becomes '_' so the result is always a plain file name. */
	for (i = 0; host[i] && n < VTC_NAME_BASE_MAX; i++)
		base[n++] = name_char((unsigned char)host[i]) ? host[i] : '_';
	/* no dots or dashes at the ends ("example.org." or a cut name) */
	while (n && (base[n - 1] == '.' || base[n - 1] == '-'))
		n--;
	i = 0;
	while (i < n && base[i] == '.')
		i++;
	if (i == n)
		return -1;
	base[n] = '\0';

	for (k = 1; k <= 999; k++) {
		Out o = { out, cap, 0 };
		put(&o, base + i);
		if (k > 1) {
			put(&o, "-");
			put_uint(&o, k);
		}
		put(&o, ".conf");
		done(&o);
		if (!exists(ctx, out))
			return 0;
	}
	out[0] = '\0';
	return -2;
}

/* ---- subnets ------------------------------------------------------------ */

static uint32_t mask_of(unsigned prefix)
{
	return prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
}

int vtc_covers_all(const VtPrefix *list, unsigned count)
{
	/* Walk the address space upwards: at each point find a prefix that
	 * contains it and jump past that prefix's end. count is small. */
	uint64_t at = 0;
	unsigned i;
	int moved;

	if (!list)
		return 0;
	while (at <= 0xFFFFFFFFu) {
		moved = 0;
		for (i = 0; i < count; i++) {
			uint32_t m = mask_of(list[i].prefix > 32 ? 32 : list[i].prefix);
			uint64_t lo = list[i].net & m, hi = lo + ((uint64_t)~m & 0xFFFFFFFFu);
			if (lo <= at && at <= hi) {
				at = hi + 1;
				moved = 1;
			}
		}
		if (!moved)
			return 0;
	}
	return 1;
}

int vtc_subnet_clash(const VtProfile *p, uint32_t local_net, uint8_t local_prefix, unsigned *index)
{
	unsigned i, count, lp = local_prefix > 32 ? 32 : local_prefix;
	int full;

	if (!p || (p->flags & VT_PROFILE_NO_ROUTES))
		return VTC_CLASH_NONE;
	count = p->allowed_count > VT_MAX_ALLOWED ? VT_MAX_ALLOWED : p->allowed_count;
	full = vtc_covers_all(p->allowed, count);
	for (i = 0; i < count; i++) {
		unsigned ap = p->allowed[i].prefix > 32 ? 32 : p->allowed[i].prefix;
		unsigned common = ap < lp ? ap : lp;
		int kind;

		if ((p->allowed[i].net ^ local_net) & mask_of(common))
			continue; /* disjoint */
		if (ap >= lp)
			kind = VTC_CLASH_INSIDE;
		else if (!full)
			kind = VTC_CLASH_SHADOWED;
		else
			continue;
		if (index)
			*index = i;
		return kind;
	}
	return VTC_CLASH_NONE;
}
