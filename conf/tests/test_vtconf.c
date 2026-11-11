/*
 * test_vtconf.c - host unit tests for the profile parser.
 *
 * Real-world layouts, every accepted form and every rejection, the
 * duplicate-check and naming helpers, and a mutation fuzz pass. Built and
 * run by "make test" (with the address and undefined-behaviour sanitisers).
 */
#include "../vtconf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests, g_checks, g_failed;
static const char *g_test;

#define CHECK(cond) do { \
	g_checks++; \
	if (!(cond)) { \
		g_failed++; \
		printf("FAIL %s (%s:%d): %s\n", g_test, __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define TEST(name) static void name(void)
#define RUN(name) do { g_test = #name; g_tests++; name(); } while (0)

/* Test keys (random, used nowhere else). */
#define K1 "/7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmpHI="
#define K1_HEX "ffb9925dd82898e6615e02a53258efa3d99ca70b39611a3eef042f4265e6a472"
#define K2 "xsF58TifdHJ/p5jRY0cifHxlshf1MwEGCyf9w8I6CuM="
#define K2_HEX "c6c179f1389f74727fa798d16347227c7c65b217f53301060b27fdc3c23a0ae3"
#define K3 "F2ALGBuOU0U2M28nyw5hMoMwV9zpufskwzx3m0gdZqE="
#define K3_HEX "17600b181b8e534536336f27cb0e6132833057dce9b9fb24c33c779b481d66a1"
#define K4 "9CDWvOTd2av9zhGr09nsJ/cahoj/ktiBX9/wL0bb7vE="
#define KZERO "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

#define IFACE "[Interface]\nPrivateKey = " K1 "\nAddress = 10.0.0.2/32\n"
#define PEER_NOALLOWED "[Peer]\nPublicKey = " K2 "\nEndpoint = 192.0.2.1:51820\n"
#define PEER PEER_NOALLOWED "AllowedIPs = 0.0.0.0/0\n"
#define MINIMAL IFACE PEER

static VtcResult r;

static int P(const char *s)
{
	return vtc_parse(s, strlen(s), &r);
}

static int hex_eq(const uint8_t *b, const char *hex)
{
	char buf[65];
	int i;

	for (i = 0; i < 32; i++)
		snprintf(buf + 2 * i, 3, "%02x", b[i]);
	return strcmp(buf, hex) == 0;
}

static int all_zero(const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n--)
		if (*b++)
			return 0;
	return 1;
}

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d;
}

/* Expects an error with this code, line and (if non-NULL) item. */
static void expect_line(const char *text, size_t len, int code, uint32_t line, const char *item, int src)
{
	int ret = vtc_parse(text, len, &r);

	g_checks++;
	if (ret != code || r.error != code || r.error_line != line ||
	    (item && strcmp(r.error_item, item) != 0)) {
		char msg[256];
		g_failed++;
		vtc_format_error(&r, msg, sizeof msg);
		printf("FAIL %s (line %d): want %d at %u `%s', got %d at %u `%s' (%s)\n",
		       g_test, src, code, line, item ? item : "*", ret, r.error_line,
		       r.error_item, msg);
	}
	/* a failed parse never leaves keys behind */
	if (code != VTC_OK) {
		CHECK(all_zero(r.private_key, 32));
		CHECK(all_zero(r.peer_public, 32));
		CHECK(all_zero(r.preshared, 32));
	}
}

#define EXPECT(text, code, line, item) expect_line(text, strlen(text), code, line, item, __LINE__)
#define OK(text) EXPECT(text, VTC_OK, 0, "")

static int has_note(int code, const char *item)
{
	unsigned i;

	for (i = 0; i < r.note_count; i++)
		if (r.notes[i].code == code && (!item || strcmp(r.notes[i].item, item) == 0))
			return 1;
	return 0;
}

/* ---- real-world layouts ------------------------------------------------- */

TEST(t_dual_stack_layout)
{
	/* A dual-stack file: a comment line after [Interface], IPv4 and IPv6 addresses. */
	VtProfile p;

	OK("[Interface]\n"
	   "# Example device\n"
	   "PrivateKey = " K1 "\n"
	   "Address = 192.0.2.21/32,2001:db8::21/128\n"
	   "DNS = 192.0.2.1\n"
	   "\n"
	   "[Peer]\n"
	   "PublicKey = " K2 "\n"
	   "AllowedIPs = 0.0.0.0/0,::0/0\n"
	   "Endpoint = 203.0.113.68:51820\n");
	CHECK(hex_eq(r.private_key, K1_HEX));
	CHECK(hex_eq(r.peer_public, K2_HEX));
	CHECK(!r.has_preshared);
	CHECK(r.address == ip(192, 0, 2, 21) && r.address_prefix == 32);
	CHECK(r.dns_count == 1 && r.dns[0] == ip(192, 0, 2, 1));
	CHECK(r.allowed_count == 1 && r.allowed[0].net == 0 && r.allowed[0].prefix == 0);
	CHECK(r.full_tunnel);
	CHECK(r.endpoint_is_ip && r.endpoint_ip == ip(203, 0, 113, 68));
	CHECK(strcmp(r.endpoint_host, "203.0.113.68") == 0 && r.endpoint_port == 51820);
	CHECK(r.note_count == 2);
	CHECK(has_note(VTC_N_ADDRESS_IPV6, "2001:db8::21/128"));
	CHECK(has_note(VTC_N_ALLOWED_IPV6, "::0/0"));
	CHECK(r.notes[0].line == 4 && r.notes[1].line == 9);
	CHECK(r.check == VTC_CHECK_NONE);

	CHECK(vtc_to_profile(&r, "example-001.conf", &p) == 0);
	CHECK(p.size == sizeof p && p.size == 416);
	CHECK(memcmp(p.private_key, r.private_key, 32) == 0);
	CHECK(all_zero(p.preshared, 32));
	CHECK(p.endpoint_ip == ip(203, 0, 113, 68) && p.endpoint_port == 51820);
	CHECK(p.allowed_count == 1 && p.mtu == 0 && p.keepalive_s == 0 && p.flags == 0);
	CHECK(p.dns[0] == ip(192, 0, 2, 1) && p.dns[1] == 0);
	CHECK(strcmp(p.name, "example-001") == 0);
}

TEST(t_home_server_style)
{
	VtProfile p;

	OK("[Interface]\n"
	   "PrivateKey = " K1 "\n"
	   "Address = 10.6.0.2/24,fd11:5ee:bad:c0de::2/64\n"
	   "DNS = 9.9.9.9, 149.112.112.112\n"
	   "\n"
	   "[Peer]\n"
	   "PublicKey = " K2 "\n"
	   "PresharedKey = " K3 "\n"
	   "Endpoint = myhome.example.org:51820\n"
	   "AllowedIPs = 0.0.0.0/0, ::0/0\n");
	CHECK(r.address == ip(10, 6, 0, 2) && r.address_prefix == 24);
	CHECK(r.dns_count == 2 && r.dns[1] == ip(149, 112, 112, 112));
	CHECK(r.has_preshared && hex_eq(r.preshared, K3_HEX));
	CHECK(!r.endpoint_is_ip && r.endpoint_ip == 0);
	CHECK(strcmp(r.endpoint_host, "myhome.example.org") == 0);
	CHECK(vtc_to_profile(&r, "home.conf", &p) == 0);
	CHECK(p.endpoint_ip == 0 && hex_eq(p.preshared, K3_HEX));
}

TEST(t_web_ui_style)
{
	OK("[Interface]\n"
	   "PrivateKey = " K1 "\n"
	   "Address = 10.8.0.2/24\n"
	   "DNS = 1.1.1.1\n"
	   "MTU = 1420\n"
	   "\n"
	   "[Peer]\n"
	   "PublicKey = " K2 "\n"
	   "PresharedKey = " K3 "\n"
	   "AllowedIPs = 0.0.0.0/0, ::/0\n"
	   "PersistentKeepalive = 0\n"
	   "Endpoint = vpn.example.com:51820\n");
	CHECK(r.mtu == 1420 && r.keepalive_s == 0 && r.full_tunnel);
	CHECK(r.note_count == 1 && has_note(VTC_N_ALLOWED_IPV6, "::/0"));
}

TEST(t_nas_style)
{
	/* A NAS's peer file: no spaces, comment headers, split tunnel to the LAN. */
	VtProfile p;

	OK("[Interface]\n"
	   "#Vita\n"
	   "PrivateKey=" K1 "\n"
	   "Address=10.253.0.2\n"
	   "ListenPort=51820\n"
	   "DNS=192.168.1.1\n"
	   "\n"
	   "[Peer]\n"
	   "#Tower\n"
	   "PublicKey=" K2 "\n"
	   "PresharedKey=" K3 "\n"
	   "Endpoint=home.example.net:51820\n"
	   "AllowedIPs=10.253.0.1/32, 192.168.1.0/24\n"
	   "PersistentKeepalive=25\n");
	CHECK(r.address == ip(10, 253, 0, 2) && r.address_prefix == 32);
	CHECK(r.listen_port == 51820 && r.keepalive_s == 25);
	CHECK(r.allowed_count == 2 && !r.full_tunnel);
	CHECK(r.allowed[1].net == ip(192, 168, 1, 0) && r.allowed[1].prefix == 24);
	CHECK(r.note_count == 0);
	CHECK(vtc_to_profile(&r, "tower.conf", &p) == 0);
	CHECK(p.listen_port == 51820);
}

TEST(t_router_style)
{
	/* Server-side extras a router or self-hosted setup exports. */
	OK("[Interface]\n"
	   "PrivateKey = " K1 "\n"
	   "Address = 10.0.0.2/24\n"
	   "ListenPort = 0\n"
	   "FwMark = 0x1234\n"
	   "Table = 1234\n"
	   "PreUp = echo up\n"
	   "PostUp = fw add forward %i accept; fw add nat %i masquerade\n"
	   "PreDown = echo down\n"
	   "PostDown = fw del forward %i accept\n"
	   "SaveConfig = true\n"
	   "[Peer]\n"
	   "PublicKey = " K2 "\n"
	   "Endpoint = 198.51.100.7:443\n"
	   "AllowedIPs = 10.0.0.0/24\n");
	CHECK(r.listen_port == 0 && !r.table_off && !r.full_tunnel);
	CHECK(has_note(VTC_N_FWMARK, "0x1234"));
	CHECK(has_note(VTC_N_TABLE, "1234"));
	CHECK(has_note(VTC_N_HOOK, "PreUp") && has_note(VTC_N_HOOK, "PostUp"));
	CHECK(has_note(VTC_N_HOOK, "PreDown") && has_note(VTC_N_HOOK, "PostDown"));
	CHECK(has_note(VTC_N_SAVECONFIG, "true"));
	CHECK(r.note_count == 7);
}

/* The "exclude private addresses" list the official clients generate. */
#define EXCLUDE_PRIVATE \
	"0.0.0.0/5, 8.0.0.0/7, 11.0.0.0/8, 12.0.0.0/6, 16.0.0.0/4, 32.0.0.0/3, " \
	"64.0.0.0/2, 128.0.0.0/3, 160.0.0.0/5, 168.0.0.0/6, 172.0.0.0/12, " \
	"172.32.0.0/11, 172.64.0.0/10, 172.128.0.0/9, 173.0.0.0/8, 174.0.0.0/7, " \
	"176.0.0.0/4, 192.0.0.0/9, 192.128.0.0/11, 192.160.0.0/13, 192.169.0.0/16, " \
	"192.170.0.0/15, 192.172.0.0/14, 192.176.0.0/12, 192.192.0.0/10, " \
	"193.0.0.0/8, 194.0.0.0/7, 196.0.0.0/6, 200.0.0.0/5, 208.0.0.0/4"

TEST(t_exclude_private)
{
	VtProfile p;
	unsigned idx = 99;

	OK(IFACE "DNS = 10.11.0.1\n" PEER_NOALLOWED
	   "AllowedIPs = " EXCLUDE_PRIVATE ", 10.11.0.1/32, ::/0\n");
	CHECK(r.allowed_count == 31 && !r.full_tunnel);
	CHECK(vtc_to_profile(&r, "x", &p) == 0);
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, &idx) == VTC_CLASH_NONE && idx == 99);
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 0, 0), 24, &idx) == VTC_CLASH_NONE);
	CHECK(vtc_subnet_clash(&p, ip(172, 16, 5, 0), 24, NULL) == VTC_CLASH_NONE);
	/* a Wi-Fi network on a public range hides part of a tunnel prefix */
	CHECK(vtc_subnet_clash(&p, ip(100, 64, 0, 0), 10, &idx) == VTC_CLASH_SHADOWED && idx == 6);
	/* the tunnel's own DNS /32 inside the Wi-Fi subnet */
	CHECK(vtc_subnet_clash(&p, ip(10, 11, 0, 0), 16, &idx) == VTC_CLASH_INSIDE && idx == 30);
}

TEST(t_crlf_bom_file)
{
	/* CRLF line endings and a UTF-8 byte order mark, as some desktop editors save. */
	OK("\xEF\xBB\xBF[Interface]\r\nPrivateKey = " K1 "\r\nAddress = 10.0.0.2/32\r\n\r\n"
	   "[Peer]\r\nPublicKey = " K2 "\r\nEndpoint = vpn.example.com:51820\r\nAllowedIPs = 0.0.0.0/0\r\n");
	CHECK(strcmp(r.endpoint_host, "vpn.example.com") == 0 && r.full_tunnel);
	/* half a BOM is not one */
	EXPECT("\xEF\xBB[Interface]\n", VTC_E_OUTSIDE_SECTION, 1, "??[Interface]");
	EXPECT("\xEF[Interface]\n", VTC_E_OUTSIDE_SECTION, 1, "?[Interface]");
	/* a BOM anywhere else is not whitespace */
	EXPECT("[Interface]\n\xEF\xBB\xBFPrivateKey = " K1 "\n", VTC_E_UNKNOWN_KEY, 2, "???PrivateKey");
	/* CR-only endings: one long line, as the standard tools accept */
	EXPECT("[Interface]\rPrivateKey = " K1 "\r", VTC_E_BAD_SECTION, 1, NULL);
}

TEST(t_whitespace_and_case)
{
	OK("  [ interface ]  \n"
	   "\tprivate key\t=\t/7mSXdgomOZhXgKlMljvo9mc pws5YRo+7wQvQmXmpHI=   \n"
	   "ADDRESS=10.0.0.2 / 24\n"
	   "dns = 1.1.1.1 , 1.0.0.1\n"
	   "\v\f\n"
	   "[PEER]\n"
	   "Public Key = " K2 "\n"
	   "Allowed IPs = 10.0.0.0 / 8 , 192.168.0.0/16\n"
	   "end point = vpn.Example.COM : 51820\n"
	   "Persistent Keepalive = OFF\n");
	CHECK(hex_eq(r.private_key, K1_HEX));
	CHECK(r.address_prefix == 24 && r.dns_count == 2 && r.allowed_count == 2);
	CHECK(strcmp(r.endpoint_host, "vpn.Example.COM") == 0 && r.keepalive_s == 0);
}

TEST(t_repeats)
{
	OK("[Interface]\n"
	   "PrivateKey = " K3 "\n"
	   "PrivateKey = " K1 "\n"     /* last one wins, as in the reference tools */
	   "Address = 10.0.0.2/32\n"
	   "Address = 10.0.0.3/32, fd00::2\n"
	   "DNS = 1.1.1.1\n"
	   "DNS = 8.8.8.8, 9.9.9.9\n"
	   "[Interface]\n"              /* re-entering is allowed */
	   "MTU = 1380\n"
	   "[Peer]\n"
	   "PublicKey = " K2 "\n"
	   "Endpoint = a.example:1\n"
	   "Endpoint = b.example:2\n"
	   "AllowedIPs = 10.0.0.0/24\n"
	   "AllowedIPs = 10.1.0.0/24, 10.2.0.0/24\n"
	   "AllowedIPs = 10.0.0.0/24\n");
	CHECK(hex_eq(r.private_key, K1_HEX));
	CHECK(r.address == ip(10, 0, 0, 2));
	CHECK(has_note(VTC_N_ADDRESS_EXTRA, "10.0.0.3/32"));
	CHECK(has_note(VTC_N_ADDRESS_IPV6, "fd00::2"));
	CHECK(r.dns_count == 2 && r.dns[1] == ip(8, 8, 8, 8));
	CHECK(has_note(VTC_N_DNS_EXTRA, "9.9.9.9"));
	CHECK(r.mtu == 1380);
	CHECK(strcmp(r.endpoint_host, "b.example") == 0 && r.endpoint_port == 2);
	CHECK(r.allowed_count == 3);
	CHECK(has_note(VTC_N_ALLOWED_DUPLICATE, "10.0.0.0/24"));
}

TEST(t_comments)
{
	OK("# whole-line comment\n"
	   "[Interface] # trailing\n"
	   "PrivateKey = " K1 " # the key\n"
	   "#Address = 10.9.9.9/32\n"
	   "Address = 10.0.0.2/32#no space\n"
	   "   # indented\n"
	   "#\n"
	   PEER);
	CHECK(r.address == ip(10, 0, 0, 2));
	/* a '#' inside a value ends it */
	EXPECT(IFACE "[Peer]\nPublicKey = " K2 "\nEndpoint = host#1:51820\nAllowedIPs=0.0.0.0/0\n",
	       VTC_E_BAD_ENDPOINT, 6, "host");
}

TEST(t_no_key_in_messages)
{
	/* a key on a line of its own, and before [Interface]: the message must
	 * not quote it */
	static const char *const texts[] = { K1 "\n" MINIMAL, IFACE K1 "\n" PEER, K1 };
	for (unsigned i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		static VtcResult kr;
		char buf[256];
		CHECK(vtc_parse(texts[i], strlen(texts[i]), &kr) != VTC_OK);
		vtc_format_error(&kr, buf, sizeof(buf));
		CHECK(strstr(buf, "/7mSXdgomOZhXgKl") == NULL);
		CHECK(strstr(kr.error_item, "/7mSXdgomOZhXgKl") == NULL);
	}
}

TEST(t_directive)
{
	OK(MINIMAL "# vita-turbo-vpn: check = generic\n");
	CHECK(r.check == VTC_CHECK_GENERIC && r.check_line == 8 && r.check_url[0] == 0);
	OK("#VITA-TURBO-VPN:CHECK=GENERIC\n" MINIMAL);
	CHECK(r.check == VTC_CHECK_GENERIC && r.check_line == 1);
	OK(IFACE "## vita-turbo-vpn: check = https://ip.example.org/json?x=1#frag\n" PEER);
	CHECK(r.check == VTC_CHECK_URL && strcmp(r.check_url, "https://ip.example.org/json?x=1#frag") == 0);
	/* on a key line, after the value */
	OK("[Interface]\nPrivateKey = " K1 " # vita-turbo-vpn: check = generic\n"
	   "Address = 10.0.0.2/32\n" PEER);
	CHECK(r.check == VTC_CHECK_GENERIC && r.check_line == 2);
	/* last one wins */
	OK("# vita-turbo-vpn: check = https://a.example/\n" MINIMAL
	   "# vita-turbo-vpn: check = generic\n");
	CHECK(r.check == VTC_CHECK_GENERIC && r.check_url[0] == 0);
	/* provider names are not presets: an unknown value is a note */
	OK(MINIMAL "# vita-turbo-vpn: check = someprovider\n");
	CHECK(r.check == VTC_CHECK_NONE && has_note(VTC_N_DIRECTIVE_BAD, "someprovider"));
	/* bad values and unknown settings are notes, never errors */
	OK(MINIMAL "# vita-turbo-vpn: check = http://ip.example.org/\n");
	CHECK(r.check == VTC_CHECK_NONE && has_note(VTC_N_DIRECTIVE_BAD, "http://ip.example.org/"));
	OK(MINIMAL "# vita-turbo-vpn: check =\n");
	CHECK(has_note(VTC_N_DIRECTIVE_BAD, ""));
	OK(MINIMAL "# vita-turbo-vpn: check = https://\n");
	CHECK(has_note(VTC_N_DIRECTIVE_BAD, "https://"));
	OK(MINIMAL "# vita-turbo-vpn: check = https://a.example/\x01\n");
	CHECK(r.check == VTC_CHECK_NONE && has_note(VTC_N_DIRECTIVE_BAD, NULL));
	OK(MINIMAL "# vita-turbo-vpn: nickname = Home\n");
	CHECK(has_note(VTC_N_DIRECTIVE_UNKNOWN, "nickname=Home"));
	OK(MINIMAL "# vita-turbo-vpn is great\n# other: check = generic\n");
	CHECK(r.note_count == 0 && r.check == VTC_CHECK_NONE);
	{
		/* a URL too long for check_url, and one far longer than any buffer */
		char text[4096];
		char url[VTC_URL_MAX + 1];
		memset(url, 'a', sizeof url - 1);
		memcpy(url, "https://", 8);
		url[sizeof url - 1] = '\0';
		snprintf(text, sizeof text, MINIMAL "# vita-turbo-vpn: check = %s\n", url);
		OK(text);
		CHECK(r.check == VTC_CHECK_NONE && has_note(VTC_N_DIRECTIVE_BAD, NULL));
		url[VTC_URL_MAX - 1] = '\0'; /* 159 characters: fits */
		snprintf(text, sizeof text, MINIMAL "# vita-turbo-vpn: check = %s\n", url);
		OK(text);
		CHECK(r.check == VTC_CHECK_URL && strlen(r.check_url) == VTC_URL_MAX - 1);
		snprintf(text, sizeof text, MINIMAL "# vita-turbo-vpn: check = https://%03000d\n", 0);
		OK(text);
		CHECK(r.check == VTC_CHECK_NONE && has_note(VTC_N_DIRECTIVE_BAD, NULL));
	}
}

/* ---- rejections --------------------------------------------------------- */

TEST(t_missing)
{
	EXPECT("", VTC_E_NO_PRIVATE_KEY, 0, "");
	EXPECT("\n\n# nothing\n", VTC_E_NO_PRIVATE_KEY, 0, "");
	EXPECT("[Interface]\nAddress = 10.0.0.2/32\n" PEER, VTC_E_NO_PRIVATE_KEY, 0, "");
	EXPECT(IFACE, VTC_E_NO_PEER, 0, "");
	EXPECT(IFACE "\n[Peer]\nEndpoint = 192.0.2.1:51820\nAllowedIPs = 0.0.0.0/0\n",
	       VTC_E_NO_PUBLIC_KEY, 5, "");
	EXPECT(IFACE "[Peer]\nPublicKey = " K2 "\nAllowedIPs = 0.0.0.0/0\n", VTC_E_NO_ENDPOINT, 4, "");
	EXPECT(IFACE PEER_NOALLOWED, VTC_E_NO_ALLOWED, 4, "");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ::/0\n", VTC_E_NO_ALLOWED, 4, "");
	EXPECT("[Interface]\nPrivateKey = " K1 "\n" PEER, VTC_E_NO_ADDRESS, 0, "");
	EXPECT("[Interface]\nPrivateKey = " K1 "\nAddress = fd00::2/64\n" PEER, VTC_E_NO_ADDRESS, 0, "");
	EXPECT("[Interface]\nPrivateKey = " K1 "\nAddress =\n" PEER, VTC_E_NO_ADDRESS, 0, "");
}

TEST(t_multiple_peers)
{
	EXPECT(MINIMAL "\n[Peer]\nPublicKey = " K3 "\n", VTC_E_MULTIPLE_PEERS, 9, "[Peer]");
	EXPECT(IFACE "[peer]\n[PEER]\n", VTC_E_MULTIPLE_PEERS, 5, "[PEER]");
}

TEST(t_bad_keys)
{
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmpH=\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* 43 characters */
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmpHIA=\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* 45 */
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmpHIA\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* no padding */
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmpHJ=\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* spare bits set */
	EXPECT("[Interface]\nPrivateKey = _7mSXdgomOZhXgKlMljvo9mcpws5YRo-7wQvQmXmpHI=\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* URL-safe alphabet */
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mcpws5YRo+7wQvQmXmp==\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* '=' too early */
	EXPECT("[Interface]\nPrivateKey = /7mSXdgomOZhXgKlMljvo9mc\xc3\xa9ws5YRo+7wQvQmXmpH=\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* non-ASCII */
	EXPECT("[Interface]\nPrivateKey = ffb9925dd82898e6615e02a53258efa3d99ca70b39611a3eef042f4265e6a472\n",
	       VTC_E_BAD_KEY, 2, "PrivateKey");                         /* hex is not accepted */
	EXPECT(IFACE "[Peer]\nPublicKey = " K2 "x\n", VTC_E_BAD_KEY, 5, "PublicKey");
	EXPECT(IFACE "[Peer]\nPresharedKey = abc=\n", VTC_E_BAD_KEY, 5, "PresharedKey");
	EXPECT("[Interface]\nPrivateKey = " KZERO "\n", VTC_E_ZERO_KEY, 2, "PrivateKey");
	EXPECT(IFACE "[Peer]\nPublicKey = " KZERO "\n", VTC_E_ZERO_KEY, 5, "PublicKey");
	/* an all-zero preshared key means none, as in the kernel structure */
	OK(MINIMAL "PresharedKey = " KZERO "\n");
	CHECK(!r.has_preshared);
	/* every one of the 64 characters decodes */
	OK("[Interface]\nPrivateKey = ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnop0=\nAddress=10.0.0.2\n"
	   "[Peer]\nPublicKey = rstuvwxyz0123456789+/AAAAAAAAAAAAAAAAAAAAAA=\nEndpoint=h:1\nAllowedIPs=0.0.0.0/0\n");
	CHECK(r.private_key[0] == 0x00 && r.private_key[1] == 0x10 && r.private_key[2] == 0x83);
	CHECK(r.peer_public[0] == 0xae && r.peer_public[14] == 0x7e && r.peer_public[15] == 0xfc && r.peer_public[16] == 0x00);
}

TEST(t_fork_keys)
{
	static const char *const keys[] = {
		"Jc", "Jmin", "Jmax", "S1", "S2", "S3", "S4", "H1", "H2", "H3", "H4",
		"I1", "I2", "I3", "I4", "I5", "J1", "J2", "J3", "Itime",
	};
	char text[512];
	unsigned i;

	for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		snprintf(text, sizeof text, IFACE "%s = 4\n" PEER, keys[i]);
		EXPECT(text, VTC_E_FORK_KEY, 4, keys[i]);
		snprintf(text, sizeof text, MINIMAL "%s = 4\n", keys[i]);
		EXPECT(text, VTC_E_FORK_KEY, 8, keys[i]);
	}
	EXPECT("jc = 4\n", VTC_E_FORK_KEY, 1, "jc");
	EXPECT(IFACE "JMIN=40\n", VTC_E_FORK_KEY, 4, "JMIN");
	EXPECT(IFACE "S5 = 1\n", VTC_E_UNKNOWN_KEY, 4, "S5");
}

TEST(t_structure)
{
	EXPECT("PrivateKey = " K1 "\n", VTC_E_OUTSIDE_SECTION, 1, "PrivateKey");
	EXPECT("\n\nfoo\n", VTC_E_OUTSIDE_SECTION, 3, "foo");
	EXPECT("[Interfaces]\n", VTC_E_BAD_SECTION, 1, "[Interfaces]");
	EXPECT("[Interface\n", VTC_E_BAD_SECTION, 1, "[Interface");
	EXPECT("[Interface]x\n", VTC_E_BAD_SECTION, 1, "[Interface]x");
	EXPECT("[]\n", VTC_E_BAD_SECTION, 1, "[]");
	EXPECT("[Interface]\nPrivateKey\n", VTC_E_NO_EQUALS, 2, "PrivateKey");
	/* a line holding key material is never quoted */
	EXPECT("[Interface]\n= " K1 "\n", VTC_E_NO_EQUALS, 2, "(hidden)");
	EXPECT("[Interface]\nFoo = 1\n", VTC_E_UNKNOWN_KEY, 2, "Foo");
	EXPECT("[Interface]\nPrivateKeys = " K1 "\n", VTC_E_UNKNOWN_KEY, 2, "PrivateKeys");
	EXPECT("[Interface]\nPublicKey = " K2 "\n", VTC_E_WRONG_SECTION, 2, "PublicKey");
	EXPECT(IFACE "[Peer]\nAddress = 10.0.0.3\n", VTC_E_WRONG_SECTION, 5, "Address");
	EXPECT(IFACE "[Peer]\nPostUp = true\n", VTC_E_WRONG_SECTION, 5, "PostUp");
	/* only the first '=' splits */
	EXPECT("[Interface]\nPrivateKey == " K1 "\n", VTC_E_BAD_KEY, 2, "PrivateKey");
}

TEST(t_empty_values)
{
	static const char *const iface[] = { "PrivateKey", "ListenPort", "FwMark" };
	static const char *const peer[] = {
		"PublicKey", "PresharedKey", "AllowedIPs", "Endpoint", "PersistentKeepalive",
	};
	char text[512];
	unsigned i;

	/* the protocol keys need a value, as in the reference tools (an empty one is an unknown line) */
	for (i = 0; i < sizeof iface / sizeof iface[0]; i++) {
		snprintf(text, sizeof text, "[Interface]\n%s =  \n", iface[i]);
		EXPECT(text, VTC_E_EMPTY_VALUE, 2, iface[i]);
	}
	for (i = 0; i < sizeof peer / sizeof peer[0]; i++) {
		snprintf(text, sizeof text, IFACE "[Peer]\n%s=\n", peer[i]);
		EXPECT(text, VTC_E_EMPTY_VALUE, 5, peer[i]);
	}
	/* the setup keys may be empty */
	OK(IFACE "DNS =\nMTU =\nTable =\nPreUp =\nPostUp=\nPreDown=\nPostDown=\nAddress=\n" PEER);
	CHECK(r.dns_count == 0 && r.mtu == 0 && !r.table_off && r.note_count == 4);
	EXPECT(IFACE "SaveConfig =\n", VTC_E_BAD_BOOL, 4, "");
}

TEST(t_listen_port)
{
	OK(IFACE "ListenPort = 65535\n" PEER);
	CHECK(r.listen_port == 65535);
	OK(IFACE "ListenPort = 00051820\n" PEER);
	CHECK(r.listen_port == 51820);
	EXPECT(IFACE "ListenPort = 65536\n", VTC_E_BAD_PORT, 4, "65536");
	EXPECT(IFACE "ListenPort = 99999999999999999999\n", VTC_E_BAD_PORT, 4, NULL);
	EXPECT(IFACE "ListenPort = -1\n", VTC_E_BAD_PORT, 4, "-1");
	EXPECT(IFACE "ListenPort = 51820x\n", VTC_E_BAD_PORT, 4, "51820x");
	EXPECT(IFACE "ListenPort = tunnel\n", VTC_E_BAD_PORT, 4, "tunnel");
}

TEST(t_fwmark)
{
	static const char *const ok_quiet[] = { "off", "OFF", "0", "0x0", "0x00000000000" };
	static const char *const ok_noted[] = {
		"1", "4294967295", "0xffffffff", "0xFFFFFFFF", "0x000000001", "0xaB",
	};
	static const char *const bad[] = {
		"4294967296", "0x100000000", "0X10", "0x", "0xg", "x", "-1", "0x-1", "on", "1e3",
	};
	char text[512];
	unsigned i;

	for (i = 0; i < sizeof ok_quiet / sizeof ok_quiet[0]; i++) {
		snprintf(text, sizeof text, IFACE "FwMark = %s\n" PEER, ok_quiet[i]);
		OK(text);
		CHECK(r.note_count == 0);
	}
	for (i = 0; i < sizeof ok_noted / sizeof ok_noted[0]; i++) {
		snprintf(text, sizeof text, IFACE "FwMark = %s\n" PEER, ok_noted[i]);
		OK(text);
		CHECK(has_note(VTC_N_FWMARK, ok_noted[i]));
	}
	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		snprintf(text, sizeof text, IFACE "FwMark = %s\n" PEER, bad[i]);
		EXPECT(text, VTC_E_BAD_FWMARK, 4, bad[i]);
	}
}

TEST(t_address)
{
	static const char *const bad[] = {
		"10.0.0.256", "10.0.0.02", "010.0.0.2", "10.0.0", "10.0.0.2.5", "10..0.2",
		"10.0.0.2/33", "10.0.0.2/", "10.0.0.2/24/1", "10.0.0.2/-1", "10.0.0.2/a",
		"fd00::2/129", "fd00:::2", "fd00::2::3", "fd00:2", "1:2:3:4:5:6:7:8:9",
		"::g", "fd00::2/", ":1::2", "1::2:", "a.b.c.d", "10.0.0.2a", "0x0a.0.0.2",
		"1:2:3:4:5:6:7::8", "12345::1", "::1.2.3", "::1.2.3.4.5", "1.2.3.4::",
	};
	char text[512];
	unsigned i;

	OK(IFACE PEER);
	CHECK(r.address == ip(10, 0, 0, 2) && r.address_prefix == 32);
	OK("[Interface]\nPrivateKey=" K1 "\nAddress = 10.0.0.2\n" PEER);
	CHECK(r.address_prefix == 32);
	OK("[Interface]\nPrivateKey=" K1 "\nAddress = ,,10.0.0.2/0,\n" PEER);
	CHECK(r.address_prefix == 0);
	OK("[Interface]\nPrivateKey=" K1 "\nAddress = 255.255.255.255/32, 0.0.0.0, ::, ::1, "
	   "1::, 1:2:3:4:5:6:7:8, ::ffff:10.0.0.2, 1:2:3:4:5:6:1.2.3.4, fe80::1:2/10\n" PEER);
	CHECK(r.address == 0xFFFFFFFFu);
	CHECK(r.note_count == 8);
	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		snprintf(text, sizeof text, "[Interface]\nPrivateKey=" K1 "\nAddress = %s\n" PEER, bad[i]);
		EXPECT(text, VTC_E_BAD_ADDRESS, 3, bad[i]);
	}
}

TEST(t_dns)
{
	OK(IFACE "DNS = 1.1.1.1, 2606:4700:4700::1111, home.lan, 1.0.0.1, 8.8.8.8, 1.1.1.1a\n" PEER);
	CHECK(r.dns_count == 2 && r.dns[0] == ip(1, 1, 1, 1) && r.dns[1] == ip(1, 0, 0, 1));
	CHECK(has_note(VTC_N_DNS_IPV6, "2606:4700:4700::1111"));
	CHECK(has_note(VTC_N_DNS_SEARCH, "home.lan"));
	CHECK(has_note(VTC_N_DNS_SEARCH, "1.1.1.1a"));
	CHECK(has_note(VTC_N_DNS_EXTRA, "8.8.8.8"));
	EXPECT(IFACE "DNS = 1.1.1.999\n", VTC_E_BAD_DNS, 4, "1.1.1.999");
	EXPECT(IFACE "DNS = 0.0.0.0\n", VTC_E_BAD_DNS, 4, "0.0.0.0");
	EXPECT(IFACE "DNS = 1.1.1\n", VTC_E_BAD_DNS, 4, "1.1.1");
	EXPECT(IFACE "DNS = ...\n", VTC_E_BAD_DNS, 4, "...");
	EXPECT(IFACE "DNS = fe80::zz\n", VTC_E_BAD_DNS, 4, "fe80::zz");
}

TEST(t_mtu)
{
	OK(IFACE "MTU = 576\n" PEER);
	CHECK(r.mtu == 576);
	OK(IFACE "MTU = 1440\n" PEER);
	CHECK(r.mtu == 1440 && r.note_count == 0);
	OK(IFACE "MTU = 1500\n" PEER);
	CHECK(r.mtu == 1440 && has_note(VTC_N_MTU_CLAMPED, "1500"));
	OK(IFACE "MTU = 1500\nMTU =\n" PEER);
	CHECK(r.mtu == 0);
	EXPECT(IFACE "MTU = 575\n", VTC_E_BAD_MTU, 4, "575");
	EXPECT(IFACE "MTU = 0\n", VTC_E_BAD_MTU, 4, "0");
	EXPECT(IFACE "MTU = 65536\n", VTC_E_BAD_MTU, 4, "65536");
	EXPECT(IFACE "MTU = auto\n", VTC_E_BAD_MTU, 4, "auto");
}

TEST(t_table_and_saveconfig)
{
	VtProfile p;

	OK(IFACE "Table = off\n" PEER);
	CHECK(r.table_off && r.note_count == 0);
	CHECK(vtc_to_profile(&r, "t", &p) == 0 && p.flags == VT_PROFILE_NO_ROUTES);
	OK(IFACE "Table = OFF\n" PEER);
	CHECK(r.table_off);
	OK(IFACE "Table = off\nTable = auto\n" PEER);
	CHECK(!r.table_off && r.note_count == 0);
	OK(IFACE "Table = main\n" PEER);
	CHECK(!r.table_off && has_note(VTC_N_TABLE, "main"));
	OK(IFACE "SaveConfig = FALSE\n" PEER);
	CHECK(r.note_count == 0);
	OK(IFACE "SaveConfig = True\n" PEER);
	CHECK(has_note(VTC_N_SAVECONFIG, "True"));
	EXPECT(IFACE "SaveConfig = yes\n", VTC_E_BAD_BOOL, 4, "yes");
}

TEST(t_allowed)
{
	char text[2048];
	size_t n;
	unsigned i;

	OK(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.1, 192.168.1.77/24, +172.16.0.0/12\n");
	CHECK(r.allowed_count == 3);
	CHECK(r.allowed[0].net == ip(10, 0, 0, 1) && r.allowed[0].prefix == 32);
	CHECK(r.allowed[1].net == ip(192, 168, 1, 0) && r.allowed[1].prefix == 24);
	CHECK(has_note(VTC_N_ALLOWED_HOST_BITS, "192.168.1.77/24"));
	CHECK(r.allowed[2].net == ip(172, 16, 0, 0));
	/* '-' removes an earlier entry, as an incremental set in the reference tools would */
	OK(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/8, 10.1.0.0/16, 10.2.0.0/16\n"
	   "AllowedIPs = -10.1.0.0/16, -10.9.0.0/16, -fd00::/8\n");
	CHECK(r.allowed_count == 2 && r.allowed[1].net == ip(10, 2, 0, 0));
	CHECK(r.allowed[2].net == 0 && r.allowed[2].prefix == 0);
	CHECK(has_note(VTC_N_ALLOWED_REMOVE, "-10.1.0.0/16") && has_note(VTC_N_ALLOWED_REMOVE, "-10.9.0.0/16"));
	CHECK(has_note(VTC_N_ALLOWED_IPV6, "-fd00::/8"));
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/8\nAllowedIPs = -10.0.0.0/8\n",
	       VTC_E_NO_ALLOWED, 4, "");
	OK(IFACE PEER_NOALLOWED "AllowedIPs = 0.0.0.0/1, 128.0.0.0/1\n");
	CHECK(r.full_tunnel);
	OK(IFACE PEER_NOALLOWED "AllowedIPs = 128.0.0.0/1, 0.0.0.0/2, 64.0.0.0/2\n");
	CHECK(r.full_tunnel);
	OK(IFACE PEER_NOALLOWED "AllowedIPs = 0.0.0.0/1, 128.0.0.0/2\n");
	CHECK(!r.full_tunnel);
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ,10.0.0.0/8\n", VTC_E_BAD_ALLOWED, 7, "");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/8,\n", VTC_E_BAD_ALLOWED, 7, "");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/x\n", VTC_E_BAD_ALLOWED, 7, "10.0.0.0/x");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/33\n", VTC_E_BAD_ALLOWED, 7, "10.0.0.0/33");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/8/8\n", VTC_E_BAD_ALLOWED, 7, "10.0.0.0/8/8");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = /8\n", VTC_E_BAD_ALLOWED, 7, "/8");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = +\n", VTC_E_BAD_ALLOWED, 7, "+");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ++10.0.0.0/8\n", VTC_E_BAD_ALLOWED, 7, "++10.0.0.0/8");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ::g/0\n", VTC_E_BAD_ALLOWED, 7, "::g/0");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ::/129\n", VTC_E_BAD_ALLOWED, 7, "::/129");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = ::/\n", VTC_E_BAD_ALLOWED, 7, "::/");
	EXPECT(IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/8 10.0.0.0/8\n", VTC_E_BAD_ALLOWED, 7, NULL);

	/* exactly the maximum, then one more */
	n = (size_t)snprintf(text, sizeof text, IFACE PEER_NOALLOWED "AllowedIPs = 10.0.0.0/32");
	for (i = 1; i < VT_MAX_ALLOWED; i++)
		n += (size_t)snprintf(text + n, sizeof text - n, ",10.0.%u.0/24", i);
	snprintf(text + n, sizeof text - n, "\n");
	OK(text);
	CHECK(r.allowed_count == VT_MAX_ALLOWED);
	snprintf(text + n, sizeof text - n, "\nAllowedIPs=10.1.0.0/16\n");
	EXPECT(text, VTC_E_TOO_MANY_ALLOWED, 8, "10.1.0.0/16");
	/* duplicates do not count against the limit */
	snprintf(text + n, sizeof text - n, "\nAllowedIPs=10.0.0.0/32,::/0\n");
	OK(text);
	CHECK(r.allowed_count == VT_MAX_ALLOWED);
}

TEST(t_endpoint)
{
	static const struct { const char *v; int code; } cases[] = {
		{ "[2001:db8::1]:51820", VTC_E_ENDPOINT_IPV6 },
		{ "2001:db8::1:51820", VTC_E_ENDPOINT_IPV6 },
		{ "[::ffff:192.0.2.1]:51820", VTC_E_ENDPOINT_IPV6 },
		{ "[2001:db8::1]", VTC_E_BAD_ENDPOINT },
		{ "[2001:db8::1]:", VTC_E_BAD_ENDPOINT },
		{ "[2001:db8::1]51820", VTC_E_BAD_ENDPOINT },
		{ "[2001:db8::1:51820", VTC_E_BAD_ENDPOINT },
		{ "[x]:51820", VTC_E_BAD_ENDPOINT },
		{ "[]:51820", VTC_E_BAD_ENDPOINT },
		{ "a:b:c", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com:", VTC_E_BAD_ENDPOINT },
		{ ":51820", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com:0", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com:65536", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com:http", VTC_E_BAD_ENDPOINT },
		{ "vpn.example.com:-1", VTC_E_BAD_ENDPOINT },
		{ "1.2.3:51820", VTC_E_BAD_ENDPOINT },
		{ "1.2.3.4.5:51820", VTC_E_BAD_ENDPOINT },
		{ "1.2.3.256:51820", VTC_E_BAD_ENDPOINT },
		{ "192.0.2.010:51820", VTC_E_BAD_ENDPOINT },
		{ "ex@mple.com:51820", VTC_E_BAD_ENDPOINT },
		{ "a..b:51820", VTC_E_BAD_ENDPOINT },
		{ ".a:51820", VTC_E_BAD_ENDPOINT },
		{ ".:51820", VTC_E_BAD_ENDPOINT },
		{ "h\xc3\xa9.example:51820", VTC_E_BAD_ENDPOINT },
	};
	char text[1024], host[300];
	unsigned i;

	for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		snprintf(text, sizeof text, IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint = %s\n", cases[i].v);
		EXPECT(text, cases[i].code, 6, NULL);
	}
	OK(IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint = vpn.example.com.:65535\nAllowedIPs=0.0.0.0/0\n");
	CHECK(strcmp(r.endpoint_host, "vpn.example.com.") == 0 && r.endpoint_port == 65535);
	OK(IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint = my_box-1.lan:1\nAllowedIPs=0.0.0.0/0\n");
	CHECK(strcmp(r.endpoint_host, "my_box-1.lan") == 0 && !r.endpoint_is_ip);
	OK(IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint = 0.0.0.0:1\nEndpoint = 1example:1\nAllowedIPs=0.0.0.0/0\n");
	CHECK(!r.endpoint_is_ip && r.endpoint_ip == 0 && strcmp(r.endpoint_host, "1example") == 0);

	/* label of 63 is fine, 64 is not; 253 characters in total is the limit */
	memset(host, 'a', 63);
	strcpy(host + 63, ".example");
	snprintf(text, sizeof text, IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint=%s:1\nAllowedIPs=0.0.0.0/0\n", host);
	OK(text);
	memset(host, 'a', 64);
	strcpy(host + 64, ".example");
	snprintf(text, sizeof text, IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint=%s:1\n", host);
	EXPECT(text, VTC_E_BAD_ENDPOINT, 6, NULL);
	for (i = 0; i < 253; i++)
		host[i] = (i % 2) ? '.' : 'a';
	host[252] = 'b';
	host[253] = '\0';
	snprintf(text, sizeof text, IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint=%s.:1\nAllowedIPs=0.0.0.0/0\n", host);
	OK(text);
	CHECK(strlen(r.endpoint_host) == 254);
	host[253] = '.';
	host[254] = 'c';
	host[255] = '\0';
	snprintf(text, sizeof text, IFACE "[Peer]\nPublicKey=" K2 "\nEndpoint=%s:1\n", host);
	EXPECT(text, VTC_E_BAD_ENDPOINT, 6, NULL);
	/* the quoted value is cut for display */
	CHECK(strlen(r.error_item) == VTC_ITEM_MAX - 1);
	CHECK(strcmp(r.error_item + VTC_ITEM_MAX - 4, "...") == 0);
}

TEST(t_keepalive)
{
	static const struct { const char *v; unsigned s; } good[] = {
		{ "off", 0 }, { "Off", 0 }, { "0", 0 }, { "25", 25 }, { "65535", 65535 }, { "007", 7 },
	};
	static const char *const bad[] = { "65536", "-1", "25s", "on", "0x19", "99999999999" };
	char text[512];
	unsigned i;

	for (i = 0; i < sizeof good / sizeof good[0]; i++) {
		snprintf(text, sizeof text, MINIMAL "PersistentKeepalive = %s\n", good[i].v);
		OK(text);
		CHECK(r.keepalive_s == good[i].s);
	}
	for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		snprintf(text, sizeof text, MINIMAL "PersistentKeepalive = %s\n", bad[i]);
		EXPECT(text, VTC_E_BAD_KEEPALIVE, 8, bad[i]);
	}
}

/* ---- adversarial input -------------------------------------------------- */

TEST(t_nul)
{
	static const char text[] = "[Interface]\nPrivateKey = " K1 "\n\0Address = 10.0.0.2\n";

	expect_line(text, sizeof text - 1, VTC_E_NUL, 3, "", __LINE__);
	expect_line("\0", 1, VTC_E_NUL, 1, "", __LINE__);
	/* in a comment too: a profile is text */
	expect_line(MINIMAL "# a\0b\n", sizeof(MINIMAL "# a\0b\n") - 1, VTC_E_NUL, 8, "", __LINE__);
	/* text without a terminator: only len bytes are read */
	expect_line(MINIMAL "garbage", strlen(MINIMAL), VTC_OK, 0, "", __LINE__);
}

TEST(t_long_lines)
{
	static char big[300000];
	size_t n;
	unsigned i;

	/* whitespace and comments do not count towards the limit */
	n = (size_t)snprintf(big, sizeof big, IFACE);
	memset(big + n, ' ', 100000);
	n += 100000;
	n += (size_t)snprintf(big + n, sizeof big - n, "DNS = 1.1.1.1");
	memset(big + n, '\t', 100000);
	n += 100000;
	big[n++] = '#';
	memset(big + n, 'c', 50000);
	n += 50000;
	n += (size_t)snprintf(big + n, sizeof big - n, "\n" PEER);
	expect_line(big, n, VTC_OK, 0, "", __LINE__);
	CHECK(r.dns_count == 1);

	/* a long hook script is fine: it is ignored anyway */
	{
		static const char *const hooks[] = { "PreUp", "PostUp", "PreDown", "PostDown" };
		for (i = 0; i < 4; i++) {
			n = (size_t)snprintf(big, sizeof big, IFACE "%s = ", hooks[i]);
			memset(big + n, 'x', 10000);
			n += 10000;
			n += (size_t)snprintf(big + n, sizeof big - n, "\n" PEER);
			expect_line(big, n, VTC_OK, 0, "", __LINE__);
			CHECK(has_note(VTC_N_HOOK, hooks[i]));
		}
	}

	/* exactly VTC_LINE_MAX is accepted */
	n = (size_t)snprintf(big, sizeof big, IFACE PEER "AllowedIPs=0.0.0.0/0");
	{
		size_t line = strlen("AllowedIPs=0.0.0.0/0");
		while (line + 10 <= VTC_LINE_MAX) {
			n += (size_t)snprintf(big + n, sizeof big - n, ",::1/128");
			line += 8;
		}
		while (line < VTC_LINE_MAX) {
			n += (size_t)snprintf(big + n, sizeof big - n, ",");
			line++;
		}
	}
	/* ...but this line ends in commas, so it is a syntax error, not a length one */
	expect_line(big, n, VTC_E_BAD_ALLOWED, 8, "", __LINE__);
	big[n] = 'x';
	expect_line(big, n + 1, VTC_E_LINE_TOO_LONG, 8, "AllowedIPs", __LINE__);

	/* too long: a value, a section header, a line with no '=' */
	n = (size_t)snprintf(big, sizeof big, IFACE "DNS = ");
	for (i = 0; i < 300; i++)
		n += (size_t)snprintf(big + n, sizeof big - n, "1.1.1.1,");
	expect_line(big, n, VTC_E_LINE_TOO_LONG, 4, "DNS", __LINE__);
	n = (size_t)snprintf(big, sizeof big, "[");
	memset(big + n, 'I', 5000);
	n += 5000;
	expect_line(big, n, VTC_E_LINE_TOO_LONG, 1, NULL, __LINE__);
	n = (size_t)snprintf(big, sizeof big, IFACE);
	memset(big + n, 'k', 5000);
	n += 5000;
	expect_line(big, n, VTC_E_LINE_TOO_LONG, 4, NULL, __LINE__);
}

TEST(t_huge_counts)
{
	static char big[400000];
	size_t n;
	unsigned i;

	/* ten thousand IPv6 entries: notes are capped, nothing overflows */
	n = (size_t)snprintf(big, sizeof big, IFACE PEER);
	for (i = 0; i < 10000; i++)
		n += (size_t)snprintf(big + n, sizeof big - n, "AllowedIPs = fd00::%x/128\n", i);
	expect_line(big, n, VTC_OK, 0, "", __LINE__);
	CHECK(r.note_count == VTC_MAX_NOTES && r.notes_dropped == 10000 - VTC_MAX_NOTES);
	CHECK(r.allowed_count == 1);
	/* more notes than the counter holds */
	n = (size_t)snprintf(big, sizeof big, IFACE);
	for (i = 0; i < 10000; i++)
		n += (size_t)snprintf(big + n, sizeof big - n, "DNS=x,x,x,x,x,x,x,x\n");
	n += (size_t)snprintf(big + n, sizeof big - n, PEER);
	expect_line(big, n, VTC_OK, 0, "", __LINE__);
	CHECK(r.notes_dropped == 0xFFFF);
	/* thousands of empty lines and sections */
	n = 0;
	for (i = 0; i < 20000; i++)
		n += (size_t)snprintf(big + n, sizeof big - n, "[Interface]\n\n");
	n += (size_t)snprintf(big + n, sizeof big - n, IFACE PEER);
	expect_line(big, n, VTC_OK, 0, "", __LINE__);
}

static uint32_t g_rng = 0x12345678;

static uint32_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

/* Invariants that must hold for any input. */
static void check_invariants(int ret)
{
	CHECK(ret == r.error);
	CHECK(ret >= 0 && ret < VTC_E_COUNT);
	CHECK(memchr(r.error_item, 0, sizeof r.error_item) != NULL);
	CHECK(r.note_count <= VTC_MAX_NOTES);
	if (ret == VTC_OK) {
		unsigned i;
		CHECK(r.allowed_count >= 1 && r.allowed_count <= VT_MAX_ALLOWED);
		for (i = 0; i < r.allowed_count; i++)
			CHECK(r.allowed[i].prefix <= 32 &&
			      (r.allowed[i].prefix == 32 ||
			       (r.allowed[i].net & (0xFFFFFFFFu >> r.allowed[i].prefix)) == 0));
		CHECK(memchr(r.endpoint_host, 0, sizeof r.endpoint_host) != NULL && r.endpoint_host[0]);
		CHECK(r.endpoint_port != 0 && r.address_prefix <= 32 && r.dns_count <= 2);
		CHECK(r.mtu == 0 || (r.mtu >= 576 && r.mtu <= 1440));
		CHECK(memchr(r.check_url, 0, sizeof r.check_url) != NULL);
	} else {
		CHECK(all_zero(r.private_key, 32) && all_zero(r.peer_public, 32));
	}
}

TEST(t_fuzz)
{
	static const char base[] =
		"[Interface]\nPrivateKey = " K1 "\nAddress = 10.0.0.2/24, fd00::2/64\n"
		"DNS = 1.1.1.1, home.lan\nMTU = 1420\nListenPort = 51820\nFwMark = 0x10\n"
		"# vita-turbo-vpn: check = https://ip.example.org/\n"
		"[Peer]\nPublicKey = " K2 "\nPresharedKey = " K3 "\n"
		"Endpoint = vpn.example.com:51820\nAllowedIPs = 0.0.0.0/1, 128.0.0.0/1, ::/0\n"
		"PersistentKeepalive = 25\n";
	static const char alphabet[] = "[]=#,:/.+-\n\r\t 0123456789abcdefxAZ=\0";
	char buf[sizeof base + 64];
	unsigned iter, ok = 0, k;

	for (iter = 0; iter < 200000; iter++) {
		size_t n = sizeof base - 1;
		unsigned edits = 1 + rnd() % 4;
		int ret;

		memcpy(buf, base, n);
		for (k = 0; k < edits; k++) {
			size_t at = rnd() % n;
			char c = (rnd() & 1) ? alphabet[rnd() % (sizeof alphabet - 1)] : (char)rnd();
			switch (rnd() % 3) {
			case 0: /* replace */
				buf[at] = c;
				break;
			case 1: /* insert */
				if (n < sizeof buf) {
					memmove(buf + at + 1, buf + at, n - at);
					buf[at] = c;
					n++;
				}
				break;
			default: /* delete */
				memmove(buf + at, buf + at + 1, n - at - 1);
				n--;
			}
		}
		ret = vtc_parse(buf, n, &r);
		check_invariants(ret);
		ok += ret == VTC_OK;
	}
	/* plenty of mutants still parse; plenty do not (a sanity range: the
	 * count moves with the corpus text - 19,881 after the directive became
	 * "vita-turbo-vpn:", 20,000+ before) */
	CHECK(ok > 15000 && ok < 190000);

	/* pure noise */
	for (iter = 0; iter < 20000; iter++) {
		size_t n = rnd() % sizeof buf, i;
		for (i = 0; i < n; i++)
			buf[i] = alphabet[rnd() % (sizeof alphabet - 1)];
		check_invariants(vtc_parse(buf, n, &r));
	}
}

TEST(t_args)
{
	VtProfile p;

	CHECK(vtc_parse("x", 1, NULL) == VTC_E_ARG);
	CHECK(vtc_parse(NULL, 5, &r) == VTC_E_ARG && r.error == VTC_E_ARG);
	CHECK(vtc_parse(NULL, 0, &r) == VTC_E_NO_PRIVATE_KEY);
	CHECK(vtc_to_profile(&r, "x", &p) == -1);
	CHECK(vtc_to_profile(NULL, "x", &p) == -1);
	P(MINIMAL);
	CHECK(vtc_to_profile(&r, "x", NULL) == -1);
}

/* ---- conversion and messages ------------------------------------------- */

TEST(t_profile_names)
{
	VtProfile p;
	/* 30 bytes of ASCII then a two-byte character straddling the limit */
	const char *utf8 = "abcdefghijklmnopqrstuvwxyz0123\xc3\xa9xyz.conf";

	P(MINIMAL);
	CHECK(vtc_to_profile(&r, "home.CONF", &p) == 0 && strcmp(p.name, "home") == 0);
	CHECK(vtc_to_profile(&r, ".conf", &p) == 0 && strcmp(p.name, "") == 0);
	CHECK(vtc_to_profile(&r, "conf", &p) == 0 && strcmp(p.name, "conf") == 0);
	CHECK(vtc_to_profile(&r, "a.conf.txt", &p) == 0 && strcmp(p.name, "a.conf.txt") == 0);
	CHECK(vtc_to_profile(&r, NULL, &p) == 0 && p.name[0] == '\0');
	CHECK(vtc_to_profile(&r, utf8, &p) == 0 && strcmp(p.name, "abcdefghijklmnopqrstuvwxyz0123") == 0);
	CHECK(vtc_to_profile(&r, "abcdefghijklmnopqrstuvwxyz01234\xc3\xa9", &p) == 0 &&
	      strlen(p.name) == 31);
	CHECK(vtc_to_profile(&r, "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac"
	                         "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", &p) == 0 &&
	      strlen(p.name) == 30);
	/* nothing but continuation bytes: nothing safe to keep */
	CHECK(vtc_to_profile(&r, "\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80"
	                         "\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80\x80", &p) == 0 &&
	      p.name[0] == '\0');
}

TEST(t_messages)
{
	char buf[256];
	size_t n;
	int i;

	for (i = 0; i < VTC_E_COUNT; i++)
		CHECK(vtc_error_text(i) != NULL && vtc_error_text(i)[0]);
	for (i = 1; i < VTC_N_COUNT; i++)
		CHECK(vtc_note_text(i) != NULL && vtc_note_text(i)[0]);
	CHECK(strcmp(vtc_error_text(-1), "unknown error") == 0);
	CHECK(strcmp(vtc_error_text(VTC_E_COUNT), "unknown error") == 0);
	CHECK(strcmp(vtc_note_text(0), "note") == 0 && strcmp(vtc_note_text(VTC_N_COUNT), "note") == 0);

	P(IFACE "Jc = 4\n");
	n = vtc_format_error(&r, buf, sizeof buf);
	CHECK(strcmp(buf, "Line 4: `Jc': key from a non-standard protocol variant; "
	                  "a standard server would not understand this profile") == 0);
	CHECK(n == strlen(buf));
	CHECK(vtc_format_error(&r, buf, 8) == n && strcmp(buf, "Line 4:") == 0);
	CHECK(vtc_format_error(&r, NULL, 0) == n);
	CHECK(vtc_format_error(&r, buf, 0) == n);
	CHECK(vtc_format_error(&r, buf, 1) == n && buf[0] == '\0');
	P(IFACE);
	vtc_format_error(&r, buf, sizeof buf);
	CHECK(strcmp(buf, "no [Peer] section") == 0);
	P("[Interface]\nPrivateKey = " K3 "x\n");
	vtc_format_error(&r, buf, sizeof buf);
	CHECK(strstr(buf, "F2AL") == NULL && strstr(buf, "PrivateKey") != NULL);
	vtc_format_error(NULL, buf, sizeof buf);
	CHECK(strcmp(buf, "internal error: missing argument") == 0);
	P(MINIMAL "DNS = home.lan\n");
	CHECK(r.note_count == 0); /* DNS in [Peer] is an error, not a note */
	P(IFACE "DNS = home.lan\n" PEER);
	vtc_format_note(&r.notes[0], buf, sizeof buf);
	CHECK(strcmp(buf, "Line 4: `home.lan': DNS search domain ignored") == 0);
	vtc_format_note(NULL, buf, sizeof buf);
	CHECK(strcmp(buf, "internal error: missing argument") == 0);
	{
		VtcResult big;
		memset(&big, 0, sizeof big);
		big.error = VTC_E_UNKNOWN_KEY;
		big.error_line = 4294967295u;
		strcpy(big.error_item, "k");
		vtc_format_error(&big, buf, sizeof buf);
		CHECK(strcmp(buf, "Line 4294967295: `k': unknown key") == 0);
	}
}

/* ---- duplicate check ---------------------------------------------------- */

TEST(t_normalise)
{
	char buf[64];
	size_t n;

	n = vtc_normalise("a\r\nb\r\n", 6, buf);
	CHECK(n == 3 && memcmp(buf, "a\nb", 3) == 0);
	n = vtc_normalise("a\nb\n\n", 5, buf);
	CHECK(n == 4 && memcmp(buf, "a\nb\n", 4) == 0);
	n = vtc_normalise("a\rb\r", 4, buf);
	CHECK(n == 4 && memcmp(buf, "a\rb\r", 4) == 0);
	n = vtc_normalise("a\r\r\n", 4, buf);
	CHECK(n == 2 && memcmp(buf, "a\r", 2) == 0);
	n = vtc_normalise("\n", 1, buf);
	CHECK(n == 0);
	n = vtc_normalise("", 0, buf);
	CHECK(n == 0);
	CHECK(vtc_normalise(NULL, 3, buf) == 0 && vtc_normalise("abc", 3, NULL) == 0);
	strcpy(buf, "x\r\ny\r\n");
	n = vtc_normalise(buf, 6, buf);
	CHECK(n == 3 && memcmp(buf, "x\ny", 3) == 0);

	CHECK(vtc_same_text("a\r\nb\r\n", 6, "a\nb", 3));
	CHECK(vtc_same_text("a\nb\n", 4, "a\r\nb", 4));
	CHECK(!vtc_same_text("a\nb\n\n", 5, "a\nb", 3));
	CHECK(!vtc_same_text("a\nb", 3, "a\nb ", 4));
	CHECK(!vtc_same_text("a\nb", 3, "a\nc", 3));
	CHECK(!vtc_same_text("a\rb", 3, "a\nb", 3));
	CHECK(vtc_same_text("", 0, "\r\n", 2));
	CHECK(!vtc_same_text(MINIMAL, strlen(MINIMAL), MINIMAL "# x\n", strlen(MINIMAL "# x\n")));
	CHECK(!vtc_same_text(NULL, 0, "", 0) && !vtc_same_text("", 0, NULL, 0));
}

TEST(t_normalise_random)
{
	/* the streaming comparison agrees with normalising both and comparing */
	static const char set[] = "\r\na";
	char a[12], b[12], na[12], nb[12];
	unsigned iter;

	for (iter = 0; iter < 200000; iter++) {
		size_t la = rnd() % sizeof a, lb = rnd() % sizeof b, i, x, y;
		for (i = 0; i < la; i++)
			a[i] = set[rnd() % 3];
		for (i = 0; i < lb; i++)
			b[i] = set[rnd() % 3];
		x = vtc_normalise(a, la, na);
		y = vtc_normalise(b, lb, nb);
		CHECK(vtc_same_text(a, la, b, lb) == (x == y && memcmp(na, nb, x) == 0));
	}
}

/* ---- automatic names ---------------------------------------------------- */

typedef struct Taken {
	const char *names[8];
	int calls;
} Taken;

static int taken(void *ctx, const char *name)
{
	Taken *t = ctx;
	int i;

	t->calls++;
	for (i = 0; i < 8 && t->names[i]; i++)
		if (strcmp(t->names[i], name) == 0)
			return 1;
	return 0;
}

static int all_taken(void *ctx, const char *name)
{
	(void)name;
	++*(int *)ctx;
	return 1;
}

TEST(t_qr_names)
{
	char out[VTC_FILE_NAME_MAX];
	char host[200];
	Taken t = { { NULL }, 0 };
	int calls = 0;

	CHECK(vtc_qr_file_name("home.example.org", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "home.example.org.conf") == 0 && t.calls == 1);
	t.names[0] = "home.example.org.conf";
	t.names[1] = "home.example.org-2.conf";
	CHECK(vtc_qr_file_name("home.example.org", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "home.example.org-3.conf") == 0);
	CHECK(vtc_qr_file_name("192.0.2.10", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "192.0.2.10.conf") == 0);
	CHECK(vtc_qr_file_name("vpn.example.com.", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "vpn.example.com.conf") == 0);
	CHECK(vtc_qr_file_name("..a/b\\c:d", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "a_b_c_d.conf") == 0);
	CHECK(vtc_qr_file_name("my_box", taken, &t, out, sizeof out) == 0 && strcmp(out, "my_box.conf") == 0);
	CHECK(vtc_qr_file_name("a{b~c", taken, &t, out, sizeof out) == 0);
	CHECK(strcmp(out, "a_b_c.conf") == 0);

	memset(host, 'h', 100);
	host[100] = '\0';
	host[63] = '-';
	CHECK(vtc_qr_file_name(host, taken, &t, out, sizeof out) == 0);
	CHECK(strlen(out) == 63 + 5 && out[62] == 'h');

	CHECK(vtc_qr_file_name("a", all_taken, &calls, out, sizeof out) == -2 && calls == 999);
	CHECK(out[0] == '\0');
	/* the last suffix tried is -999 and still fits */
	CHECK(vtc_qr_file_name(host, all_taken, &calls, out, sizeof out) == -2);

	CHECK(vtc_qr_file_name("", taken, &t, out, sizeof out) == -1);
	CHECK(vtc_qr_file_name("...", taken, &t, out, sizeof out) == -1);
	CHECK(vtc_qr_file_name("-", taken, &t, out, sizeof out) == -1);
	CHECK(vtc_qr_file_name(NULL, taken, &t, out, sizeof out) == -1);
	CHECK(vtc_qr_file_name("a", NULL, &t, out, sizeof out) == -1);
	CHECK(vtc_qr_file_name("a", taken, &t, NULL, sizeof out) == -1);
	CHECK(vtc_qr_file_name("a", taken, &t, out, VTC_FILE_NAME_MAX - 1) == -1);
}

static int count_to(void *ctx, const char *name)
{
	int *left = ctx;

	(void)name;
	return (*left)-- > 0;
}

TEST(t_qr_names_suffix_fits)
{
	/* the longest base with the longest suffix fills the buffer exactly */
	char out[VTC_FILE_NAME_MAX];
	char host[VTC_NAME_BASE_MAX + 1];
	int left = 998;

	memset(host, 'z', VTC_NAME_BASE_MAX);
	host[VTC_NAME_BASE_MAX] = '\0';
	CHECK(vtc_qr_file_name(host, count_to, &left, out, sizeof out) == 0);
	CHECK(strlen(out) == VTC_FILE_NAME_MAX - 1);
	CHECK(strcmp(out + VTC_NAME_BASE_MAX, "-999.conf") == 0);
}

/* ---- subnets ------------------------------------------------------------ */

static VtProfile profile_with(const char *allowed)
{
	char text[1024];
	VtProfile p;

	snprintf(text, sizeof text, IFACE PEER_NOALLOWED "AllowedIPs = %s\n", allowed);
	CHECK(P(text) == VTC_OK);
	CHECK(vtc_to_profile(&r, "x", &p) == 0);
	return p;
}

TEST(t_clash)
{
	VtProfile p;
	unsigned idx = 99;

	p = profile_with("0.0.0.0/0");
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, &idx) == VTC_CLASH_NONE && idx == 99);
	p = profile_with("0.0.0.0/0, 192.168.1.0/24");
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, &idx) == VTC_CLASH_INSIDE && idx == 1);
	p = profile_with("10.253.0.1/32, 192.168.1.0/24");
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, &idx) == VTC_CLASH_INSIDE && idx == 1);
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 0, 0), 16, NULL) == VTC_CLASH_INSIDE);
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 2, 0), 24, NULL) == VTC_CLASH_NONE);
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 0, 0), 8, &idx) == VTC_CLASH_INSIDE && idx == 0);
	p = profile_with("192.168.0.0/16");
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, &idx) == VTC_CLASH_SHADOWED && idx == 0);
	CHECK(vtc_subnet_clash(&p, ip(192, 169, 1, 0), 24, NULL) == VTC_CLASH_NONE);
	CHECK(vtc_subnet_clash(&p, 0, 0, NULL) == VTC_CLASH_INSIDE);
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 7), 40, NULL) == VTC_CLASH_SHADOWED);
	p = profile_with("0.0.0.0/1, 128.0.0.0/1");
	CHECK(vtc_subnet_clash(&p, ip(192, 168, 1, 0), 24, NULL) == VTC_CLASH_NONE);
	p = profile_with("10.0.0.0/8");
	p.flags = VT_PROFILE_NO_ROUTES;
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 0, 0), 24, NULL) == VTC_CLASH_NONE);
	CHECK(vtc_subnet_clash(NULL, 0, 0, NULL) == VTC_CLASH_NONE);
	p = profile_with("10.0.0.0/8");
	p.allowed[0].prefix = 77; /* hand-made garbage is clamped to /32, not trusted */
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 0, 0), 24, NULL) == VTC_CLASH_INSIDE);
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 1, 0), 24, NULL) == VTC_CLASH_NONE);
	CHECK(vtc_subnet_clash(&p, ip(10, 0, 0, 0), 77, NULL) == VTC_CLASH_INSIDE);
	p = profile_with("10.0.0.0/8");
	p.allowed_count = 200; /* beyond the array: only VT_MAX_ALLOWED are read */
	CHECK(vtc_subnet_clash(&p, ip(172, 16, 0, 0), 24, NULL) == VTC_CLASH_NONE);
}

TEST(t_covers_all)
{
	VtPrefix l[4];

	memset(l, 0, sizeof l);
	CHECK(!vtc_covers_all(NULL, 3));
	CHECK(!vtc_covers_all(l, 0));
	CHECK(vtc_covers_all(l, 1));                  /* 0.0.0.0/0 */
	l[0].prefix = 1;
	CHECK(!vtc_covers_all(l, 1));
	l[1].net = 0x80000000u;
	l[1].prefix = 1;
	CHECK(vtc_covers_all(l, 2));
	l[1].prefix = 2;
	CHECK(!vtc_covers_all(l, 2));
	l[2].net = 0xC0000000u;
	l[2].prefix = 2;
	CHECK(vtc_covers_all(l, 3));
	l[3] = l[0];
	CHECK(vtc_covers_all(l, 4));
	l[0].prefix = 40; /* garbage prefix treated as /32 */
	CHECK(!vtc_covers_all(l, 1));
	l[0].net = 0xFFFFFFFFu;
	l[0].prefix = 32;
	CHECK(!vtc_covers_all(l, 1));
}

int main(void)
{
	RUN(t_dual_stack_layout);
	RUN(t_home_server_style);
	RUN(t_web_ui_style);
	RUN(t_nas_style);
	RUN(t_router_style);
	RUN(t_exclude_private);
	RUN(t_crlf_bom_file);
	RUN(t_whitespace_and_case);
	RUN(t_repeats);
	RUN(t_comments);
	RUN(t_no_key_in_messages);
	RUN(t_directive);
	RUN(t_missing);
	RUN(t_multiple_peers);
	RUN(t_bad_keys);
	RUN(t_fork_keys);
	RUN(t_structure);
	RUN(t_empty_values);
	RUN(t_listen_port);
	RUN(t_fwmark);
	RUN(t_address);
	RUN(t_dns);
	RUN(t_mtu);
	RUN(t_table_and_saveconfig);
	RUN(t_allowed);
	RUN(t_endpoint);
	RUN(t_keepalive);
	RUN(t_nul);
	RUN(t_long_lines);
	RUN(t_huge_counts);
	RUN(t_fuzz);
	RUN(t_args);
	RUN(t_profile_names);
	RUN(t_messages);
	RUN(t_normalise);
	RUN(t_normalise_random);
	RUN(t_qr_names);
	RUN(t_qr_names_suffix_fits);
	RUN(t_clash);
	RUN(t_covers_all);

	vtc_wipe(&r, sizeof r);
	printf("%d tests, %d checks, %d failed\n", g_tests, g_checks, g_failed);
	return g_failed != 0;
}
