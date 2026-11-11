/*
 * vtconf.c - .conf profile parser.
 *
 * Line handling follows the reference tools: a line ends at '\n', everything
 * from '#' on is a comment, every whitespace byte (space, \t \n \v \f \r)
 * is removed, and what is left is either a section header or key=value
 * split at the first '='. Section names and keys are case-insensitive.
 * Values are checked as the reference tools check them, then reduced to
 * what v1 uses (one peer, IPv4); everything else is reported as a note.
 */
#include "vtconf.h"

#include <string.h>

/* ---- characters --------------------------------------------------------- */

/* The same set the reference tools strip; locale-independent. */
static int is_space(unsigned char c)
{
	return c == ' ' || (c >= '\t' && c <= '\r');
}

static int is_digit(unsigned char c)
{
	return c >= '0' && c <= '9';
}

static int is_hex(unsigned char c)
{
	return is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}

static int is_alnum(unsigned char c)
{
	return is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}

static unsigned char lower(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ? (unsigned char)(c | 0x20) : c;
}

/* Case-insensitive: s[0..n) equals the NUL-terminated word. */
static int eq_ci(const char *s, size_t n, const char *word)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!word[i] || lower((unsigned char)s[i]) != lower((unsigned char)word[i]))
			return 0;
	return word[n] == '\0';
}

/* Case-insensitive prefix test. */
static int starts_ci(const char *s, size_t n, const char *word)
{
	size_t i;

	for (i = 0; word[i]; i++)
		if (i >= n || lower((unsigned char)s[i]) != lower((unsigned char)word[i]))
			return 0;
	return 1;
}

static const char *find(const char *s, size_t n, char c)
{
	return memchr(s, c, n);
}

/* ---- numbers and addresses ---------------------------------------------- */

/* Decimal, digits only (no sign, no space), at most max (which is >= 9). */
static int parse_uint(const char *s, size_t n, uint32_t max, uint32_t *out)
{
	uint32_t v = 0, d;
	int big = 0;
	size_t i;

	if (n == 0)
		return 0;
	for (i = 0; i < n; i++) {
		if (!is_digit((unsigned char)s[i]))
			return 0;
		d = (uint32_t)(s[i] - '0');
		if (big || v > (max - d) / 10)
			big = 1; /* too big; keep checking the syntax */
		else
			v = v * 10 + d;
	}
	if (big)
		return 0;
	*out = v;
	return 1;
}

/*
 * Strict dotted quad, as inet_pton(AF_INET): four decimal parts 0-255, no
 * leading zeros, nothing else.
 */
static int parse_ipv4(const char *s, size_t n, uint32_t *out)
{
	uint32_t ip = 0, part = 0;
	unsigned parts = 0, digits = 0;
	size_t i;

	for (i = 0; i <= n; i++) {
		if (i == n || s[i] == '.') {
			if (digits == 0 || ++parts > 4)
				return 0;
			ip = (ip << 8) | part;
			part = 0;
			digits = 0;
			continue;
		}
		if (!is_digit((unsigned char)s[i]))
			return 0;
		if (digits == 1 && part == 0)
			return 0; /* leading zero */
		part = part * 10 + (uint32_t)(s[i] - '0');
		if (part > 255)
			return 0;
		digits++;
	}
	if (parts != 4)
		return 0;
	*out = ip;
	return 1;
}

/*
 * IPv6 text form, as inet_pton(AF_INET6): up to eight groups of 1-4 hex
 * digits, one "::" standing for at least one zero group, optionally ending
 * in a dotted quad worth two groups. Only validated; v1 does not use IPv6.
 */
static int valid_ipv6(const char *s, size_t n)
{
	unsigned groups = 0, digits;
	int gap = 0;
	size_t i = 0, j;
	uint32_t v4;

	if (n == 0)
		return 0;
	if (s[0] == ':') {
		if (n < 2 || s[1] != ':')
			return 0;
		gap = 1;
		i = 2;
		if (n == 2)
			return 1;
	}
	for (;;) {
		for (j = i, digits = 0; j < n && is_hex((unsigned char)s[j]); j++)
			digits++;
		if (j < n && s[j] == '.') {
			/* a dotted quad takes the rest */
			if (!parse_ipv4(s + i, n - i, &v4))
				return 0;
			groups += 2;
			break;
		}
		if (digits == 0 || digits > 4)
			return 0;
		groups++;
		if (j == n)
			break;
		if (s[j] != ':' || ++j == n)
			return 0; /* not a separator, or a trailing single ':' */
		if (s[j] == ':') {
			if (gap)
				return 0;
			gap = 1;
			if (++j == n)
				break;
		}
		i = j;
	}
	return gap ? groups <= 7 : groups == 8;
}

static uint32_t prefix_mask(unsigned prefix)
{
	return prefix == 0 ? 0 : 0xFFFFFFFFu << (32 - prefix);
}

/* ---- base64 keys -------------------------------------------------------- */

/*
 * Value of one base64 character, or a value with bit 8 set if it is not
 * one. Branch-free in the character so key bytes do not steer the timing.
 */
static uint32_t b64_value(unsigned char c)
{
	uint32_t x = c, v = 0, ok = 0, m;

#define B64_RANGE(lo, hi, base) \
	m = (((x - (lo)) | ((hi) - x)) >> 31) - 1; \
	v |= m & (x - (lo) + (base)); \
	ok |= m;
	B64_RANGE('A', 'Z', 0)
	B64_RANGE('a', 'z', 26)
	B64_RANGE('0', '9', 52)
	B64_RANGE('+', '+', 62)
	B64_RANGE('/', '/', 63)
#undef B64_RANGE
	return (v & 63) | (~ok & 0x100);
}

/*
 * A tunnel key: exactly 44 characters, 43 of base64 then '=', and the
 * two spare bits of the last character zero (as the reference tools require).
 */
static int decode_key(const char *s, size_t n, uint8_t key[VT_KEY_LEN])
{
	uint32_t acc = 0, bad = 0, v;
	unsigned bits = 0, out = 0;
	size_t i;

	if (n != 44 || s[43] != '=')
		return 0;
	for (i = 0; i < 43; i++) {
		v = b64_value((unsigned char)s[i]);
		bad |= v;
		acc = (acc << 6) | (v & 63);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			key[out++] = (uint8_t)(acc >> bits);
		}
	}
	/* 43 * 6 = 258 bits: 32 bytes and 2 left over, which must be zero */
	bad |= (acc & 3) << 8;
	if (bad & 0x100) {
		vtc_wipe(key, VT_KEY_LEN);
		return 0;
	}
	return 1;
}

static int key_is_zero(const uint8_t key[VT_KEY_LEN])
{
	uint8_t acc = 0;
	unsigned i;

	for (i = 0; i < VT_KEY_LEN; i++)
		acc |= key[i];
	return acc == 0;
}

void vtc_wipe(void *p, size_t n)
{
	volatile uint8_t *b = p;

	while (n--)
		*b++ = 0;
}

/* ---- keys we know ------------------------------------------------------- */

enum { SEC_NONE = 0, SEC_INTERFACE = 1, SEC_PEER = 2 };

enum {
	K_PRIVATEKEY, K_LISTENPORT, K_FWMARK,
	K_ADDRESS, K_DNS, K_MTU, K_TABLE,
	K_PREUP, K_POSTUP, K_PREDOWN, K_POSTDOWN, K_SAVECONFIG,
	K_PUBLICKEY, K_PRESHAREDKEY, K_ALLOWEDIPS, K_ENDPOINT, K_KEEPALIVE,
};

static const struct {
	const char *name;
	uint8_t section;
	uint8_t quick; /* setup key: an empty value is allowed */
} k_keys[] = {
	[K_PRIVATEKEY]   = { "PrivateKey", SEC_INTERFACE, 0 },
	[K_LISTENPORT]   = { "ListenPort", SEC_INTERFACE, 0 },
	[K_FWMARK]       = { "FwMark", SEC_INTERFACE, 0 },
	[K_ADDRESS]      = { "Address", SEC_INTERFACE, 1 },
	[K_DNS]          = { "DNS", SEC_INTERFACE, 1 },
	[K_MTU]          = { "MTU", SEC_INTERFACE, 1 },
	[K_TABLE]        = { "Table", SEC_INTERFACE, 1 },
	[K_PREUP]        = { "PreUp", SEC_INTERFACE, 1 },
	[K_POSTUP]       = { "PostUp", SEC_INTERFACE, 1 },
	[K_PREDOWN]      = { "PreDown", SEC_INTERFACE, 1 },
	[K_POSTDOWN]     = { "PostDown", SEC_INTERFACE, 1 },
	[K_SAVECONFIG]   = { "SaveConfig", SEC_INTERFACE, 1 },
	[K_PUBLICKEY]    = { "PublicKey", SEC_PEER, 0 },
	[K_PRESHAREDKEY] = { "PresharedKey", SEC_PEER, 0 },
	[K_ALLOWEDIPS]   = { "AllowedIPs", SEC_PEER, 0 },
	[K_ENDPOINT]     = { "Endpoint", SEC_PEER, 0 },
	[K_KEEPALIVE]    = { "PersistentKeepalive", SEC_PEER, 0 },
};

#define N_KEYS (sizeof k_keys / sizeof k_keys[0])

/*
 * Keys added by forks that obfuscate the handshake. A standard peer would
 * not understand a client using them, so such a profile cannot work here.
 */
static const char *const k_fork_keys[] = {
	"Jc", "Jmin", "Jmax", "S1", "S2", "S3", "S4",
	"H1", "H2", "H3", "H4", "I1", "I2", "I3", "I4", "I5",
	"J1", "J2", "J3", "Itime",
};

/* ---- parser state ------------------------------------------------------- */

typedef struct Ctx {
	VtcResult *r;
	uint32_t line;
	int section;
	unsigned peers;
	uint32_t peer_line;
	uint8_t have_private, have_public, have_endpoint, have_address;
} Ctx;

static int is_b64(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	       (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

/* Copies an item for display: control and non-ASCII bytes become '?',
 * and a long item is cut with "...". An item holding a run of 20 or more
 * base64 characters may be key material (a key pasted on its own line, or
 * before [Interface]), so it is replaced by "(hidden)": messages never
 * quote keys. */
static void copy_item(char dst[VTC_ITEM_MAX], const char *s, size_t n)
{
	size_t i, max = VTC_ITEM_MAX - 1, run = 0;
	int cut = 0;

	for (i = 0; i < n; i++) {
		run = is_b64((unsigned char)s[i]) ? run + 1 : 0;
		if (run >= 20) {
			memcpy(dst, "(hidden)", sizeof("(hidden)"));
			return;
		}
	}

	if (n > max) {
		n = max - 3;
		cut = 1;
	}
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		dst[i] = (c < 0x20 || c >= 0x7f) ? '?' : (char)c;
	}
	if (cut) {
		memcpy(dst + i, "...", 3);
		i += 3;
	}
	dst[i] = '\0';
}

static int fail(Ctx *c, int code, const char *item, size_t n)
{
	c->r->error = code;
	c->r->error_line = c->line;
	copy_item(c->r->error_item, item ? item : "", item ? n : 0);
	return code;
}

static int fail_word(Ctx *c, int code, const char *word)
{
	return fail(c, code, word, strlen(word));
}

static void note(Ctx *c, int code, const char *item, size_t n)
{
	VtcResult *r = c->r;
	VtcNote *nt;

	if (r->note_count >= VTC_MAX_NOTES) {
		if (r->notes_dropped < 0xFFFF)
			r->notes_dropped++;
		return;
	}
	nt = &r->notes[r->note_count++];
	nt->line = c->line;
	nt->code = (uint16_t)code;
	copy_item(nt->item, item, n);
}

/* ---- values ------------------------------------------------------------- */

static int set_key(Ctx *c, int k, const char *v, size_t n, uint8_t key[VT_KEY_LEN])
{
	/* The error names the key, never the value: it may be the private key. */
	if (!decode_key(v, n, key))
		return fail_word(c, VTC_E_BAD_KEY, k_keys[k].name);
	if (k != K_PRESHAREDKEY && key_is_zero(key))
		return fail_word(c, VTC_E_ZERO_KEY, k_keys[k].name);
	return VTC_OK;
}

/* Reference tools: "off", or a number 0-0xffffffff, decimal or with a 0x prefix.
 * Checked so a file the reference tools would refuse is refused here too, then ignored. */
static int check_fwmark(Ctx *c, const char *v, size_t n)
{
	uint32_t mark = 0;
	size_t i;

	if (eq_ci(v, n, "off"))
		return VTC_OK;
	if (n > 2 && v[0] == '0' && v[1] == 'x') {
		for (i = 2; i < n - 1 && v[i] == '0'; i++)
			;
		if (n - i > 8)
			return fail(c, VTC_E_BAD_FWMARK, v, n);
		for (; i < n; i++) {
			unsigned char d = (unsigned char)v[i];
			if (!is_hex(d))
				return fail(c, VTC_E_BAD_FWMARK, v, n);
			mark = (mark << 4) | (uint32_t)(is_digit(d) ? d - '0' : (d | 0x20) - 'a' + 10);
		}
	} else if (!parse_uint(v, n, 0xFFFFFFFFu, &mark)) {
		return fail(c, VTC_E_BAD_FWMARK, v, n);
	}
	if (mark)
		note(c, VTC_N_FWMARK, v, n);
	return VTC_OK;
}

/* Splits "ip/prefix". Returns 0 on bad syntax. */
static int split_cidr(const char *s, size_t n, size_t *ip_len, const char **pfx, size_t *pfx_len)
{
	const char *slash = find(s, n, '/');

	if (!slash) {
		*ip_len = n;
		*pfx = NULL;
		*pfx_len = 0;
		return 1;
	}
	*ip_len = (size_t)(slash - s);
	*pfx = slash + 1;
	*pfx_len = n - *ip_len - 1;
	return 1;
}

/* Setup key Address: comma list; empty entries are skipped, as the shell's
 * word splitting does. IPv4 entries are a.b.c.d[/0-32]. */
static int do_address(Ctx *c, const char *v, size_t n)
{
	VtcResult *r = c->r;
	size_t pos = 0;

	while (pos < n) {
		const char *e = v + pos, *comma = find(e, n - pos, ','), *pfx;
		size_t len = comma ? (size_t)(comma - e) : n - pos, ip_len, pfx_len;
		uint32_t ip, prefix = 32;

		pos += len + 1;
		if (len == 0)
			continue;
		split_cidr(e, len, &ip_len, &pfx, &pfx_len);
		if (find(e, ip_len, ':')) {
			if (!valid_ipv6(e, ip_len) || (pfx && !parse_uint(pfx, pfx_len, 128, &prefix)))
				return fail(c, VTC_E_BAD_ADDRESS, e, len);
			note(c, VTC_N_ADDRESS_IPV6, e, len);
			continue;
		}
		if (!parse_ipv4(e, ip_len, &ip) || (pfx && !parse_uint(pfx, pfx_len, 32, &prefix)))
			return fail(c, VTC_E_BAD_ADDRESS, e, len);
		if (c->have_address) {
			note(c, VTC_N_ADDRESS_EXTRA, e, len);
			continue;
		}
		r->address = ip;
		r->address_prefix = (uint8_t)prefix;
		c->have_address = 1;
	}
	return VTC_OK;
}

/* Setup key DNS: digits and dots are an IPv4 server, anything with ':' an
 * IPv6 server, anything else a search domain. */
static int do_dns(Ctx *c, const char *v, size_t n)
{
	VtcResult *r = c->r;
	size_t pos = 0, i;

	while (pos < n) {
		const char *e = v + pos, *comma = find(e, n - pos, ',');
		size_t len = comma ? (size_t)(comma - e) : n - pos;
		int numeric = 1;
		uint32_t ip;

		pos += len + 1;
		if (len == 0)
			continue;
		for (i = 0; i < len; i++)
			if (!is_digit((unsigned char)e[i]) && e[i] != '.')
				numeric = 0;
		if (numeric) {
			if (!parse_ipv4(e, len, &ip) || ip == 0)
				return fail(c, VTC_E_BAD_DNS, e, len);
			if (r->dns_count < 2)
				r->dns[r->dns_count++] = ip;
			else
				note(c, VTC_N_DNS_EXTRA, e, len);
		} else if (find(e, len, ':')) {
			if (!valid_ipv6(e, len))
				return fail(c, VTC_E_BAD_DNS, e, len);
			note(c, VTC_N_DNS_IPV6, e, len);
		} else {
			note(c, VTC_N_DNS_SEARCH, e, len);
		}
	}
	return VTC_OK;
}

/* Setup key MTU. The Vita's Wi-Fi MTU is 1500, which leaves 1440 for the
 * tunnel over IPv4 (20 IP + 8 UDP + 32 tunnel header and tag). */
#define MTU_MIN 576
#define MTU_MAX 1440

static int do_mtu(Ctx *c, const char *v, size_t n)
{
	uint32_t mtu;

	if (n == 0) {
		c->r->mtu = 0;
		return VTC_OK;
	}
	if (!parse_uint(v, n, 65535, &mtu) || mtu < MTU_MIN)
		return fail(c, VTC_E_BAD_MTU, v, n);
	if (mtu > MTU_MAX) {
		note(c, VTC_N_MTU_CLAMPED, v, n);
		mtu = MTU_MAX;
	}
	c->r->mtu = (uint16_t)mtu;
	return VTC_OK;
}

static int do_table(Ctx *c, const char *v, size_t n)
{
	c->r->table_off = 0;
	if (eq_ci(v, n, "off"))
		c->r->table_off = 1;
	else if (n && !eq_ci(v, n, "auto"))
		note(c, VTC_N_TABLE, v, n);
	return VTC_OK;
}

static int do_saveconfig(Ctx *c, const char *v, size_t n)
{
	if (eq_ci(v, n, "true"))
		note(c, VTC_N_SAVECONFIG, v, n);
	else if (!eq_ci(v, n, "false"))
		return fail(c, VTC_E_BAD_BOOL, v, n);
	return VTC_OK;
}

static int find_prefix(const VtcResult *r, uint32_t net, uint8_t prefix)
{
	unsigned i;

	for (i = 0; i < r->allowed_count; i++)
		if (r->allowed[i].net == net && r->allowed[i].prefix == prefix)
			return (int)i;
	return -1;
}

/* AllowedIPs (reference tools): comma list, every entry ip[/cidr], optionally with a
 * leading '+' (add) or '-' (remove an earlier entry). Empty entries are
 * errors here, unlike Address and DNS. */
static int do_allowed(Ctx *c, const char *v, size_t n)
{
	VtcResult *r = c->r;
	size_t pos = 0;

	for (;;) {
		const char *e = v + pos, *comma = find(e, n - pos, ','), *pfx;
		size_t len = comma ? (size_t)(comma - e) : n - pos, ip_len, pfx_len;
		const char *whole = e;
		size_t whole_len = len;
		uint32_t ip, prefix, net;
		int remove = 0, at;

		pos += len + 1;
		if (len && (e[0] == '+' || e[0] == '-')) {
			remove = e[0] == '-';
			e++;
			len--;
		}
		split_cidr(e, len, &ip_len, &pfx, &pfx_len);
		if (find(e, ip_len, ':')) {
			prefix = 128;
			if (!valid_ipv6(e, ip_len) || (pfx && !parse_uint(pfx, pfx_len, 128, &prefix)))
				return fail(c, VTC_E_BAD_ALLOWED, whole, whole_len);
			note(c, VTC_N_ALLOWED_IPV6, whole, whole_len);
		} else {
			prefix = 32;
			if (!parse_ipv4(e, ip_len, &ip) || (pfx && !parse_uint(pfx, pfx_len, 32, &prefix)))
				return fail(c, VTC_E_BAD_ALLOWED, whole, whole_len);
			net = ip & prefix_mask(prefix);
			if (net != ip)
				note(c, VTC_N_ALLOWED_HOST_BITS, whole, whole_len);
			at = find_prefix(r, net, (uint8_t)prefix);
			if (remove) {
				note(c, VTC_N_ALLOWED_REMOVE, whole, whole_len);
				if (at >= 0) {
					memmove(&r->allowed[at], &r->allowed[at + 1],
						(r->allowed_count - (unsigned)at - 1) * sizeof r->allowed[0]);
					r->allowed_count--;
					memset(&r->allowed[r->allowed_count], 0, sizeof r->allowed[0]);
				}
			} else if (at >= 0) {
				note(c, VTC_N_ALLOWED_DUPLICATE, whole, whole_len);
			} else {
				if (r->allowed_count >= VT_MAX_ALLOWED)
					return fail(c, VTC_E_TOO_MANY_ALLOWED, whole, whole_len);
				r->allowed[r->allowed_count].net = net;
				r->allowed[r->allowed_count].prefix = (uint8_t)prefix;
				r->allowed_count++;
			}
		}
		if (!comma)
			return VTC_OK;
	}
}

/* Host name as the app's resolver will take it: letters, digits, '-', '_',
 * dot-separated labels of 1-63, at most 253 characters plus a final dot. */
static int valid_hostname(const char *s, size_t n)
{
	size_t i, label = 0;

	if (n && s[n - 1] == '.')
		n--;
	if (n == 0 || n > 253)
		return 0;
	for (i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '.') {
			if (label == 0)
				return 0;
			label = 0;
			continue;
		}
		if (!is_alnum(ch) && ch != '-' && ch != '_')
			return 0;
		if (++label > 63)
			return 0;
	}
	return 1;
}

/*
 * Endpoint (reference tools): "[v6]:port" or "host:port", the port after the last ':'.
 * v1 rejects IPv6. The port must be a number 1-65535.
 */
static int do_endpoint(Ctx *c, const char *v, size_t n)
{
	VtcResult *r = c->r;
	const char *host = v, *port;
	size_t host_len, port_len, i;
	uint32_t pnum, ip;
	int numeric = 1;

	if (v[0] == '[') {
		const char *close = find(v, n, ']');
		if (!close || (size_t)(close - v) + 2 >= n || close[1] != ':')
			return fail(c, VTC_E_BAD_ENDPOINT, v, n);
		if (valid_ipv6(v + 1, (size_t)(close - v) - 1))
			return fail(c, VTC_E_ENDPOINT_IPV6, v, n);
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	}
	port = NULL;
	for (i = n; i > 0; i--)
		if (v[i - 1] == ':') {
			port = v + i;
			break;
		}
	if (!port || port == v + n)
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	host_len = (size_t)(port - v) - 1;
	port_len = n - host_len - 1;
	if (host_len == 0)
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	if (find(host, host_len, ':')) {
		if (valid_ipv6(host, host_len))
			return fail(c, VTC_E_ENDPOINT_IPV6, v, n);
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	}
	if (!parse_uint(port, port_len, 65535, &pnum) || pnum == 0)
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	for (i = 0; i < host_len; i++)
		if (!is_digit((unsigned char)host[i]) && host[i] != '.')
			numeric = 0;
	if (parse_ipv4(host, host_len, &ip)) {
		r->endpoint_is_ip = 1;
		r->endpoint_ip = ip;
	} else if (numeric || !valid_hostname(host, host_len)) {
		/* "1.2.3" and friends: resolvers disagree on these, so refuse */
		return fail(c, VTC_E_BAD_ENDPOINT, v, n);
	} else {
		r->endpoint_is_ip = 0;
		r->endpoint_ip = 0;
	}
	memcpy(r->endpoint_host, host, host_len);
	r->endpoint_host[host_len] = '\0';
	r->endpoint_port = (uint16_t)pnum;
	c->have_endpoint = 1;
	return VTC_OK;
}

static int do_keepalive(Ctx *c, const char *v, size_t n)
{
	uint32_t s;

	if (eq_ci(v, n, "off"))
		s = 0;
	else if (!parse_uint(v, n, 65535, &s))
		return fail(c, VTC_E_BAD_KEEPALIVE, v, n);
	c->r->keepalive_s = (uint16_t)s;
	return VTC_OK;
}

static int do_listenport(Ctx *c, const char *v, size_t n)
{
	uint32_t p;

	if (!parse_uint(v, n, 65535, &p))
		return fail(c, VTC_E_BAD_PORT, v, n);
	c->r->listen_port = (uint16_t)p;
	return VTC_OK;
}

static int do_key(Ctx *c, int k, const char *v, size_t n)
{
	VtcResult *r = c->r;
	int ret;

	switch (k) {
	case K_PRIVATEKEY:
		ret = set_key(c, k, v, n, r->private_key);
		c->have_private = ret == VTC_OK;
		return ret;
	case K_LISTENPORT:
		return do_listenport(c, v, n);
	case K_FWMARK:
		return check_fwmark(c, v, n);
	case K_ADDRESS:
		return do_address(c, v, n);
	case K_DNS:
		return do_dns(c, v, n);
	case K_MTU:
		return do_mtu(c, v, n);
	case K_TABLE:
		return do_table(c, v, n);
	case K_PREUP:
	case K_POSTUP:
	case K_PREDOWN:
	case K_POSTDOWN:
		note(c, VTC_N_HOOK, k_keys[k].name, strlen(k_keys[k].name));
		return VTC_OK;
	case K_SAVECONFIG:
		return do_saveconfig(c, v, n);
	case K_PUBLICKEY:
		ret = set_key(c, k, v, n, r->peer_public);
		c->have_public = ret == VTC_OK;
		return ret;
	case K_PRESHAREDKEY:
		ret = set_key(c, k, v, n, r->preshared);
		r->has_preshared = ret == VTC_OK && !key_is_zero(r->preshared);
		return ret;
	case K_ALLOWEDIPS:
		return do_allowed(c, v, n);
	case K_ENDPOINT:
		return do_endpoint(c, v, n);
	default:
		return do_keepalive(c, v, n);
	}
}

static int lookup_key(const char *s, size_t n)
{
	unsigned i;

	for (i = 0; i < N_KEYS; i++)
		if (eq_ci(s, n, k_keys[i].name))
			return (int)i;
	return -1;
}

static int is_fork_key(const char *s, size_t n)
{
	unsigned i;

	for (i = 0; i < sizeof k_fork_keys / sizeof k_fork_keys[0]; i++)
		if (eq_ci(s, n, k_fork_keys[i]))
			return 1;
	return 0;
}

/* One cleaned line (comments and whitespace gone, not empty). overflow:
 * the line was longer than the buffer, which holds its first part. */
static int process_line(Ctx *c, char *s, size_t n, int overflow)
{
	const char *eq;
	size_t klen;
	int k;

	if (s[0] == '[') {
		if (overflow)
			return fail(c, VTC_E_LINE_TOO_LONG, s, 32);
		if (eq_ci(s, n, "[Interface]")) {
			c->section = SEC_INTERFACE;
			return VTC_OK;
		}
		if (eq_ci(s, n, "[Peer]")) {
			if (++c->peers > 1)
				return fail(c, VTC_E_MULTIPLE_PEERS, s, n);
			c->section = SEC_PEER;
			c->peer_line = c->line;
			return VTC_OK;
		}
		return fail(c, VTC_E_BAD_SECTION, s, n);
	}
	eq = find(s, n, '=');
	if (overflow && !eq)
		return fail(c, VTC_E_LINE_TOO_LONG, s, 32);
	klen = eq ? (size_t)(eq - s) : n;
	if (is_fork_key(s, klen))
		return fail(c, VTC_E_FORK_KEY, s, klen);
	if (c->section == SEC_NONE)
		return fail(c, VTC_E_OUTSIDE_SECTION, s, klen);
	if (!eq || klen == 0)
		return fail(c, VTC_E_NO_EQUALS, s, n);
	k = lookup_key(s, klen);
	if (k < 0)
		return fail(c, VTC_E_UNKNOWN_KEY, s, klen);
	if (k_keys[k].section != c->section)
		return fail(c, VTC_E_WRONG_SECTION, s, klen);
	if (overflow) {
		/* Hook scripts can be long and are ignored anyway. */
		if (k == K_PREUP || k == K_POSTUP || k == K_PREDOWN || k == K_POSTDOWN)
			return do_key(c, k, NULL, 0);
		return fail(c, VTC_E_LINE_TOO_LONG, s, klen);
	}
	if (eq + 1 == s + n && !k_keys[k].quick)
		return fail(c, VTC_E_EMPTY_VALUE, s, klen);
	return do_key(c, k, eq + 1, (size_t)(s + n - eq - 1));
}

/* ---- our directive ------------------------------------------------------ */

#define DIRECTIVE "vita-turbo-vpn:"
#define DIRECTIVE_MAX (sizeof DIRECTIVE + 6 + VTC_URL_MAX)

/* Comment text with whitespace removed and leading '#'s skipped. */
static void process_comment(Ctx *c, const char *s, size_t n, int overflow)
{
	VtcResult *r = c->r;
	size_t dl = sizeof DIRECTIVE - 1, i;
	const char *v;
	size_t vn;

	while (n && s[0] == '#') {
		s++;
		n--;
	}
	if (!starts_ci(s, n, DIRECTIVE))
		return;
	s += dl;
	n -= dl;
	if (!starts_ci(s, n, "check=")) {
		note(c, VTC_N_DIRECTIVE_UNKNOWN, s, n);
		return;
	}
	v = s + 6;
	vn = n - 6;
	if (overflow || vn == 0)
		goto bad;
	if (eq_ci(v, vn, "generic")) {
		r->check = VTC_CHECK_GENERIC;
	} else {
		if (!starts_ci(v, vn, "https://") || vn <= 8 || vn >= VTC_URL_MAX)
			goto bad;
		for (i = 0; i < vn; i++)
			if ((unsigned char)v[i] < 0x21 || (unsigned char)v[i] > 0x7e)
				goto bad;
		memcpy(r->check_url, v, vn);
		r->check_url[vn] = '\0';
		r->check = VTC_CHECK_URL;
	}
	if (r->check != VTC_CHECK_URL)
		r->check_url[0] = '\0';
	r->check_line = c->line;
	return;
bad:
	note(c, VTC_N_DIRECTIVE_BAD, v, vn);
}

/* ---- driver ------------------------------------------------------------- */

static void wipe_keys(VtcResult *r)
{
	vtc_wipe(r->private_key, sizeof r->private_key);
	vtc_wipe(r->peer_public, sizeof r->peer_public);
	vtc_wipe(r->preshared, sizeof r->preshared);
}

static int finish(Ctx *c)
{
	VtcResult *r = c->r;

	c->line = 0;
	if (!c->have_private)
		return fail(c, VTC_E_NO_PRIVATE_KEY, NULL, 0);
	if (c->peers == 0)
		return fail(c, VTC_E_NO_PEER, NULL, 0);
	c->line = c->peer_line;
	if (!c->have_public)
		return fail(c, VTC_E_NO_PUBLIC_KEY, NULL, 0);
	if (!c->have_endpoint)
		return fail(c, VTC_E_NO_ENDPOINT, NULL, 0);
	if (r->allowed_count == 0)
		return fail(c, VTC_E_NO_ALLOWED, NULL, 0);
	c->line = 0;
	if (!c->have_address)
		return fail(c, VTC_E_NO_ADDRESS, NULL, 0);
	r->full_tunnel = (uint8_t)vtc_covers_all(r->allowed, r->allowed_count);
	return VTC_OK;
}

static int parse(Ctx *c, const char *text, size_t len, char *line)
{
	char comment[DIRECTIVE_MAX + 1];
	size_t pos = 0;

	if (memchr(text, '\0', len)) {
		/* report the line it is on */
		const char *z = memchr(text, '\0', len);
		const char *p;
		c->line = 1;
		for (p = text; p < z; p++)
			if (*p == '\n')
				c->line++;
		return fail(c, VTC_E_NUL, NULL, 0);
	}
	/* A UTF-8 byte order mark, as some desktop editors write. */
	if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
	    (unsigned char)text[2] == 0xBF)
		pos = 3;

	while (pos < len) {
		const char *s = text + pos, *nl = find(s, len - pos, '\n');
		size_t raw = nl ? (size_t)(nl - s) : len - pos;
		const char *hash = find(s, raw, '#');
		size_t body = hash ? (size_t)(hash - s) : raw, n = 0, i;
		int over = 0, ret;

		pos += raw + 1;
		c->line++;
		for (i = 0; i < body; i++) {
			if (is_space((unsigned char)s[i]))
				continue;
			if (n < VTC_LINE_MAX)
				line[n++] = s[i];
			else
				over = 1;
		}
		line[n] = '\0';
		if (hash) {
			size_t m = 0;
			int cover = 0;
			for (i = body + 1; i < raw; i++) {
				if (is_space((unsigned char)s[i]))
					continue;
				if (m < DIRECTIVE_MAX)
					comment[m++] = s[i];
				else
					cover = 1;
			}
			process_comment(c, comment, m, cover);
		}
		if (n == 0)
			continue;
		ret = process_line(c, line, n, over);
		if (ret != VTC_OK)
			return ret;
	}
	return finish(c);
}

int vtc_parse(const char *text, size_t len, VtcResult *res)
{
	char line[VTC_LINE_MAX + 1];
	Ctx c;
	int ret;

	if (!res)
		return VTC_E_ARG;
	memset(res, 0, sizeof *res);
	if (!text && len) {
		res->error = VTC_E_ARG;
		return VTC_E_ARG;
	}
	memset(&c, 0, sizeof c);
	c.r = res;
	ret = parse(&c, text ? text : "", len, line);
	/* the line buffer last held key text */
	vtc_wipe(line, sizeof line);
	if (ret != VTC_OK)
		wipe_keys(res);
	return ret;
}

/* ---- to VtProfile ------------------------------------------------------- */

int vtc_to_profile(const VtcResult *r, const char *name, VtProfile *p)
{
	size_t n, max = VT_NAME_MAX - 1;

	if (!r || !p || r->error != VTC_OK)
		return -1;
	vtc_wipe(p, sizeof *p);
	p->size = sizeof *p;
	memcpy(p->private_key, r->private_key, VT_KEY_LEN);
	memcpy(p->peer_public, r->peer_public, VT_KEY_LEN);
	if (r->has_preshared)
		memcpy(p->preshared, r->preshared, VT_KEY_LEN);
	p->address = r->address;
	p->address_prefix = r->address_prefix;
	p->allowed_count = r->allowed_count;
	p->mtu = r->mtu;
	memcpy(p->allowed, r->allowed, sizeof p->allowed);
	p->endpoint_ip = r->endpoint_is_ip ? r->endpoint_ip : 0;
	p->endpoint_port = r->endpoint_port;
	p->keepalive_s = r->keepalive_s;
	p->dns[0] = r->dns[0];
	p->dns[1] = r->dns[1];
	p->listen_port = r->listen_port;
	p->flags = r->table_off ? VT_PROFILE_NO_ROUTES : 0;

	if (!name)
		name = "";
	n = strlen(name);
	if (n >= 5 && eq_ci(name + n - 5, 5, ".conf"))
		n -= 5;
	if (n > max) {
		n = max;
		/* do not cut a UTF-8 sequence: back up over continuation bytes */
		while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80)
			n--;
	}
	memcpy(p->name, name, n);
	p->name[n] = '\0';
	return 0;
}
