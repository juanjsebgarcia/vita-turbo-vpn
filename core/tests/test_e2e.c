/*
 * test_e2e.c - two peers in one process, talking through the wire formats
 * of tn_core.h: handshake with and without a preshared key, transport data
 * both ways, replay and counter handling, the rekey and reject timers, key
 * rotation, and cookie replies and rate limiting under load.
 *
 * Peer a plays the console (initiator), peer b the server. The clock is
 * moved with tnshim_host_advance_ns instead of sleeping.
 */
#include <tn_core.h>

#include <pthread.h>

#include "harness.h"

#define SEC	1000000000ULL

struct peer {
	const char		*name;
	struct noise_local	 l;
	struct noise_remote	 r;
	struct cookie_maker	 cm;	/* keyed with the other side's public key */
	struct cookie_checker	 cc;	/* keyed with our own */
	struct pool		 pool;
	uint8_t			 priv[NOISE_PUBLIC_KEY_LEN];
	uint8_t			 pub[NOISE_PUBLIC_KEY_LEN];
	struct sockaddr_in	 sin;	/* our address, as the other side sees it */
	uint32_t		 idx[8];	/* live local indices (0 = free) */
	int			 nidx;
};

static struct peer pa, pb;

/* ---- upcalls (the interface layer's job on the console) ----------------- */

static struct noise_remote *
up_remote_get(void *arg, uint8_t pub[NOISE_PUBLIC_KEY_LEN])
{
	struct peer *p = arg;

	return memcmp(p->r.r_public, pub, NOISE_PUBLIC_KEY_LEN) == 0 ?
	    &p->r : NULL;
}

static uint32_t
up_index_set(void *arg, struct noise_remote *r)
{
	struct peer *p = arg;
	uint32_t v;
	int i, dup;

	(void)r;
	do {
		v = arc4random();
		dup = (v == 0);
		for (i = 0; i < 8; i++)
			dup |= (p->idx[i] == v);
	} while (dup);
	for (i = 0; i < 8; i++) {
		if (p->idx[i] == 0) {
			p->idx[i] = v;
			p->nidx++;
			return v;
		}
	}
	fprintf(stderr, "%s: index table full\n", p->name);
	return v;
}

static void
up_index_drop(void *arg, uint32_t v)
{
	struct peer *p = arg;
	int i;

	for (i = 0; i < 8; i++) {
		if (p->idx[i] == v) {
			p->idx[i] = 0;
			p->nidx--;
		}
	}
}

static int
index_known(struct peer *p, uint32_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		if (v != 0 && p->idx[i] == v)
			return 1;
	return 0;
}

/* ---- set-up ------------------------------------------------------------- */

static void
peer_init(struct peer *p, const char *name, uint32_t addr, uint16_t port)
{
	struct noise_upcall up = {
		.u_arg = p,
		.u_remote_get = up_remote_get,
		.u_index_set = up_index_set,
		.u_index_drop = up_index_drop,
	};

	memset(p, 0, sizeof(*p));
	p->name = name;
	noise_local_init(&p->l, &up);
	arc4random_buf(p->priv, sizeof(p->priv));
	noise_local_lock_identity(&p->l);
	noise_local_set_private(&p->l, p->priv);
	noise_local_unlock_identity(&p->l);
	noise_local_keys(&p->l, p->pub, NULL);

	pool_init(&p->pool, sizeof(struct ratelimit_entry), 0, IPL_NET, 0,
	    "ratelimit", NULL);
	cookie_checker_init(&p->cc, &p->pool);
	cookie_checker_update(&p->cc, p->pub);

	p->sin.sin_len = sizeof(p->sin);
	p->sin.sin_family = AF_INET;
	p->sin.sin_addr.s_addr = addr;
	p->sin.sin_port = port;
}

/* Each side learns the other's public key, as from a profile. */
static void
peers_pair(const uint8_t *psk_a, const uint8_t *psk_b)
{
	noise_remote_init(&pa.r, pb.pub, &pa.l);
	noise_remote_init(&pb.r, pa.pub, &pb.l);
	cookie_maker_init(&pa.cm, pb.pub);
	cookie_maker_init(&pb.cm, pa.pub);
	if (psk_a != NULL)
		noise_remote_set_psk(&pa.r, (uint8_t *)psk_a);
	if (psk_b != NULL)
		noise_remote_set_psk(&pb.r, (uint8_t *)psk_b);
}

static void
peer_fini(struct peer *p)
{
	noise_remote_clear(&p->r);
	cookie_checker_deinit(&p->cc);
	pool_destroy(&p->pool);
}

static void
peers_reset(const uint8_t *psk_a, const uint8_t *psk_b)
{
	static uint32_t n;

	n++;
	if (pa.name != NULL) {
		peer_fini(&pa);
		peer_fini(&pb);
	}
	peer_init(&pa, "a", htobe32(0x0a000002), htobe16(40000 + n));
	peer_init(&pb, "b", htobe32(0xc0a80101), htobe16(51820));
	peers_pair(psk_a, psk_b);
}

/* ---- messages ----------------------------------------------------------- */

static int
make_initiation(struct peer *p, struct tn_pkt_initiation *pkt)
{
	int ret;

	memset(pkt, 0, sizeof(*pkt));
	ret = noise_create_initiation(&p->r, &pkt->s_idx, pkt->ue, pkt->es,
	    pkt->ets);
	pkt->t = TN_PKT_INITIATION;
	cookie_maker_mac(&p->cm, &pkt->m, pkt, sizeof(*pkt) - sizeof(pkt->m));
	return ret;
}

/*
 * As if_wg.c's tn_handshake does: MACs first; on EAGAIN a cookie reply is
 * built into *ck. Returns the first non-zero result.
 */
static int
take_initiation(struct peer *p, struct tn_pkt_initiation *pkt, int busy,
    struct sockaddr_in *from, struct tn_pkt_cookie *ck)
{
	struct noise_remote *r = NULL;
	int ret;

	ret = cookie_checker_validate_macs(&p->cc, &pkt->m, pkt,
	    sizeof(*pkt) - sizeof(pkt->m), busy, sintosa(from));
	if (ret == EAGAIN && ck != NULL) {
		ck->t = TN_PKT_COOKIE;
		ck->r_idx = pkt->s_idx;
		cookie_checker_create_payload(&p->cc, &pkt->m, ck->nonce,
		    ck->ec, sintosa(from));
	}
	if (ret != 0)
		return ret;
	ret = noise_consume_initiation(&p->l, &r, pkt->s_idx, pkt->ue,
	    pkt->es, pkt->ets);
	if (ret == 0 && r != &p->r)
		return -1;
	return ret;
}

static int
make_response(struct peer *p, struct tn_pkt_response *pkt)
{
	int ret;

	memset(pkt, 0, sizeof(*pkt));
	ret = noise_create_response(&p->r, &pkt->s_idx, &pkt->r_idx, pkt->ue,
	    pkt->en);
	if (ret != 0)
		return ret;
	ret = noise_remote_begin_session(&p->r);
	pkt->t = TN_PKT_RESPONSE;
	cookie_maker_mac(&p->cm, &pkt->m, pkt, sizeof(*pkt) - sizeof(pkt->m));
	return ret;
}

static int
take_response(struct peer *p, struct tn_pkt_response *pkt, int busy,
    struct sockaddr_in *from)
{
	int ret;

	ret = cookie_checker_validate_macs(&p->cc, &pkt->m, pkt,
	    sizeof(*pkt) - sizeof(pkt->m), busy, sintosa(from));
	if (ret != 0)
		return ret;
	if (!index_known(p, pkt->r_idx))
		return ENOENT;
	ret = noise_consume_response(&p->r, pkt->s_idx, pkt->r_idx, pkt->ue,
	    pkt->en);
	if (ret != 0)
		return ret;
	return noise_remote_begin_session(&p->r);
}

/* Full handshake a -> b -> a; 0 on success. */
static int
handshake(void)
{
	struct tn_pkt_initiation init;
	struct tn_pkt_response resp;

	if (make_initiation(&pa, &init) != 0)
		return 1;
	if (take_initiation(&pb, &init, 0, &pa.sin, NULL) != 0)
		return 2;
	if (make_response(&pb, &resp) != 0)
		return 3;
	if (take_response(&pa, &resp, 0, &pb.sin) != 0)
		return 4;
	return 0;
}

/* struct tn_pkt_data with room for the body after the header. */
struct datagram {
	struct {
		uint32_t	t;
		uint32_t	r_idx;
		uint8_t		nonce[sizeof(uint64_t)];
	}			hdr;
	uint8_t			body[256 + NOISE_AUTHTAG_LEN];
	size_t			len;	/* plaintext length */
};
_Static_assert(sizeof(((struct datagram *)0)->hdr) ==
    sizeof(struct tn_pkt_data), "datagram header matches tn_pkt_data");

static int
seal(struct peer *p, const void *msg, size_t len, struct datagram *d)
{
	uint64_t nonce;
	int ret;

	memset(d, 0, sizeof(*d));
	memcpy(d->body, msg, len);
	d->len = len;
	d->hdr.t = TN_PKT_DATA;
	ret = noise_remote_encrypt(&p->r, &d->hdr.r_idx, &nonce, d->body, len);
	htolem64(d->hdr.nonce, nonce);
	return ret;
}

/* Decrypts a copy, so a datagram can be replayed. */
static int
open_dgram(struct peer *p, const struct datagram *d, uint8_t *out)
{
	struct datagram c = *d;
	int ret;

	ret = noise_remote_decrypt(&p->r, c.hdr.r_idx,
	    lemtoh64(c.hdr.nonce), c.body, c.len + NOISE_AUTHTAG_LEN);
	if (out != NULL && (ret == 0 || ret == ECONNRESET || ret == ESTALE))
		memcpy(out, c.body, c.len);
	return ret;
}

static uint64_t
dgram_nonce(const struct datagram *d)
{
	return lemtoh64(d->hdr.nonce);
}

/* ---- tests -------------------------------------------------------------- */

static void
test_constants(void)
{
	/* The protocol constants. */
	CHECK_EQ(REKEY_AFTER_TIME, 120, "REKEY_AFTER_TIME");
	CHECK_EQ(REKEY_AFTER_TIME_RECV, 165, "REKEY_AFTER_TIME_RECV");
	CHECK_EQ(REJECT_AFTER_TIME, 180, "REJECT_AFTER_TIME");
	CHECK_EQ(REKEY_TIMEOUT, 5, "REKEY_TIMEOUT");
	CHECK_EQ(REKEY_TIMEOUT_JITTER, 334, "REKEY_TIMEOUT_JITTER");
	CHECK_EQ(KEEPALIVE_TIMEOUT, 10, "KEEPALIVE_TIMEOUT");
	CHECK_EQ(MAX_TIMER_HANDSHAKES, 18, "MAX_TIMER_HANDSHAKES");
	CHECK_EQ(NEW_HANDSHAKE_TIMEOUT, 15, "NEW_HANDSHAKE_TIMEOUT");
	CHECK_EQ(COOKIE_SECRET_MAX_AGE, 120, "COOKIE_SECRET_MAX_AGE");
	CHECK_EQ(REKEY_AFTER_MESSAGES, 1ULL << 60, "REKEY_AFTER_MESSAGES");
	CHECK(REJECT_AFTER_MESSAGES == UINT64_MAX - COUNTER_WINDOW_SIZE - 1,
	    "REJECT_AFTER_MESSAGES");
	CHECK_EQ(sizeof(struct tn_pkt_initiation), 148, "initiation size");
	CHECK_EQ(sizeof(struct tn_pkt_response), 92, "response size");
	CHECK_EQ(sizeof(struct tn_pkt_cookie), 64, "cookie size");
}

static void
test_handshake_and_data(const char *label, int use_psk)
{
	uint8_t psk[NOISE_SYMMETRIC_KEY_LEN], got[256], msg[200], tag[64];
	struct datagram d;
	size_t i;
	int ret;

	arc4random_buf(psk, sizeof(psk));
	peers_reset(use_psk ? psk : NULL, use_psk ? psk : NULL);
	snprintf((char *)tag, sizeof(tag), "%s: handshake", label);
	CHECK_EQ(handshake(), 0, (char *)tag);

	CHECK_EQ(noise_remote_ready(&pa.r), 0, "initiator ready");
	/* The responder may not send until the initiator has confirmed. */
	CHECK_EQ(noise_remote_ready(&pb.r), EINVAL, "responder not ready");
	CHECK_EQ(seal(&pb, "x", 1, &d), EINVAL, "responder waits");

	for (i = 0; i < sizeof(msg); i++)
		msg[i] = (uint8_t)(i * 3);
	CHECK_EQ(seal(&pa, msg, sizeof(msg), &d), 0, "a seals");
	CHECK_EQ(dgram_nonce(&d), 0, "first nonce is 0");
	CHECK(memcmp(d.body, msg, sizeof(msg)) != 0, "body is encrypted");
	ret = open_dgram(&pb, &d, got);
	CHECK_EQ(ret, ECONNRESET, "b's first packet confirms the session");
	CHECK_MEM(got, msg, sizeof(msg), "a -> b payload");

	CHECK_EQ(noise_remote_ready(&pb.r), 0, "responder ready");
	CHECK_EQ(seal(&pb, msg, 64, &d), 0, "b seals");
	CHECK_EQ(open_dgram(&pa, &d, got), 0, "a opens");
	CHECK_MEM(got, msg, 64, "b -> a payload");

	/* A keepalive is an empty payload. */
	CHECK_EQ(seal(&pa, NULL, 0, &d), 0, "keepalive seals");
	CHECK_EQ(open_dgram(&pb, &d, NULL), 0, "keepalive opens");

	/* Tampering and wrong index. */
	CHECK_EQ(seal(&pa, msg, 32, &d), 0, "seal for tamper");
	d.body[5] ^= 1;
	CHECK_EQ(open_dgram(&pb, &d, NULL), EINVAL, "tampered body rejected");
	d.body[5] ^= 1;
	d.hdr.r_idx ^= 1;
	CHECK_EQ(open_dgram(&pb, &d, NULL), EINVAL, "unknown index rejected");
	d.hdr.r_idx ^= 1;
	CHECK_EQ(open_dgram(&pb, &d, NULL), 0, "untampered copy accepted");
}

static void
test_psk_mismatch(void)
{
	uint8_t psk1[32], psk2[32];
	struct tn_pkt_initiation init;
	struct tn_pkt_response resp;

	arc4random_buf(psk1, sizeof(psk1));
	memcpy(psk2, psk1, sizeof(psk2));
	psk2[0] ^= 1;
	peers_reset(psk1, psk2);
	CHECK_EQ(make_initiation(&pa, &init), 0, "psk mismatch: initiation");
	/* The PSK only enters at the response. */
	CHECK_EQ(take_initiation(&pb, &init, 0, &pa.sin, NULL), 0,
	    "psk mismatch: initiation still accepted");
	CHECK_EQ(make_response(&pb, &resp), 0, "psk mismatch: response");
	CHECK_EQ(take_response(&pa, &resp, 0, &pb.sin), EINVAL,
	    "psk mismatch: response rejected");
	CHECK_EQ(noise_remote_ready(&pa.r), EINVAL, "psk mismatch: no session");

	/* PSK on one side only fails the same way. */
	peers_reset(psk1, NULL);
	CHECK(handshake() != 0, "psk on one side only fails");
}

static void
test_unknown_peer(void)
{
	struct tn_pkt_initiation init;
	struct peer stranger;
	struct noise_upcall up = {
		.u_arg = &stranger, .u_remote_get = up_remote_get,
		.u_index_set = up_index_set, .u_index_drop = up_index_drop,
	};

	peers_reset(NULL, NULL);
	/* A third key that b has never heard of, with b's public key. */
	memset(&stranger, 0, sizeof(stranger));
	stranger.name = "stranger";
	noise_local_init(&stranger.l, &up);
	arc4random_buf(stranger.priv, 32);
	noise_local_lock_identity(&stranger.l);
	noise_local_set_private(&stranger.l, stranger.priv);
	noise_local_unlock_identity(&stranger.l);
	noise_remote_init(&stranger.r, pb.pub, &stranger.l);
	cookie_maker_init(&stranger.cm, pb.pub);

	CHECK_EQ(make_initiation(&stranger, &init), 0, "stranger initiates");
	CHECK_EQ(take_initiation(&pb, &init, 0, &pa.sin, NULL), EINVAL,
	    "unknown static key rejected");

	/* mac1 keyed with the wrong public key fails before any DH. */
	cookie_maker_init(&stranger.cm, pa.pub);
	make_initiation(&stranger, &init);
	CHECK_EQ(cookie_checker_validate_macs(&pb.cc, &init.m, &init,
	    sizeof(init) - sizeof(init.m), 0, sintosa(&pa.sin)), EINVAL,
	    "bad mac1 rejected");
	noise_remote_clear(&stranger.r);
}

static void
test_initiation_replay(void)
{
	struct tn_pkt_initiation old, cur;

	peers_reset(NULL, NULL);
	CHECK_EQ(make_initiation(&pa, &old), 0, "replay: first initiation");
	tnshim_host_advance_ns(SEC);
	CHECK_EQ(make_initiation(&pa, &cur), 0, "replay: second initiation");
	CHECK_EQ(take_initiation(&pb, &cur, 0, &pa.sin, NULL), 0,
	    "replay: newer accepted");
	tnshim_host_advance_ns(SEC);
	CHECK_EQ(take_initiation(&pb, &cur, 0, &pa.sin, NULL), EINVAL,
	    "replay: same initiation again rejected");
	CHECK_EQ(take_initiation(&pb, &old, 0, &pa.sin, NULL), EINVAL,
	    "replay: older timestamp rejected");

	/* Flood guard: a newer initiation within REJECT_INTERVAL (20 ms) of
	 * the last accepted one is dropped. */
	{
		struct tn_pkt_initiation i1, i2, i3;

		tnshim_host_advance_ns(SEC);
		CHECK_EQ(make_initiation(&pa, &i1), 0, "flood: initiation 1");
		tnshim_host_advance_ns(SEC);
		CHECK_EQ(make_initiation(&pa, &i2), 0, "flood: initiation 2");
		CHECK_EQ(take_initiation(&pb, &i1, 0, &pa.sin, NULL), 0,
		    "flood: 1 accepted");
		CHECK_EQ(take_initiation(&pb, &i2, 0, &pa.sin, NULL), EINVAL,
		    "flood: 2 right after is dropped");
		tnshim_host_advance_ns(2 * REJECT_INTERVAL);
		CHECK_EQ(make_initiation(&pa, &i3), 0, "flood: initiation 3");
		CHECK_EQ(take_initiation(&pb, &i3, 0, &pa.sin, NULL), 0,
		    "flood: 3 after the interval accepted");
	}
}

static void
test_replay_window(void)
{
	static struct datagram first, mid, edge, last, d;
	uint64_t n, total = COUNTER_WINDOW_SIZE + 100;
	int bad = 0;

	peers_reset(NULL, NULL);
	CHECK_EQ(handshake(), 0, "window: handshake");
	seal(&pa, "go", 2, &d);
	open_dgram(&pb, &d, NULL);		/* confirms; nonce 0 */

	/* Out of order inside the window is fine, duplicates are not. */
	{
		struct datagram x1, x2, x3;
		seal(&pa, "1", 1, &x1);
		seal(&pa, "2", 1, &x2);
		seal(&pa, "3", 1, &x3);
		CHECK(dgram_nonce(&x1) == 1 && dgram_nonce(&x3) == 3,
		    "nonces count up");
		CHECK_EQ(open_dgram(&pb, &x3, NULL), 0, "window: 3 first");
		CHECK_EQ(open_dgram(&pb, &x1, NULL), 0, "window: then 1");
		CHECK_EQ(open_dgram(&pb, &x2, NULL), 0, "window: then 2");
		CHECK_EQ(open_dgram(&pb, &x2, NULL), EINVAL,
		    "window: 2 replayed rejected");
		CHECK_EQ(open_dgram(&pb, &x3, NULL), EINVAL,
		    "window: 3 replayed rejected");
	}

	/* Run the counter past a whole window, keeping a few. */
	for (n = 4; n < total; n++) {
		seal(&pa, "w", 1, &d);
		if (dgram_nonce(&d) != n)
			bad++;
		if (n == 4)
			first = d;
		else if (n == total / 2)
			mid = d;
		else if (n == total - COUNTER_WINDOW_SIZE)
			edge = d;
		else if (n == total - 1)
			last = d;
	}
	CHECK_EQ(bad, 0, "send counter strictly sequential");
	CHECK_EQ(open_dgram(&pb, &last, NULL), 0, "window: newest accepted");
	CHECK_EQ(open_dgram(&pb, &first, NULL), EINVAL,
	    "window: older than the window rejected");
	CHECK_EQ(open_dgram(&pb, &edge, NULL), 0,
	    "window: oldest still inside accepted");
	CHECK_EQ(open_dgram(&pb, &mid, NULL), 0, "window: middle accepted");
	CHECK_EQ(open_dgram(&pb, &mid, NULL), EINVAL, "window: middle replay");
}

static void
test_timers(void)
{
	struct datagram d, from_b_old;
	struct tn_pkt_initiation init;
	struct tn_pkt_response resp;
	uint8_t got[8];

	peers_reset(NULL, NULL);
	CHECK_EQ(handshake(), 0, "timers: handshake");
	seal(&pa, "go", 2, &d);
	open_dgram(&pb, &d, NULL);

	tnshim_host_advance_ns((REKEY_AFTER_TIME - 1) * SEC);
	CHECK_EQ(seal(&pa, "x", 1, &d), 0, "timers: fresh before 120 s");
	tnshim_host_advance_ns(2 * SEC);
	/* Past REKEY_AFTER_TIME the initiator is told to rekey but may still
	 * send; the responder is not. */
	CHECK_EQ(seal(&pa, "x", 1, &d), ESTALE, "timers: initiator stale");
	CHECK_EQ(open_dgram(&pb, &d, NULL), 0, "timers: stale still opens");
	CHECK_EQ(seal(&pb, "y", 1, &d), 0, "timers: responder not stale");
	CHECK_EQ(open_dgram(&pa, &d, NULL), 0, "timers: a opens before 165 s");

	tnshim_host_advance_ns((REKEY_AFTER_TIME_RECV - REKEY_AFTER_TIME) * SEC);
	CHECK_EQ(seal(&pb, "y", 1, &d), 0, "timers: b seals at 166 s");
	CHECK_EQ(open_dgram(&pa, &d, NULL), ESTALE,
	    "timers: initiator stale on receive past 165 s");

	tnshim_host_advance_ns((REJECT_AFTER_TIME - REKEY_AFTER_TIME_RECV) * SEC);
	CHECK_EQ(seal(&pa, "x", 1, &d), EINVAL, "timers: rejected past 180 s");
	CHECK_EQ(noise_remote_ready(&pa.r), EINVAL, "timers: not ready");

	/* Rekey and rotation: b keeps sending on the old keys until a's
	 * first packet on the new ones arrives. */
	peers_reset(NULL, NULL);
	CHECK_EQ(handshake(), 0, "rotate: handshake 1");
	seal(&pa, "go", 2, &d);
	open_dgram(&pb, &d, NULL);
	tnshim_host_advance_ns((REKEY_AFTER_TIME + 1) * SEC);
	CHECK_EQ(seal(&pb, "old", 3, &from_b_old), 0, "rotate: b on old keys");

	CHECK_EQ(make_initiation(&pa, &init), 0, "rotate: initiation 2");
	CHECK_EQ(take_initiation(&pb, &init, 0, &pa.sin, NULL), 0,
	    "rotate: b takes initiation 2");
	CHECK_EQ(make_response(&pb, &resp), 0, "rotate: response 2");
	CHECK_EQ(seal(&pb, "old", 3, &d), 0, "rotate: b still on old keys");
	CHECK_EQ(take_response(&pa, &resp, 0, &pb.sin), 0, "rotate: a done");
	CHECK_EQ(open_dgram(&pa, &from_b_old, got), 0,
	    "rotate: a opens in-flight old-key packet");
	CHECK_EQ(open_dgram(&pa, &d, NULL), 0, "rotate: and the next one");
	CHECK_EQ(seal(&pa, "new", 3, &d), 0, "rotate: a on new keys");
	CHECK_EQ(open_dgram(&pb, &d, got), ECONNRESET,
	    "rotate: b switches on a's first new-key packet");
	CHECK_EQ(seal(&pb, "new", 3, &d), 0, "rotate: b on new keys");
	CHECK_EQ(open_dgram(&pa, &d, NULL), 0, "rotate: a opens new-key packet");

	/* A third session pushes the first keys out entirely. Its
	 * timestamp must be newer than the second's. */
	tnshim_host_advance_ns(SEC);
	CHECK_EQ(handshake(), 0, "rotate: handshake 3");
	seal(&pa, "3", 1, &d);
	CHECK_EQ(open_dgram(&pb, &d, NULL), ECONNRESET, "rotate: b on keys 3");
	CHECK_EQ(open_dgram(&pa, &from_b_old, NULL), EINVAL,
	    "rotate: keys 1 gone");

	/* Clearing drops every index through the upcall. */
	noise_remote_clear(&pa.r);
	noise_remote_clear(&pb.r);
	CHECK_EQ(pa.nidx, 0, "clear: a's indices dropped");
	CHECK_EQ(pb.nidx, 0, "clear: b's indices dropped");
	CHECK_EQ(seal(&pa, "x", 1, &d), EINVAL, "clear: nothing to send on");
}

static void
test_cookies(void)
{
	struct tn_pkt_initiation init;
	struct tn_pkt_response resp;
	struct tn_pkt_cookie ck;
	struct sockaddr_in spoof;
	static const uint8_t zero[COOKIE_MAC_SIZE];
	int i, ok;

	peers_reset(NULL, NULL);
	CHECK_EQ(make_initiation(&pa, &init), 0, "cookie: initiation");
	CHECK_MEM(init.m.mac2, zero, COOKIE_MAC_SIZE, "cookie: no mac2 yet");

	/* Under load, b answers with a cookie instead of a response. */
	memset(&ck, 0, sizeof(ck));
	CHECK_EQ(take_initiation(&pb, &init, 1, &pa.sin, &ck), EAGAIN,
	    "cookie: busy b sends a cookie");
	CHECK(ck.t == TN_PKT_COOKIE && ck.r_idx == init.s_idx,
	    "cookie: reply addressed to a's index");
	CHECK(index_known(&pa, ck.r_idx), "cookie: a knows the index");
	ck.ec[0] ^= 1;
	CHECK_EQ(cookie_maker_consume_payload(&pa.cm, ck.nonce, ck.ec), EINVAL,
	    "cookie: tampered reply rejected");
	ck.ec[0] ^= 1;
	CHECK_EQ(cookie_maker_consume_payload(&pa.cm, ck.nonce, ck.ec), 0,
	    "cookie: a takes the cookie");
	CHECK_EQ(cookie_maker_consume_payload(&pa.cm, ck.nonce, ck.ec),
	    ETIMEDOUT, "cookie: reply replay rejected");

	/* The retry carries mac2 and gets through. */
	tnshim_host_advance_ns(SEC);
	CHECK_EQ(make_initiation(&pa, &init), 0, "cookie: retry");
	CHECK(memcmp(init.m.mac2, zero, COOKIE_MAC_SIZE) != 0,
	    "cookie: retry has mac2");
	spoof = pa.sin;
	spoof.sin_addr.s_addr ^= htobe32(1);
	CHECK_EQ(take_initiation(&pb, &init, 1, &spoof, NULL), EAGAIN,
	    "cookie: mac2 bound to the source address");
	spoof = pa.sin;
	spoof.sin_port ^= 1;
	CHECK_EQ(take_initiation(&pb, &init, 1, &spoof, NULL), EAGAIN,
	    "cookie: mac2 bound to the source port");
	CHECK_EQ(take_initiation(&pb, &init, 1, &pa.sin, NULL), 0,
	    "cookie: busy b accepts with mac2");
	CHECK_EQ(make_response(&pb, &resp), 0, "cookie: response");
	CHECK_EQ(take_response(&pa, &resp, 0, &pb.sin), 0, "cookie: session");

	/* Rate limit per source: INITIATIONS_BURSTABLE at once, then
	 * INITIATIONS_PER_SECOND. The first call above used one token. */
	ok = 0;
	for (i = 0; i < 2 * INITIATIONS_BURSTABLE; i++)
		ok += cookie_checker_validate_macs(&pb.cc, &init.m, &init,
		    sizeof(init) - sizeof(init.m), 1, sintosa(&pa.sin)) == 0;
	CHECK_EQ(ok, INITIATIONS_BURSTABLE - 1, "ratelimit: burst then refuse");
	CHECK_EQ(cookie_checker_validate_macs(&pb.cc, &init.m, &init,
	    sizeof(init) - sizeof(init.m), 1, sintosa(&pa.sin)), ECONNREFUSED,
	    "ratelimit: refused");
	tnshim_host_advance_ns(INITIATION_COST);
	CHECK_EQ(cookie_checker_validate_macs(&pb.cc, &init.m, &init,
	    sizeof(init) - sizeof(init.m), 1, sintosa(&pa.sin)), 0,
	    "ratelimit: one more after 1/20 s");
	/* Not busy: no rate limit at all. */
	CHECK_EQ(cookie_checker_validate_macs(&pb.cc, &init.m, &init,
	    sizeof(init) - sizeof(init.m), 0, sintosa(&pa.sin)), 0,
	    "ratelimit: off when not busy");

	/* a stops sending mac2 once the cookie is older than 115 s. */
	tnshim_host_advance_ns((COOKIE_SECRET_MAX_AGE - COOKIE_SECRET_LATENCY
	    + 1) * SEC);
	make_initiation(&pa, &init);
	CHECK_MEM(init.m.mac2, zero, COOKIE_MAC_SIZE, "cookie: expires at a");

	/* b's secret rotates after 120 s, so an old cookie stops working. */
	peers_reset(NULL, NULL);
	make_initiation(&pa, &init);
	take_initiation(&pb, &init, 1, &pa.sin, &ck);
	cookie_maker_consume_payload(&pa.cm, ck.nonce, ck.ec);
	tnshim_host_advance_ns((COOKIE_SECRET_MAX_AGE + 1) * SEC);
	/* Keep a's copy fresh enough to send by refreshing its birthdate. */
	getnanouptime(&pa.cm.cp_birthdate);
	make_initiation(&pa, &init);
	CHECK(memcmp(init.m.mac2, zero, COOKIE_MAC_SIZE) != 0,
	    "cookie: a still sends the old cookie");
	CHECK_EQ(take_initiation(&pb, &init, 1, &pa.sin, NULL), EAGAIN,
	    "cookie: rotated secret refuses the old cookie");
}

/* ---- both directions at once, from two threads -------------------------- */

#define THREAD_PKTS	20000

struct dir {
	struct peer	*tx, *rx;
	int		 errors;
};

static void *
pump(void *arg)
{
	struct dir *dir = arg;
	struct datagram d;
	uint8_t msg[128], got[128];
	int i, ret;

	for (i = 0; i < THREAD_PKTS; i++) {
		memset(msg, i, sizeof(msg));
		if (seal(dir->tx, msg, sizeof(msg), &d) != 0) {
			dir->errors++;
			continue;
		}
		ret = open_dgram(dir->rx, &d, got);
		if (ret != 0 || memcmp(got, msg, sizeof(msg)) != 0)
			dir->errors++;
	}
	return NULL;
}

static void
test_threads(void)
{
	struct dir ab = { &pa, &pb, 0 }, ba = { &pb, &pa, 0 };
	struct datagram d;
	pthread_t t1, t2;

	peers_reset(NULL, NULL);
	CHECK_EQ(handshake(), 0, "threads: handshake");
	seal(&pa, "go", 2, &d);
	open_dgram(&pb, &d, NULL);
	pthread_create(&t1, NULL, pump, &ab);
	pthread_create(&t2, NULL, pump, &ba);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	CHECK_EQ(ab.errors, 0, "threads: a -> b clean");
	CHECK_EQ(ba.errors, 0, "threads: b -> a clean");
	CHECK(!tnshim_lock_held(TNSHIM_LOCK_MTX) &&
	    !tnshim_lock_held(TNSHIM_LOCK_RW), "threads: no lock left held");
}

int
main(void)
{
	if (tnshim_init() != 0)
		return 1;
	test_constants();
	test_handshake_and_data("no psk", 0);
	test_handshake_and_data("psk", 1);
	test_psk_mismatch();
	test_unknown_peer();
	test_initiation_replay();
	test_replay_window();
	test_timers();
	test_cookies();
	test_threads();
	peer_fini(&pa);
	peer_fini(&pb);
	CHECK_EQ(tnshim_rng_fallbacks(), 0, "no random-source fallbacks");
	tnshim_fini();
	return harness_report("e2e");
}
