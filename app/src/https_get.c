/*
 * https_get.c - one HTTPS GET for the exit-IP check.
 *
 * TLS is mbedTLS 3.6 (the VitaSDK's), verifying the server's certificate
 * chain against Mozilla's root bundle embedded in the app (ca_bundle.c,
 * third_party/ca-bundle) plus the user's own roots in
 * ux0:data/vita-turbo-vpn/extra-ca.pem if that file exists, and the
 * host name against the certificate (SNI is sent for it). Entropy comes
 * from the system's random number generator (getentropy, which is
 * sceKernelGetRandomNumber in the VitaSDK's libc).
 *
 * Nothing identifying is sent: the request is a bare
 * HTTP/1.1 GET with Host, a generic User-Agent, Accept and Connection:
 * close. No cookies, no client certificate, and no TLS session is kept or
 * resumed: each check is a fresh connection whose state is freed after it.
 *
 * The socket is a non-blocking sceNet TCP socket; every wait (connect,
 * send, receive) is an epoll wait bounded by one deadline, so a check
 * never takes much longer than its timeout once the name is looked up.
 */
#include <psp2/kernel/processmgr.h>
#include "../../include/vt_paths.h"
#include <psp2/net/net.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "https_get.h"

/* ca_bundle.c: cacert.pem with a NUL after it, as mbedTLS's PEM parser wants */
extern const char vpnturbo_ca_pem[], vpnturbo_ca_pem_end[];

#define EXTRA_CA     "ux0:" VT_PROFILE_DIR "/extra-ca.pem"
#define RAW_MAX      (16 * 1024)

/* ---- the roots, parsed once ---------------------------------------------- */

static mbedtls_x509_crt g_ca;
static int g_ca_state;          /* 0 not yet, 1 parsed, -1 failed */

static int ca_load(void)
{
	if (g_ca_state)
		return g_ca_state > 0 ? 0 : -1;
	mbedtls_x509_crt_init(&g_ca);
	/* a positive return is the number of entries that did not parse (an
	 * unsupported algorithm, say); the rest are usable */
	int r = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)vpnturbo_ca_pem,
		(size_t)(vpnturbo_ca_pem_end - vpnturbo_ca_pem));
	if (r < 0 || g_ca.version == 0) {
		mbedtls_x509_crt_free(&g_ca);
		g_ca_state = -1;
		return -1;
	}
	mbedtls_x509_crt_parse_file(&g_ca, EXTRA_CA);   /* optional; errors ignored */
	g_ca_state = 1;
	return 0;
}

/* ---- the socket ------------------------------------------------------------ */

typedef struct Conn {
	int s, eid;
	uint64_t deadline;          /* process time, us */
} Conn;

static uint64_t now_us(void)
{
	return sceKernelGetProcessTimeWide();
}

static int is_err(int r, unsigned code)
{
	return r < 0 && (unsigned)r == code;
}

/* Wait until the socket is ready for events; 1 ready, 0 deadline, -1 error */
static int wait_io(Conn *c, unsigned events)
{
	uint64_t t = now_us();
	if (t >= c->deadline)
		return 0;
	SceNetEpollEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = events;
	ev.data.fd = c->s;
	if (sceNetEpollControl(c->eid, SCE_NET_EPOLL_CTL_MOD, c->s, &ev) < 0)
		return -1;
	memset(&ev, 0, sizeof(ev));
	uint64_t left = c->deadline - t;
	int r = sceNetEpollWait(c->eid, &ev, 1, (int)(left > 0x7FFFFFFF ? 0x7FFFFFFF : left));
	if (r < 0)
		return -1;
	return r > 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	Conn *c = ctx;
	for (;;) {
		int r = sceNetSend(c->s, buf, (unsigned)len, 0);
		if (r >= 0)
			return r;
		if (!is_err(r, SCE_NET_ERROR_EAGAIN))
			return MBEDTLS_ERR_NET_SEND_FAILED;
		int w = wait_io(c, SCE_NET_EPOLLOUT);
		if (w <= 0)
			return w == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_SEND_FAILED;
	}
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	Conn *c = ctx;
	for (;;) {
		int r = sceNetRecv(c->s, buf, (unsigned)len, 0);
		if (r >= 0)
			return r;
		if (!is_err(r, SCE_NET_ERROR_EAGAIN))
			return MBEDTLS_ERR_NET_RECV_FAILED;
		int w = wait_io(c, SCE_NET_EPOLLIN);
		if (w <= 0)
			return w == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
	}
}

static HgResult tcp_open(Conn *c, uint32_t ip, uint16_t port)
{
	c->s = sceNetSocket("vpnturbo_check", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if (c->s < 0)
		return HG_CONNECT;
	c->eid = sceNetEpollCreate("vpnturbo_check", 0);
	if (c->eid < 0)
		return HG_CONNECT;
	SceNetEpollEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = SCE_NET_EPOLLOUT;
	ev.data.fd = c->s;
	if (sceNetEpollControl(c->eid, SCE_NET_EPOLL_CTL_ADD, c->s, &ev) < 0)
		return HG_CONNECT;
	int one = 1;
	sceNetSetsockopt(c->s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
	sceNetSetsockopt(c->s, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof(one));

	SceNetSockaddrIn sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = SCE_NET_AF_INET;
	sa.sin_port = sceNetHtons(port);
	sa.sin_addr.s_addr = __builtin_bswap32(ip);    /* host to network order */
	int r = sceNetConnect(c->s, (const SceNetSockaddr *)&sa, sizeof(sa));
	if (r < 0 && !is_err(r, SCE_NET_ERROR_EINPROGRESS))
		return HG_CONNECT;
	if (r < 0) {
		int w = wait_io(c, SCE_NET_EPOLLOUT);
		if (w == 0)
			return HG_TIMEOUT;
		if (w < 0)
			return HG_CONNECT;
		int err = 0;
		unsigned n = sizeof(err);
		if (sceNetGetsockopt(c->s, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &err, &n) < 0 || err)
			return HG_CONNECT;
	}
	return HG_OK;
}

static void tcp_close(Conn *c)
{
	if (c->eid >= 0)
		sceNetEpollDestroy(c->eid);
	if (c->s >= 0)
		sceNetSocketClose(c->s);
	c->eid = c->s = -1;
}

/* ---- URL and lookup ---------------------------------------------------- */

typedef struct Url {
	char host[256];
	uint16_t port;
	const char *path;           /* into the URL; "" means "/" */
} Url;

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int prefix_ci(const char *s, const char *p)
{
	for (; *p; s++, p++)
		if (lower((unsigned char)*s) != *p)
			return 0;
	return 1;
}

static int url_parse(const char *url, Url *u)
{
	if (!prefix_ci(url, "https://"))
		return -1;
	const char *h = url + 8, *e = h;
	while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#')
		e++;
	size_t n = (size_t)(e - h);
	if (n == 0 || n >= sizeof(u->host))
		return -1;
	for (const char *p = h; p < e; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      *p == '-' || *p == '.'))
			return -1;        /* no user info, IPv6 literal or odd bytes */
	memcpy(u->host, h, n);
	u->host[n] = 0;
	u->port = 443;
	if (*e == ':') {
		unsigned port = 0;
		for (e++; *e >= '0' && *e <= '9'; e++)
			if ((port = port * 10 + (unsigned)(*e - '0')) > 65535)
				return -1;
		if (!port)
			return -1;
		u->port = (uint16_t)port;
	}
	if (*e && *e != '/' && *e != '?' && *e != '#')
		return -1;
	u->path = *e == '#' ? "" : e;
	for (const char *p = u->path; *p; p++)
		if ((unsigned char)*p <= ' ' || *p == 0x7F)
			return -1;
	return 0;
}

static uint32_t dotted(const char *s)
{
	unsigned a, b, c, d;
	char extra;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
		return 0;
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

int hg_lookup_err;

static int lookup(const char *host, uint32_t *ip)
{
	hg_lookup_err = 0;
	if ((*ip = dotted(host)) != 0)
		return 0;
	SceNetInAddr addr;
	memset(&addr, 0, sizeof(addr));
	int rid = sceNetResolverCreate("vpnturbo_check", NULL, 0);
	if (rid < 0) {
		hg_lookup_err = rid;
		return -1;
	}
	int r = sceNetResolverStartNtoa(rid, host, &addr, 0, 0, 0);
	sceNetResolverDestroy(rid);
	if (r < 0 || addr.s_addr == 0) {
		hg_lookup_err = r < 0 ? r : -1;   /* -1: no error but no address */
		return -1;
	}
	*ip = __builtin_bswap32(addr.s_addr);
	return 0;
}

/* ---- the HTTP answer ------------------------------------------------------- */

/* The value of header name (lower case, with the colon) in the header block */
static const char *header(const char *hdr, const char *name)
{
	for (const char *l = hdr; l && *l;) {
		if (prefix_ci(l, name)) {
			l += strlen(name);
			while (*l == ' ' || *l == '\t')
				l++;
			return l;
		}
		l = strstr(l, "\r\n");
		if (l)
			l += 2;
	}
	return NULL;
}

/* Dechunk body (len bytes) in place; the new length, or -1 */
static long dechunk(char *body, size_t len)
{
	size_t in = 0, out = 0;
	for (;;) {
		unsigned long n = 0;
		int digits = 0;
		while (in < len) {
			int c = lower((unsigned char)body[in]);
			int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
			if (v < 0)
				break;
			if (n > 0xFFFFFF)
				return -1;
			n = n * 16 + (unsigned long)v;
			digits++;
			in++;
		}
		if (!digits)
			return -1;
		while (in < len && body[in] != '\n')   /* chunk extensions, CR */
			in++;
		if (in >= len)
			return n == 0 ? (long)out : -1;
		in++;
		if (n == 0)
			return (long)out;
		if (n > len - in)
			n = len - in;   /* cut short: keep what came */
		memmove(body + out, body + in, n);
		out += n;
		in += n;
		if (in + 2 <= len && body[in] == '\r' && body[in + 1] == '\n')
			in += 2;
		else if (in < len && body[in] == '\n')
			in++;
		if (in >= len)
			return (long)out;
	}
}

/* Whether raw (len bytes, NUL after) holds the whole answer already, so
 * a server that keeps the connection open is not waited for */
static int http_complete(const char *raw, size_t len)
{
	const char *end = strstr(raw, "\r\n\r\n");
	if (!end)
		return 0;
	size_t blen = len - (size_t)(end + 4 - raw);
	/* the header lookups stop at the blank line: copy the header block */
	char hdr[2048];
	size_t hn = (size_t)(end - raw);
	if (hn >= sizeof(hdr))
		return 0;
	memcpy(hdr, raw, hn);
	hdr[hn] = 0;
	const char *te = header(hdr, "transfer-encoding:");
	if (te && prefix_ci(te, "chunked"))
		return blen >= 5 && memcmp(raw + len - 5, "0\r\n\r\n", 5) == 0;
	const char *cl = header(hdr, "content-length:");
	unsigned long want;
	return cl && sscanf(cl, "%lu", &want) == 1 && blen >= want;
}

/* raw holds the whole answer (len bytes, NUL after); body out */
static HgResult http_parse(char *raw, size_t len, char *body, size_t cap)
{
	if (len < 12 || !prefix_ci(raw, "http/1.") || raw[8] != ' ')
		return HG_HTTP;
	if (strncmp(raw + 9, "200", 3) != 0)
		return HG_HTTP;
	char *end = strstr(raw, "\r\n\r\n");
	if (!end)
		return HG_HTTP;
	*end = 0;
	char *b = end + 4;
	size_t blen = len - (size_t)(b - raw);
	const char *te = header(raw, "transfer-encoding:");
	if (te && prefix_ci(te, "chunked")) {
		long n = dechunk(b, blen);
		if (n < 0)
			return HG_HTTP;
		blen = (size_t)n;
	} else {
		const char *cl = header(raw, "content-length:");
		unsigned long want;
		if (cl && sscanf(cl, "%lu", &want) == 1 && want < blen)
			blen = want;
	}
	if (blen >= cap)
		blen = cap - 1;
	memcpy(body, b, blen);
	body[blen] = 0;
	return HG_OK;
}

/* ---- the GET ------------------------------------------------------------- */

static int g_psa_ok;

HgResult hg_get(const char *url, char *body, size_t cap, uint32_t *peer_ip, int timeout_ms)
{
	static char raw[RAW_MAX + 1];
	static char req[512];
	Url u;
	uint32_t ip = 0;
	HgResult res = HG_TLS;

	*peer_ip = 0;
	if (cap)
		body[0] = 0;
	if (!cap || url_parse(url, &u) != 0)
		return HG_BAD_URL;
	char port[8] = "";
	if (u.port != 443)
		snprintf(port, sizeof(port), ":%u", u.port);
	int qn = snprintf(req, sizeof(req),
		"GET %s%s HTTP/1.1\r\n"
		"Host: %s%s\r\n"
		"User-Agent: Mozilla/5.0\r\n"
		"Accept: application/json\r\n"
		"Connection: close\r\n"
		"\r\n",
		u.path[0] == '/' ? "" : "/", u.path, u.host, port);
	if (qn <= 0 || (size_t)qn >= sizeof(req))
		return HG_BAD_URL;

	if (ca_load() != 0)
		return HG_TLS;
	if (!g_psa_ok) {
		/* TLS 1.3 in mbedTLS 3.6 runs on PSA crypto */
		if (psa_crypto_init() != PSA_SUCCESS)
			return HG_TLS;
		g_psa_ok = 1;
	}

	uint64_t start = now_us();
	if (lookup(u.host, &ip) != 0)
		return HG_RESOLVE;
	*peer_ip = ip;

	/* the lookup's own timeout is the system's; the rest gets what is
	 * left of timeout_ms, and at least 3 s */
	Conn c = { -1, -1, 0 };
	uint64_t budget = (uint64_t)timeout_ms * 1000, used = now_us() - start;
	c.deadline = now_us() + (used + 3000000 > budget ? 3000000 : budget - used);

	mbedtls_entropy_context ent;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_ssl_config conf;
	mbedtls_ssl_context ssl;
	mbedtls_entropy_init(&ent);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_ssl_config_init(&conf);
	mbedtls_ssl_init(&ssl);

	static const unsigned char pers[] = "vpnturbo_app exit check";
	if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, pers, sizeof(pers) - 1) != 0)
		goto out;
	if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
	                                MBEDTLS_SSL_PRESET_DEFAULT) != 0)
		goto out;
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
	mbedtls_ssl_conf_session_tickets(&conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
	if (mbedtls_ssl_setup(&ssl, &conf) != 0)
		goto out;
	/* SNI, and the name the certificate must carry */
	if (mbedtls_ssl_set_hostname(&ssl, u.host) != 0)
		goto out;

	res = tcp_open(&c, ip, u.port);
	if (res != HG_OK)
		goto out;
	mbedtls_ssl_set_bio(&ssl, &c, bio_send, bio_recv, NULL);

	res = HG_TLS;
	int r;
	while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r == MBEDTLS_ERR_SSL_TIMEOUT) {
			res = HG_TIMEOUT;
		} else if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
			if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
				res = HG_CERT_TIME;
		} else if (r == MBEDTLS_ERR_NET_SEND_FAILED || r == MBEDTLS_ERR_NET_RECV_FAILED) {
			res = HG_CONNECT;
		}
		goto out;
	}

	size_t off = 0;
	while (off < (size_t)qn) {
		r = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off, (size_t)qn - off);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0) {
			res = r == MBEDTLS_ERR_SSL_TIMEOUT ? HG_TIMEOUT : HG_CONNECT;
			goto out;
		}
		off += (size_t)r;
	}

	/* read until the server closes (Connection: close) or the buffer is
	 * full; a close without close_notify ends the answer too */
	size_t len = 0;
	int timed_out = 0;
	while (len < RAW_MAX) {
		r = mbedtls_ssl_read(&ssl, (unsigned char *)raw + len, RAW_MAX - len);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
		if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;         /* offered, never kept */
#endif
		if (r <= 0) {
			timed_out = r == MBEDTLS_ERR_SSL_TIMEOUT;
			break;
		}
		len += (size_t)r;
		raw[len] = 0;
		if (http_complete(raw, len))
			break;
	}
	raw[len] = 0;
	res = http_parse(raw, len, body, cap);
	if (res != HG_OK && timed_out)
		res = HG_TIMEOUT;
	if (res == HG_OK)
		mbedtls_ssl_close_notify(&ssl);

out:
	tcp_close(&c);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&ent);
	mbedtls_platform_zeroize(raw, sizeof(raw));
	return res;
}

