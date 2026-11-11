/*
 * tn_pair_test.c - two copies of kernel/proto.c talking to each other on the
 * host, no network and no Linux peer needed (pair.sh builds it).
 *
 * Copy A is the usual one (tnp_*); copy B is the same object with its public
 * symbols renamed to tnpb_* (objcopy), so each has its own peer state. Both
 * share the core and its two module-wide locks, as on the Vita. Datagrams go
 * through one FIFO; the main thread pumps it.
 *
 * Checks, for the tunnel-protection work:
 *   1. handshake A -> B, data both ways; the handshake counter moves on both
 *      sides (initiator at the response, responder at the confirming data);
 *   2. tnp_rehandshake from UP: state HANDSHAKING, then UP with the counter
 *      moved (what the lockdown waits for);
 *   3. tnp_stop: IDLE, no session (encrypt fails), reconnect works;
 *   4. tnp_same_private: true for A's own key, false for another;
 * The responder refuses initiations less than 20 ms apart (the core's
 * REJECT_INTERVAL flood guard), so the test waits between handshakes.
 *
 *   5. the send scratch race: two threads encrypt data while a third sends
 *      keepalives through the timer path; every datagram must authenticate
 *      at B and carry an intact payload.
 *   6. initiation storm: A's session expires
 *      (REJECT_AFTER_TIME) while UP and A keeps sending data with the
 *      network blackholed: A goes to HANDSHAKING, at most one initiation
 *      per REKEY_TIMEOUT (5 s, plus the timer's jitter), and once the
 *      network is back the handshake completes and data flows;
 *   7. rekey as responder: B answered A's handshake; with only B sending,
 *      B starts a rekey by itself at REKEY_AFTER_TIME_RECV, exactly one;
 *   8. the same storm check from the responder side (B's responder
 *      keypair expires under steady traffic).
 * Tests 6-8 move the shim's clock (tnshim_host_advance_ns) instead of
 * sleeping; both peers see the same clock.
 *
 * Further cases:
 *   9. roaming: a data packet of B's replayed from another address, a
 *      fresh one from there, and an initiation from there never reach A's
 *      roam sink; a handshake response from a new address does (once),
 *      and one from the old address again reports it (the platform then
 *      drops the roam);
 *  10. a refused configuration (zero peer key, zero private key) leaves
 *      the running peer and its session untouched;
 *  11. control vs data: one thread stops, reconfigures and restarts A
 *      over and over while others encrypt and tick the timers and the
 *      main thread feeds A's input: no crash, and A works afterwards.
 *
 * The Lockdown connect:
 *  12. a lent step never waits for the protocol lock. One thread holds it
 *      the way a control call does (tnp_rehandshake, parked inside the send
 *      sink, i.e. inside the initiation, for 1.3 s). Meanwhile the waiting
 *      entry the lent RX step used before (tnp_input) is stuck behind it -
 *      the hazard - and the watchdog names the holder after 1 s; the
 *      never-waiting entries (tnp_input_try, tnp_timer_tick_try,
 *      tnp_rehandshake_try) return "busy" at once, having done nothing; and
 *      the kept datagram (the peer's response), fed again once the lock is
 *      free, completes the handshake.
 *
 * Throughput:
 *  13. the loss counter of the lent RX step (rx_seq.h) on B's real data
 *      messages: none in order; every 100th dropped = exactly that many
 *      lost; two swapped = one gap and one late. Then a host benchmark:
 *      B seals and A opens N full-size packets through the lent RX entry
 *      (tnp_input_try), printed as us per packet (information only: the
 *      host's portable C, not the Vita's NEON).
 *
 * Keys are fixed test values generated here; nothing real is used.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tn_proto.h"
#include "rx_seq.h"

/* Copy B (renamed by pair.sh). */
void tnpb_set_sinks(tnp_send_fn, tnp_deliver_fn, tnp_roam_fn, void *);
int  tnpb_init(void);
int  tnpb_set_config(const struct tnp_config *);
void tnpb_input(const void *, uint32_t, uint32_t, uint16_t);
int  tnpb_encrypt_send(const void *, uint32_t);
void tnpb_get_stats(struct tnp_stats *);
int  tnpb_timer_tick(void);
void tnpb_start_handshake(void);
void tnpb_rehandshake(void);

void tnshim_host_advance_ns(uint64_t ns);   /* core/shim/tn_shim_host.c */

int curve25519_generate_public(uint8_t pub[32], const uint8_t secret[32]);

#define A_IP   0x0A000001u   /* "underlay" addresses, only labels here */
#define B_IP   0x0A000002u
#define A_TUN  0x0A090002u   /* 10.9.0.2 */
#define B_TUN  0x0A090001u   /* 10.9.0.1 */
#define C_IP   0x0A000003u   /* another underlay address (test 9) */

/* ---- FIFO of datagrams ---------------------------------------------------- */

struct dgram { int to_b; uint32_t len; uint8_t buf[1600]; };
#define QN 16384
static struct dgram g_q[QN];
static unsigned g_qh, g_qt;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_qdrop;

/* Tests 6-8: a virtual clock (ms, moved with the shim's), a blackhole, and
 * the time of every initiation each side sent. */
static uint64_t g_vms;
static int g_block;
#define INIT_LOG 64
static uint64_t g_init_t[2][INIT_LOG];   /* [0] from B, [1] from A */
static unsigned g_init_n[2];

static void
advance_ms(unsigned ms)
{
	tnshim_host_advance_ns((uint64_t)ms * 1000000ULL);
	g_vms += ms;
}

static int
enqueue(int to_b, const void *buf, uint32_t len)
{
	if (len >= 4 && ((const uint8_t *)buf)[0] == 1) {   /* an initiation */
		unsigned *n = &g_init_n[to_b ? 1 : 0];
		if (*n < INIT_LOG)
			g_init_t[to_b ? 1 : 0][*n] = g_vms;
		(*n)++;
	}
	if (g_block)
		return 0;                         /* lost on the way */
	pthread_mutex_lock(&g_qm);
	if (g_qh - g_qt >= QN || len > sizeof(g_q[0].buf)) {
		g_qdrop++;
		pthread_mutex_unlock(&g_qm);
		return -1;
	}
	g_q[g_qh % QN].to_b = to_b;
	g_q[g_qh % QN].len = len;
	memcpy(g_q[g_qh % QN].buf, buf, len);
	g_qh++;
	pthread_mutex_unlock(&g_qm);
	return 0;
}

/* Test 12: A's next initiation parks inside the send sink (the protocol
 * lock held) until g_park is cleared. */
static volatile int g_park, g_parked;
static int send_a(void *c, const void *b, uint32_t l)
{
	(void)c;
	if (g_park && l >= 4 && ((const uint8_t *)b)[0] == 1) {
		g_parked = 1;
		while (g_park)
			usleep(1000);
	}
	return enqueue(1, b, l);
}
/* Test 9: the next B -> A datagram of this type is also kept aside. */
static int g_cap_type;
static struct dgram g_cap;
static int send_b(void *c, const void *b, uint32_t l)
{
	(void)c;
	if (g_cap_type && l >= 4 && ((const uint8_t *)b)[0] == g_cap_type && l <= sizeof(g_cap.buf)) {
		memcpy(g_cap.buf, b, l);
		g_cap.len = l;
		g_cap_type = 0;
	}
	return enqueue(0, b, l);
}
/* A's roam sink records what it was told; B's ignores it. */
static unsigned g_roam_n;
static uint32_t g_roam_ip;
static void roam(void *c, uint32_t ip, uint16_t port) { (void)c; (void)ip; (void)port; }
static void roam_a(void *c, uint32_t ip, uint16_t port) { (void)c; (void)port; g_roam_n++; g_roam_ip = ip; }
static volatile uint32_t g_b_src = B_IP;   /* where A sees B's datagrams come from */

static unsigned g_del_a, g_del_b, g_bad_b;

static void
deliver_a(void *c, const void *ip, uint32_t len) { (void)c; (void)ip; (void)len; g_del_a++; }

/* B checks every payload byte equals the pattern byte at offset 28. */
static void
deliver_b(void *c, const void *ip, uint32_t len)
{
	const uint8_t *p = ip;
	uint32_t i;

	(void)c;
	g_del_b++;
	if (len < 40) { g_bad_b++; return; }
	for (i = 28; i < len; i++)
		if (p[i] != p[28]) { g_bad_b++; return; }
}

static void
pump(void)
{
	static struct dgram d;
	for (;;) {
		pthread_mutex_lock(&g_qm);
		if (g_qt == g_qh) { pthread_mutex_unlock(&g_qm); return; }
		d = g_q[g_qt % QN];
		g_qt++;
		pthread_mutex_unlock(&g_qm);
		if (d.to_b)
			tnpb_input(d.buf, d.len, A_IP, 51000);
		else
			tnp_input(d.buf, d.len, g_b_src, 51820);
	}
}

/* Test 12 counts the watchdog lines that name the holder. */
static volatile unsigned g_wd_named;

void
tnp_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[proto] %s\n", line);
	if (strstr(line, "watchdog:") && strstr(line, "protocol lock") &&
	    strstr(line, "held by tnp_rehandshake"))
		g_wd_named++;
}

/* ---- packets -------------------------------------------------------------- */

static void
wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* An IPv4/UDP-shaped packet from src to dst of len bytes, payload = pat. */
static void
mkpkt(uint8_t *p, uint32_t len, uint32_t src, uint32_t dst, uint8_t pat)
{
	memset(p, pat, len);
	memset(p, 0, 28);
	p[0] = 0x45;
	p[2] = (uint8_t)(len >> 8);
	p[3] = (uint8_t)len;
	p[8] = 64;
	p[9] = 17;
	wr32(p + 12, src);
	wr32(p + 16, dst);
}

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } else { printf("ok   "); printf(__VA_ARGS__); putchar('\n'); } } while (0)

/* ---- the race ------------------------------------------------------------- */

#define PER_THREAD 3000
static volatile int g_go;

static void *
data_thread(void *arg)
{
	uint8_t pat = (uint8_t)(uintptr_t)arg, pkt[1400];
	int i, sent = 0;

	while (!g_go) ;
	for (i = 0; i < PER_THREAD; i++) {
		uint32_t len = 60 + (uint32_t)((i * 37 + pat) % 1300);
		mkpkt(pkt, len, A_TUN, B_TUN, pat);
		if (tnp_encrypt_send(pkt, len) == 0)
			sent++;
	}
	return (void *)(intptr_t)sent;
}

static void *
keepalive_thread(void *arg)
{
	int i;
	(void)arg;
	while (!g_go) ;
	for (i = 0; i < PER_THREAD; i++)
		tnp_encrypt_send(NULL, 0);    /* a keepalive, as the timers send */
	return NULL;
}

/* ---- the initiation storm (6-8) ------------------------------------------ */

/*
 * Side `a` (1 = A, 0 = B) sends a data packet every 10 ms for `ms`
 * milliseconds of virtual time, its timer ticking every 250 ms as on the
 * Vita, and the datagrams pumped after each step. Returns how many of the
 * packets were encrypted.
 */
static unsigned
steady_traffic(int a, unsigned ms)
{
	uint8_t pkt[200];
	unsigned t, ok = 0;

	for (t = 0; t < ms; t += 10) {
		if (a) {
			mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x61);
			ok += tnp_encrypt_send(pkt, sizeof(pkt)) == 0;
		} else {
			mkpkt(pkt, sizeof(pkt), B_TUN, A_TUN, 0x62);
			ok += tnpb_encrypt_send(pkt, sizeof(pkt)) == 0;
		}
		if (t % 250 == 0) {
			if (a)
				tnp_timer_tick();
			else
				tnpb_timer_tick();
		}
		pump();
		advance_ms(10);
	}
	return ok;
}

/* Initiations side `a` sent since index `from`: the count, and the smallest
 * and largest gap between consecutive ones (ms). */
static unsigned
init_gaps(int a, unsigned from, uint64_t *mingap, uint64_t *maxgap)
{
	unsigned i, n = g_init_n[a] > INIT_LOG ? INIT_LOG : g_init_n[a];

	*mingap = ~0ULL;
	*maxgap = 0;
	for (i = from + 1; i < n; i++) {
		uint64_t d = g_init_t[a][i] - g_init_t[a][i - 1];
		if (d < *mingap) *mingap = d;
		if (d > *maxgap) *maxgap = d;
	}
	return g_init_n[a] - from;
}

static void
storm_test(int a, const char *who)
{
	struct tnp_stats st;
	uint64_t mingap, maxgap;
	unsigned from, n, hs0, ok, before;

	if (a) tnp_get_stats(&st); else tnpb_get_stats(&st);
	hs0 = st.handshakes;

	/* The keypair passes REJECT_AFTER_TIME (180 s) with the state UP. */
	advance_ms(181 * 1000);
	g_block = 1;
	from = g_init_n[a];
	ok = steady_traffic(a, 30 * 1000);          /* 3000 packets in 30 s */
	if (a) tnp_get_stats(&st); else tnpb_get_stats(&st);
	n = init_gaps(a, from, &mingap, &maxgap);
	CHECK(ok == 0, "%s expired: no packet encrypted with the dead keypair (%u)", who, ok);
	CHECK(st.state == TNP_HANDSHAKING, "%s expired: state HANDSHAKING (%d), not UP", who, st.state);
	/* 0 s, then every 5 s + jitter (<= 334 ms) on the 250 ms tick: 6 in
	 * 30 s; 3000 without the rate limit. */
	CHECK(n >= 5 && n <= 7, "%s expired: %u initiations for 3000 packets in 30 s", who, n);
	CHECK(mingap >= 5000 && maxgap <= 5000 + 334 + 250 + 10,
	    "%s expired: initiations %llu-%llu ms apart (one per REKEY_TIMEOUT)", who,
	    (unsigned long long)mingap, (unsigned long long)maxgap);

	/* The network comes back: the next retransmit completes it. */
	g_block = 0;
	from = g_init_n[a];
	before = a ? g_del_b : g_del_a;
	ok = steady_traffic(a, 7 * 1000);
	if (a) tnp_get_stats(&st); else tnpb_get_stats(&st);
	n = g_init_n[a] - from;
	CHECK(st.state == TNP_UP && st.handshakes == hs0 + 1,
	    "%s: handshake completed after the outage (state %d, %u -> %u, %u initiation(s))",
	    who, st.state, hs0, st.handshakes, n);
	CHECK(n == 1, "%s: exactly one initiation needed once the network was back (%u)", who, n);
	CHECK(ok > 0 && (a ? g_del_b : g_del_a) - before == ok,
	    "%s: data flows again (%u encrypted, %u delivered)", who, ok,
	    (a ? g_del_b : g_del_a) - before);
}

/* ---- test 11 --------------------------------------------------------------- */

#define RACE_ROUNDS 300
static const struct tnp_config *g_ctl_cfg;
static volatile int g_stop_race;

static void *
control_thread(void *arg)
{
	unsigned i;
	(void)arg;
	for (i = 0; i < RACE_ROUNDS; i++) {
		switch (i % 3) {
		case 0: tnp_stop(); break;
		case 1: tnp_set_config(g_ctl_cfg); break;
		default: tnp_start_handshake(); break;
		}
	}
	g_stop_race = 1;
	return NULL;
}

static void *
race_send_thread(void *arg)
{
	uint8_t pkt[300];
	(void)arg;
	while (!g_stop_race) {
		mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x49);
		tnp_encrypt_send(pkt, sizeof(pkt));
		tnp_encrypt_send(NULL, 0);
		(void)tnp_allowed(B_TUN);
	}
	return NULL;
}

static void *
race_timer_thread(void *arg)
{
	struct tnp_stats st;
	(void)arg;
	while (!g_stop_race) {
		tnp_timer_tick();
		tnp_get_stats(&st);
		tnp_rehandshake();
	}
	return NULL;
}

/* ---- test 12 --------------------------------------------------------------- */

static volatile int g_hold_done, g_wait_done;

static void *
holder_thread(void *arg)
{
	(void)arg;
	tnp_rehandshake();              /* parks in send_a with the lock held */
	g_hold_done = 1;
	return NULL;
}

static void *
waiter_thread(void *arg)
{
	uint8_t junk[64];
	(void)arg;
	memset(junk, 0, sizeof(junk));
	junk[0] = 4;                    /* a data message with an unknown index */
	tnp_input(junk, sizeof(junk), B_IP, 51820);   /* the old, waiting entry */
	g_wait_done = 1;
	return NULL;
}

static uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- main ----------------------------------------------------------------- */

int
main(void)
{
	struct tnp_config ca, cb;
	struct tnp_stats sa, sb;
	uint8_t a_priv[32], b_priv[32], other[32], pkt[200];
	unsigned i, hs0;

	for (i = 0; i < 32; i++) {
		a_priv[i] = (uint8_t)(0x11 * i + 7);
		b_priv[i] = (uint8_t)(0x5d ^ (i * 13));
		other[i] = (uint8_t)(i * 3 + 1);
	}
	memset(&ca, 0, sizeof(ca));
	memset(&cb, 0, sizeof(cb));
	memcpy(ca.private_key, a_priv, 32);
	memcpy(cb.private_key, b_priv, 32);
	curve25519_generate_public(ca.peer_public, b_priv);
	curve25519_generate_public(cb.peer_public, a_priv);
	ca.tunnel_address = A_TUN;
	ca.allowed_count = 1;          /* 0.0.0.0/0 */
	cb.tunnel_address = B_TUN;
	cb.allowed[0].net = A_TUN;
	cb.allowed[0].prefix = 32;
	cb.allowed_count = 1;

	if (tnp_init() || tnpb_init()) { fprintf(stderr, "init failed\n"); return 2; }
	tnp_set_sinks(send_a, deliver_a, roam_a, NULL);
	tnpb_set_sinks(send_b, deliver_b, roam, NULL);
	if (tnp_set_config(&ca) || tnpb_set_config(&cb)) { fprintf(stderr, "config failed\n"); return 2; }

	/* 1. handshake and data */
	tnp_start_handshake();
	pump();
	tnp_get_stats(&sa);
	tnpb_get_stats(&sb);
	CHECK(sa.state == TNP_UP, "A up after the handshake (state %d)", sa.state);
	CHECK(sa.handshakes == 1, "A counted one handshake (%u)", sa.handshakes);
	CHECK(sb.handshakes == 1, "B counted it at the confirming keepalive (%u)", sb.handshakes);
	mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x42);
	CHECK(tnp_encrypt_send(pkt, sizeof(pkt)) == 0, "A encrypts data");
	pump();
	CHECK(g_del_b == 1 && g_bad_b == 0, "B delivered it intact (%u, bad %u)", g_del_b, g_bad_b);
	mkpkt(pkt, sizeof(pkt), B_TUN, A_TUN, 0x43);
	CHECK(tnpb_encrypt_send(pkt, sizeof(pkt)) == 0, "B encrypts data back");
	pump();
	CHECK(g_del_a == 1, "A delivered it (%u)", g_del_a);

	/* 2. rehandshake from UP */
	hs0 = sa.handshakes;
	usleep(60 * 1000);
	tnp_rehandshake();
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_HANDSHAKING, "rehandshake: A handshaking (state %d)", sa.state);
	CHECK(tnp_encrypt_send(pkt, sizeof(pkt)) == 0 || 1, "old session may still carry data");
	pump();
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_UP && sa.handshakes == hs0 + 1,
	    "rehandshake: A up again, counter %u -> %u", hs0, sa.handshakes);
	usleep(60 * 1000);
	tnp_rehandshake();             /* from UP: goes out even soon after the last */
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_HANDSHAKING, "second rehandshake from UP goes out");
	tnp_get_stats(&sb);
	hs0 = sb.handshakes;
	tnp_rehandshake();             /* a repeat while handshaking, within 1 s: ignored */
	pump();
	tnp_get_stats(&sa);
	tnpb_get_stats(&sb);
	CHECK(sa.state == TNP_UP && sb.handshakes == hs0 + 1,
	    "repeat while handshaking was not sent (B saw %u new)", sb.handshakes - hs0);

	/* 3. stop */
	tnp_stop();
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_IDLE, "stop: A idle");
	mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x44);
	CHECK(tnp_encrypt_send(pkt, sizeof(pkt)) < 0, "stop: no session, encrypt refused");
	CHECK(tnp_timer_tick() == TNP_IDLE, "stop: timers do nothing");
	hs0 = sa.handshakes;
	usleep(60 * 1000);
	tnp_start_handshake();
	pump();
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_UP && sa.handshakes == hs0 + 1, "reconnect after stop works");

	/* 4. same private key */
	CHECK(tnp_same_private(a_priv) == 1, "same_private: own key");
	CHECK(tnp_same_private(other) == 0, "same_private: another key");
	CHECK(tnp_same_private(b_priv) == 0, "same_private: the peer's key");

	/* 5. three threads through send_data at once */
	{
		pthread_t t1, t2, t3;
		void *r1, *r2;
		unsigned before = g_del_b, sent;
		pthread_create(&t1, NULL, data_thread, (void *)(uintptr_t)0xA5);
		pthread_create(&t2, NULL, data_thread, (void *)(uintptr_t)0x5A);
		pthread_create(&t3, NULL, keepalive_thread, NULL);
		g_go = 1;
		pthread_join(t1, &r1);
		pthread_join(t2, &r2);
		pthread_join(t3, NULL);
		sent = (unsigned)(intptr_t)r1 + (unsigned)(intptr_t)r2;
		pump();
		CHECK(g_qdrop == 0, "no FIFO overflow (%u)", g_qdrop);
		CHECK(sent == 2 * PER_THREAD, "all %u data packets encrypted", sent);
		CHECK(g_del_b - before == sent && g_bad_b == 0,
		    "B authenticated and delivered %u of %u, %u corrupt", g_del_b - before, sent, g_bad_b);
	}

	/* 5b. The peer goes silent while UP: sending data with nothing
	 * coming back must leave UP within NEW_HANDSHAKE_TIMEOUT (15 s) plus a
	 * tick, not after the keypair expires (180 s) - the quick menu, the
	 * Settings page and the app said "Connected" for minutes before. Then
	 * the retries, then STALLED, and back to UP once the peer answers. */
	{
		struct tnp_stats st;
		unsigned t;
		pump();
		tnp_get_stats(&st);
		CHECK(st.state == TNP_UP, "silent peer: starts UP (%d)", st.state);
		g_block = 1;
		for (t = 0; t < 30; t++) {          /* 30 x 1 s of traffic */
			steady_traffic(1, 1000);
			tnp_get_stats(&st);
			if (st.state != TNP_UP)
				break;
		}
		CHECK(st.state == TNP_HANDSHAKING && t >= 14 && t <= 17,
		    "silent peer: HANDSHAKING after %u s of unanswered data (%d)", t + 1, st.state);
		for (t = 0; t < 120 && st.state == TNP_HANDSHAKING; t++) {
			steady_traffic(1, 1000);
			tnp_get_stats(&st);
		}
		CHECK(st.state == TNP_STALLED && t >= 85 && t <= 100,
		    "silent peer: STALLED %u s after HANDSHAKING (%d)", t, st.state);
		g_block = 0;
		for (t = 0; t < 15 && st.state != TNP_UP; t++) {
			steady_traffic(1, 1000);
			tnp_get_stats(&st);
		}
		CHECK(st.state == TNP_UP, "silent peer: UP again %u s after it answers (%d)", t, st.state);
	}

	/* 6. A (the initiator) expires under steady traffic */
	pump();
	storm_test(1, "A");

	/* 7. rekey as responder. B is a tunnel end too here (wanted, its timers
	 * running): it connects once, then A starts a newer session, so B's
	 * current keypair is one it answered. Only B sends. */
	{
		unsigned from, hs_a, hs_b, ok, before;
		struct tnp_stats sb2;
		advance_ms(6 * 1000);
		tnpb_start_handshake();
		pump();
		advance_ms(6 * 1000);
		tnp_rehandshake();
		pump();
		tnpb_get_stats(&sb2);
		hs_b = sb2.handshakes;
		tnp_get_stats(&sa);
		hs_a = sa.handshakes;
		advance_ms(160 * 1000);                 /* < 165 s: nothing yet */
		from = g_init_n[0];
		steady_traffic(0, 2000);
		CHECK(g_init_n[0] == from, "responder B: no rekey before 165 s (%u)", g_init_n[0] - from);
		advance_ms(4 * 1000);                   /* now 166 s */
		before = g_del_a;
		ok = steady_traffic(0, 3000);
		tnpb_get_stats(&sb2);
		tnp_get_stats(&sa);
		CHECK(g_init_n[0] - from == 1, "responder B: exactly one rekey initiation (%u)",
		    g_init_n[0] - from);
		CHECK(sb2.state == TNP_UP && sb2.handshakes == hs_b + 1 && sa.handshakes == hs_a + 1,
		    "responder B: rekeyed (B %u -> %u, A %u -> %u)", hs_b, sb2.handshakes, hs_a, sa.handshakes);
		CHECK(ok == 300 && g_del_a - before == ok, "responder B: no packet lost (%u of 300 sent, %u delivered)",
		    ok, g_del_a - before);
	}

	/* 8. B as responder expires under steady traffic: A starts a session,
	 * A's keepalive confirms it, then only B sends. */
	advance_ms(6 * 1000);
	tnp_rehandshake();
	pump();
	tnpb_get_stats(&sb);
	tnp_get_stats(&sa);
	CHECK(sa.state == TNP_UP && sb.state == TNP_UP, "A began a new session (A %d, B %d)", sa.state, sb.state);
	storm_test(0, "B");

	/* 9. roaming */
	{
		unsigned r0, before;
		advance_ms(6 * 1000);
		tnp_rehandshake();
		pump();
		tnp_get_stats(&sa);
		CHECK(sa.state == TNP_UP, "roam: A up from B's usual address (state %d)", sa.state);
		CHECK(g_roam_n > 0 && g_roam_ip == B_IP, "roam: a completed response reports its source (%u, 0x%08X)",
		    g_roam_n, (unsigned)g_roam_ip);
		r0 = g_roam_n;
		/* B sends data; the datagram is kept aside, then replayed from C. */
		g_cap_type = 4;
		mkpkt(pkt, sizeof(pkt), B_TUN, A_TUN, 0x45);
		tnpb_encrypt_send(pkt, sizeof(pkt));
		pump();
		before = g_del_a;
		tnp_input(g_cap.buf, g_cap.len, C_IP, 4444);
		CHECK(g_roam_n == r0, "roam: B's data replayed from another address: no roam (%u)", g_roam_n - r0);
		CHECK(g_del_a == before, "roam: the replay was not delivered (replay counter)");
		/* Fresh data from C: decrypted and delivered, no roam. */
		g_b_src = C_IP;
		mkpkt(pkt, sizeof(pkt), B_TUN, A_TUN, 0x46);
		tnpb_encrypt_send(pkt, sizeof(pkt));
		pump();
		CHECK(g_del_a == before + 1, "roam: fresh data from another address still delivered");
		CHECK(g_roam_n == r0, "roam: ... and still no roam");
		/* B initiates from C (A answers as responder): no roam. */
		advance_ms(6 * 1000);
		tnpb_rehandshake();
		pump();
		CHECK(g_roam_n == r0, "roam: an initiation from another address: no roam (%u)", g_roam_n - r0);
		/* A initiates; B's response arrives from C: the one roam. */
		advance_ms(6 * 1000);
		tnp_rehandshake();
		pump();
		tnp_get_stats(&sa);
		CHECK(sa.state == TNP_UP && g_roam_n == r0 + 1 && g_roam_ip == C_IP,
		    "roam: a handshake response from the new address roams (%u, 0x%08X)",
		    g_roam_n - r0, (unsigned)g_roam_ip);
		/* Back at the usual address: reported again (the platform drops the roam). */
		g_b_src = B_IP;
		advance_ms(6 * 1000);
		tnp_rehandshake();
		pump();
		CHECK(g_roam_n == r0 + 2 && g_roam_ip == B_IP, "roam: a response from the old address again reports it");
	}

	/* 10. refused configurations leave the running peer alone */
	{
		struct tnp_config bad = ca;
		unsigned before;
		memset(bad.peer_public, 0, 32);
		CHECK(tnp_set_config(&bad) == -1, "config: zero peer key refused");
		memcpy(bad.peer_public, ca.peer_public, 32);
		memset(bad.private_key, 0, 32);
		CHECK(tnp_set_config(&bad) == -1, "config: zero private key refused");
		tnp_get_stats(&sa);
		before = g_del_b;
		mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x47);
		CHECK(sa.state == TNP_UP && tnp_encrypt_send(pkt, sizeof(pkt)) == 0,
		    "config: after the refusals A is still up and encrypts (state %d)", sa.state);
		pump();
		CHECK(g_del_b == before + 1, "config: ... and B still decrypts it");
		CHECK(tnp_same_private(a_priv) == 1, "config: A's key is the old one");
	}

	/* 11. control calls racing the data path */
	{
		pthread_t tc, td, tt;
		g_ctl_cfg = &ca;
		g_stop_race = 0;
		pthread_create(&tc, NULL, control_thread, NULL);
		pthread_create(&td, NULL, race_send_thread, NULL);
		pthread_create(&tt, NULL, race_timer_thread, NULL);
		while (!g_stop_race)
			pump();
		pthread_join(tc, NULL);
		pthread_join(td, NULL);
		pthread_join(tt, NULL);
		pump();
		advance_ms(6 * 1000);
		tnp_start_handshake();
		pump();
		tnp_get_stats(&sa);
		CHECK(sa.state == TNP_UP, "race: %u reconfigurations under traffic, A up afterwards (state %d)",
		    RACE_ROUNDS, sa.state);
		mkpkt(pkt, sizeof(pkt), A_TUN, B_TUN, 0x48);
		{
			unsigned before = g_del_b;
			tnp_encrypt_send(pkt, sizeof(pkt));
			pump();
			CHECK(g_del_b == before + 1, "race: data flows afterwards");
		}
	}

	/* 12. a lent step never waits for the protocol lock */
	{
		pthread_t th, tw;
		uint8_t junk[64];
		uint64_t t0, dt;
		unsigned hs0b;
		int r1, r2, r3, fed = 0;

		pump();
		advance_ms(6 * 1000);           /* past the rate limit: the rehandshake sends */
		tnp_get_stats(&sa);
		hs0b = sa.handshakes;
		g_park = 1;
		g_parked = 0;
		g_hold_done = g_wait_done = 0;
		g_wd_named = 0;
		pthread_create(&th, NULL, holder_thread, NULL);
		while (!g_parked)
			usleep(1000);
		pthread_create(&tw, NULL, waiter_thread, NULL);

		memset(junk, 0, sizeof(junk));
		junk[0] = 4;
		t0 = mono_ms();
		r1 = tnp_input_try(junk, sizeof(junk), B_IP, 51820);
		r2 = tnp_timer_tick_try();
		r3 = tnp_rehandshake_try();
		dt = mono_ms() - t0;
		CHECK(r1 == -1 && r2 == -1 && r3 == -1,
		    "lock held: input_try %d, timer_tick_try %d, rehandshake_try %d (busy = -1)", r1, r2, r3);
		CHECK(dt < 20, "lock held: the never-waiting entries returned in %llu ms", (unsigned long long)dt);
		{
			uint32_t held = 0;
			const char *h = tnp_lock_holder(&held);
			CHECK(h && strcmp(h, "tnp_rehandshake") == 0, "tnp_lock_holder names the holder (%s)",
			    h ? h : "none");
		}

		usleep(1300 * 1000);            /* the holder keeps it 1.3 s */
		CHECK(!g_wait_done, "lock held 1.3 s: the waiting entry (what the lent RX step used) is stuck behind it");
		CHECK(g_wd_named >= 1, "watchdog: a line after 1 s names the holder (%u)", g_wd_named);

		g_park = 0;                     /* the holder's initiation goes out */
		pthread_join(th, NULL);
		pthread_join(tw, NULL);
		CHECK(g_hold_done && g_wait_done, "released: both threads finished");
		CHECK(tnp_lock_holder(NULL) == NULL, "released: tnp_lock_holder says free");

		/* B answers; A's datagrams are fed only through the try entry,
		 * kept and fed again while busy (as the lent RX step does). */
		for (;;) {
			static struct dgram d;
			pthread_mutex_lock(&g_qm);
			if (g_qt == g_qh) { pthread_mutex_unlock(&g_qm); break; }
			d = g_q[g_qt % QN];
			g_qt++;
			pthread_mutex_unlock(&g_qm);
			if (d.to_b) {
				tnpb_input(d.buf, d.len, A_IP, 51000);
			} else {
				while (tnp_input_try(d.buf, d.len, g_b_src, 51820) != 0)
					usleep(1000);
				fed++;
			}
		}
		pump();
		tnp_get_stats(&sa);
		CHECK(sa.state == TNP_UP && sa.handshakes == hs0b + 1 && fed >= 1,
		    "the response fed through tnp_input_try completes the handshake (%u -> %u, %d fed)",
		    hs0b, sa.handshakes, fed);
		CHECK(tnp_timer_tick_try() == TNP_UP, "lock free: timer_tick_try runs (state UP)");
	}

	/* 13. the RX loss counter and the per-packet cost */
	{
		static uint8_t big[1420];
		struct rx_seq rs;
		static struct dgram d;
		unsigned k, n_in, del0, dropped = 0;
		struct timespec t0, t1;
		double us;

		pump();
		memset(&rs, 0, sizeof(rs));
		mkpkt(big, sizeof(big), B_TUN, A_TUN, 0x5a);
#define POP() ({ int _ok = 0; pthread_mutex_lock(&g_qm); \
		if (g_qt != g_qh) { d = g_q[g_qt % QN]; g_qt++; _ok = 1; } \
		pthread_mutex_unlock(&g_qm); _ok; })
		/* a: in order */
		del0 = g_del_a;
		for (k = 0; k < 2000; k++)
			tnpb_encrypt_send(big, sizeof(big));
		n_in = 0;
		while (POP())
			if (!d.to_b) {
				rx_seq_note(&rs, d.buf, d.len);
				tnp_input_try(d.buf, d.len, B_IP, 51820);
				n_in++;
			}
		CHECK(n_in == 2000 && g_del_a - del0 == 2000 && rs.lost == 0 && rs.late == 0,
		    "rx_seq in order: %u in, %u delivered, lost %u late %u", n_in, g_del_a - del0,
		    rs.lost, rs.late);
		/* b: every 100th lost on the way (a gap shows at the next message
		 * that arrives, so one more is sent after the last drop) */
		for (k = 0; k < 1001; k++)
			tnpb_encrypt_send(big, sizeof(big));
		k = 0;
		while (POP())
			if (!d.to_b && ++k % 100 != 0) {
				rx_seq_note(&rs, d.buf, d.len);
				tnp_input_try(d.buf, d.len, B_IP, 51820);
			} else if (!d.to_b) {
				dropped++;
			}
		CHECK(rs.lost == dropped && rs.late == 0, "rx_seq: %u dropped -> lost %u late %u",
		    dropped, rs.lost, rs.late);
		/* b2: the window - the last drop leaves it only after
		 * RX_SEQ_WIN more; then every drop is "gone", nothing reordered */
		CHECK(rs.gone < dropped, "rx_seq window: the latest drops are not gone yet (%u of %u)",
		    rs.gone, dropped);
		for (k = 0; k < RX_SEQ_WIN + 4; k++)
			tnpb_encrypt_send(big, sizeof(big));
		while (POP())
			if (!d.to_b) {
				rx_seq_note(&rs, d.buf, d.len);
				tnp_input_try(d.buf, d.len, B_IP, 51820);
			}
		CHECK(rs.gone == dropped && rs.filled == 0 && rs.dup == 0,
		    "rx_seq window: %u dropped -> gone %u fill %u dup %u", dropped, rs.gone, rs.filled, rs.dup);
		/* c: two swapped */
		{
			static struct dgram first;
			unsigned l0 = rs.lost, t0l = rs.late;
			tnpb_encrypt_send(big, sizeof(big));
			tnpb_encrypt_send(big, sizeof(big));
			CHECK(POP() && !d.to_b, "rx_seq swap: first datagram");
			first = d;
			CHECK(POP() && !d.to_b, "rx_seq swap: second datagram");
			rx_seq_note(&rs, d.buf, d.len);
			rx_seq_note(&rs, first.buf, first.len);
			CHECK(rs.lost - l0 == 1 && rs.late - t0l == 1, "rx_seq swapped pair: lost +%u late +%u",
			    rs.lost - l0, rs.late - t0l);
			/* a swap is a fill at depth 1, never gone; the
			 * same datagram again is a dup */
			{
				unsigned g0 = rs.gone, f0 = rs.filled, d0 = rs.dup, h0 = rs.depth[0];
				rx_seq_note(&rs, first.buf, first.len);
				CHECK(rs.dup - d0 == 1 && rs.filled == f0, "rx_seq duplicate: dup +%u",
				    rs.dup - d0);
				for (k = 0; k < RX_SEQ_WIN + 4; k++)
					tnpb_encrypt_send(big, sizeof(big));
				while (POP())
					if (!d.to_b)
						rx_seq_note(&rs, d.buf, d.len);
				CHECK(rs.gone == g0 && f0 >= 1 && h0 >= 1 && rs.depth_max >= 1,
				    "rx_seq swap: gone +%u (fill %u, depth1 %u, max %u)",
				    rs.gone - g0, f0, h0, rs.depth_max);
			}
		}
		/* bench: seal at B + open and deliver at A, full-size packets */
		del0 = g_del_a;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (k = 0; k < 20000; k++) {
			tnpb_encrypt_send(big, sizeof(big));
			while (POP())
				if (!d.to_b)
					tnp_input_try(d.buf, d.len, B_IP, 51820);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		us = (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
		CHECK(g_del_a - del0 == 20000, "bench: 20000 packets of 1420 bytes delivered (%u)",
		    g_del_a - del0);
		printf("info bench (host, portable C): %.2f us per packet seal+open, %.0f Mbit/s\n",
		    us / 20000.0, 20000.0 * 1420 * 8 / us);
#undef POP
	}

	tnp_fini();
	printf("%s\n", g_fail ? "PAIR TEST FAILED" : "PAIR TEST PASSED");
	return g_fail ? 1 : 0;
}
