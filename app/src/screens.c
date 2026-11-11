/*
 * screens.c - drawing for each screen. Positions follow the design:
 * 28 px side margins, a 60 px header and a 72 px footer of button hints.
 */
#include <stdio.h>
#include <string.h>

#include "app_internal.h"
#include "camera.h"

#ifdef HOST_BUILD
#include "draw_host.h"
#endif

#define M       28            /* side margin */
#define CW      (SCREEN_W - 2 * M)
#define MAIN_Y  61
#define FOOT_Y  471
#define TILE_W  ((CW - 14) / 2.0f)

/* Make a style's size smaller, down to two thirds, until s fits in w px
 * (long status words in some languages) */
static void fit_size(TextStyle *st, const char *s, float w)
{
	float min = st->size * 2 / 3;
	while (st->size > min && text_width(st, s) > w)
		st->size -= 2;
}

/* ---- tiles ------------------------------------------------------------- */

/* TL_SIDE_OK and TL_SIDE_WARN go beside the TL_BIG line before them, with
 * no height of their own: the icon, and the words if they fit */
enum { TL_EYEBROW, TL_BIG, TL_OK, TL_WARN, TL_NOTE, TL_TEXT, TL_MUTED, TL_SIDE_OK, TL_SIDE_WARN };

typedef struct TileLine {
	int kind;
	const char *s;
	Colour c;     /* TL_NOTE, TL_BIG: 0 = default */
} TileLine;

static float line_h(int kind)
{
	switch (kind) {
	case TL_EYEBROW:
		return 21;
	case TL_BIG:
		return 31;
	case TL_TEXT:
		return 26.4f;     /* 22 px at line-height 1.2 */
	case TL_MUTED:
		return 22;
	case TL_SIDE_OK:
	case TL_SIDE_WARN:
		return -6;        /* takes back the gap before it */
	default:
		return 24;
	}
}

static float tile_h(const TileLine *l, int n)
{
	float h = 2 + 28;
	for (int i = 0; i < n; i++)
		h += line_h(l[i].kind) + (i ? 6 : 0);
	return h;
}

static void tile_draw(float x, float y, float w, float h, int warn, const TileLine *l, int n)
{
	char fit[128];
	ui_tile(x, y, w, h, warn);
	float cx = x + 19, cy = y + 15, cr = x + w - 19, bx = cx, by = cy;
	for (int i = 0; i < n; i++) {
		TextStyle st;
		switch (l[i].kind) {
		case TL_EYEBROW:
			draw_text(&ST_EYEBROW, cx, cy, l[i].s);
			break;
		case TL_BIG:
			st = st_col(&ST_BIG, l[i].c ? l[i].c : C_TEXT);
			draw_text(&st, cx, cy, l[i].s);
			bx = cx + text_width(&st, l[i].s);
			by = cy;
			break;
		case TL_SIDE_OK:
		case TL_SIDE_WARN: {
			Colour c = l[i].kind == TL_SIDE_OK ? C_LIME : C_ORANGE;
			float vx = bx + 18;
			draw_icon(l[i].kind == TL_SIDE_OK ? ICON_CHECK : ICON_WARN, vx, by + 8, 18, 18, c);
			st = st_col(&ST_TEXT18, c);
			if (vx + 24 + text_width(&st, l[i].s) <= cr)
				draw_text(&st, vx + 24, by + 6, l[i].s);
			break;
		}
		case TL_OK:
		case TL_WARN:
		case TL_NOTE: {
			Colour c = l[i].kind == TL_OK ? C_LIME : l[i].kind == TL_WARN ? C_ORANGE
				: l[i].c ? l[i].c : C_MUTED;
			float tx = cx;
			if (l[i].kind != TL_NOTE) {
				draw_icon(l[i].kind == TL_OK ? ICON_CHECK : ICON_WARN, cx, cy + 3, 18, 18, c);
				tx += 26;
			}
			st = st_col(&ST_OK, c);
			draw_text(&st, tx, cy, ui_fit(&st, l[i].s, cr - tx, fit, sizeof(fit)));
			break;
		}
		case TL_TEXT:
			draw_text(&ST_TEXT22, cx, cy + 0.2f, l[i].s);
			break;
		case TL_MUTED:
			st = st_col(&ST_TEXT18, C_MUTED);
			draw_text(&st, cx, cy, l[i].s);
			break;
		}
		cy += line_h(l[i].kind) + 6;
	}
}

/* Two tiles side by side, as tall as the taller */
static float tile_pair(float y, const TileLine *a, int na, int wa, const TileLine *b, int nb, int wb,
	int action_a, int action_b, int arg_a, int arg_b)
{
	float h = tile_h(a, na), hb = tile_h(b, nb);
	if (hb > h)
		h = hb;
	tile_draw(M, y, TILE_W, h, wa, a, na);
	tile_draw(M + TILE_W + 14, y, TILE_W, h, wb, b, nb);
	if (action_a)
		ui_hit(M, y, TILE_W, h, action_a, arg_a);
	if (action_b)
		ui_hit(M + TILE_W + 14, y, TILE_W, h, action_b, arg_b);
	return h;
}

/* ---- home -------------------------------------------------------------- */

static const BkProfile *active(const App *a)
{
	int p = a->st.profile >= 0 ? a->st.profile : a->last_profile;
	return p >= 0 && p < a->nprof ? &a->prof[p] : NULL;
}

static void header_chips(const App *a)
{
	Chip chips[3];
	int n = 0;
	if (!a->comp.clock_ok)
		chips[n++] = (Chip){ tr(S_CHIP_CLOCK), 1 };
	else if (a->comp.module_loaded && !a->comp.shell_loaded)
		chips[n++] = (Chip){ tr(S_CHIP_NO_QUICK_MENU), 1 };
	else
		chips[n++] = (Chip){ tr(a->st.allow_lan ? S_CHIP_LAN_DIRECT : S_CHIP_LAN_TUNNEL), 0 };
	chips[n++] = (Chip){ tr(a->st.kill_switch ? S_CHIP_KS_ON : S_CHIP_KS_OFF), 0 };
	if (a->st.lockdown)
		chips[n++] = (Chip){ tr(S_CHIP_LOCKDOWN), 0 };
	ui_header_mark(home_view(a) == HV_PROTECTED, chips, n);
}

/* Small "RECEIVED 2.2 MiB   SENT 265 KiB", right-aligned at y; its width.
 * Only measured when draw is 0. */
static float traffic_line(const App *a, float y, int draw)
{
	char rx[24], tx[24];
	const char *lr = tr(S_ROW_RECEIVED), *ls = tr(S_ROW_SENT);
	fmt_bytes(a->st.rx_bytes, rx, sizeof(rx));
	fmt_bytes(a->st.tx_bytes, tx, sizeof(tx));
	float x = SCREEN_W - M;
	x -= text_width(&ST_MONO16, tx);
	if (draw)
		draw_text(&ST_MONO16, x, y, tx);
	x -= 8 + text_width(&ST_EYEBROW, ls);
	if (draw)
		draw_text(&ST_EYEBROW, x, y + 1, ls);
	x -= 24 + text_width(&ST_MONO16, rx);
	if (draw)
		draw_text(&ST_MONO16, x, y, rx);
	x -= 8 + text_width(&ST_EYEBROW, lr);
	if (draw)
		draw_text(&ST_EYEBROW, x, y + 1, lr);
	return SCREEN_W - M - x;
}

/* The status word, the line under it, and a timer on the right; with
 * traffic, the received/sent line above the timer */
static void status_block(const char *word, Colour c, const Span *sub, int nsub,
	const char *eyebrow, const char *value, const App *traffic)
{
	TextStyle st = st_col(&ST_STATE, c);
	float room = CW;
	if (value) {
		float vw = text_width(&ST_MONO26, value), ew = text_width(&ST_EYEBROW, eyebrow);
		float tw = traffic ? traffic_line(traffic, 0, 0) : 0;
		if (tw > vw)
			vw = tw;
		room -= (vw > ew ? vw : ew) + 24;
	}
	fit_size(&st, word, room);
	draw_text(&st, M, 79 + (60 - text_height(&st)) / 2, word);
	if (nsub)
		spans_draw(sub, nsub, M, 147);
	if (value) {
		draw_text(&ST_MONO26, SCREEN_W - M - text_width(&ST_MONO26, value), 142, value);
		draw_text(&ST_EYEBROW, SCREEN_W - M - text_width(&ST_EYEBROW, eyebrow), 115, eyebrow);
	}
	if (traffic)
		traffic_line(traffic, 84, 1);
}

/* "<text> · 23 ms" when the tunnel's round trip is known; text may be buf */
static const char *with_latency(const BkStatus *st, const char *text, char *buf, size_t n)
{
	char tmp[96];
	if (st->latency_ms == BK_NEVER)
		return text;
	snprintf(tmp, sizeof(tmp), "%s \xC2\xB7 %u ms", text, (unsigned)st->latency_ms);
	snprintf(buf, n, "%s", tmp);
	return buf;
}

/* "<profile> · <what is happening>", the name in white */
static int sub_line(const App *a, StrId what, Span *sp, char *buf, size_t n)
{
	const BkProfile *p = active(a);
	return spans_template(tr(what), &ST_SUB_MUTED, p ? profile_display(p) : "", &ST_SUB,
		NULL, NULL, sp, 4, buf, n);
}

static PipeItem node(float w, int disc, int icon, Colour ic, const char *label, Colour lc)
{
	PipeItem it = { 0 };
	it.kind = PIPE_NODE;
	it.width = w;
	it.disc = disc;
	it.icon = icon;
	it.icon_colour = ic;
	it.label = label;
	it.label_colour = lc;
	it.tube_icon = -1;
	return it;
}

static PipeItem line(float w, const char *slab)
{
	PipeItem it = { 0 };
	it.kind = PIPE_LINE;
	it.width = w;
	it.slab = slab;
	it.icon = it.tube_icon = -1;
	return it;
}

static PipeItem tube(int style, const char *text, const char *slab, const TextStyle *slab_st)
{
	PipeItem it = { 0 };
	it.kind = PIPE_TUBE;
	it.tube = style;
	it.tube_text = text;
	it.tube_icon = style == TUBE_SOLID ? ICON_LOCK : ICON_CROSS_SMALL;
	it.slab = slab;
	it.slab_st = slab_st;
	it.icon = -1;
	return it;
}

/* "City, Country", or whichever of them the service gave; 0 if neither */
static int location(const BkCheck *c, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s", c->city, c->city[0] && c->country[0] ? ", " : "", c->country);
	return out[0] != 0;
}

static void home_protected(App *a)
{
	char buf[160], t[24], hs[96], ip[24], dns[24], loc[100];
	Span sp[4];
	const BkProfile *p = active(a);
	char net[32] = "", ipb[16], cnt[8];
	StrId sub = S_SUB_ALL;
	if (!p->full_tunnel && p->allowed_count == 1) {
		snprintf(net, sizeof(net), "%s/%u", fmt_ip(p->allowed[0].net, ipb, sizeof(ipb)), p->allowed[0].prefix);
		sub = S_SUB_ONLY;
	} else if (!p->full_tunnel) {
		snprintf(cnt, sizeof(cnt), "%u", p->allowed_count);
		snprintf(net, sizeof(net), "%s", cnt);
		sub = S_SUB_ONLY_N;
	}
	int n = spans_template(tr(sub), &ST_SUB_MUTED, profile_display(p), &ST_SUB,
		net, &ST_SUB_MUTED, sp, 4, buf, sizeof(buf));
	status_block(tr(S_STATE_PROTECTED), C_LIME, sp, n, tr(S_UP_FOR), fmt_hms(a->st.up_s, t, sizeof(t)), a);

	snprintf(t, sizeof(t), "%u", a->st.handshake_age_s);
	PipeItem pipe[] = {
		node(150, DISC_ON, ICON_VITA, C_TEXT, tr(S_NODE_VITA), 0),
		line(80, tr(S_SEG_WIFI)),
		tube(TUBE_SOLID, tr(S_TUBE_ENCRYPTED), with_latency(&a->st, trf(hs, sizeof(hs), S_HANDSHAKE_AGO, t, NULL),
			hs, sizeof(hs)), NULL),
		node(150, DISC_ON, ICON_HOUSE, C_TEXT, tr(S_NODE_EXIT), 0),
		line(50, NULL),
		node(120, DISC_OFF, ICON_GLOBE, C_MUTED, tr(S_NODE_INTERNET), 0),
	};
	ui_pipe(pipe, 6, M, 191, CW, &PIPE_LARGE, NULL);

	/* exit IP tile: the address, "City, Country", the network, one
	 * a line; whether it is the ISP's goes on a line of its own when there
	 * is room, else beside the address */
	TileLine l1[5] = { { TL_EYEBROW, tr(S_YOUR_IP), 0 } };
	TileLine l2[3] = { { TL_EYEBROW, tr(S_DNS), 0 } };
	int n1 = 3;
	if (a->chk.state == BK_CHECK_DONE) {
		const BkCheck *c = &a->chk;
		int known = c->isp_ip != 0, same = c->exit_ip == c->isp_ip;
		l1[1] = (TileLine){ TL_BIG, fmt_ip(c->exit_ip, ip, sizeof(ip)), 0 };
		n1 = 2;
		if (location(c, loc, sizeof(loc)))
			l1[n1++] = (TileLine){ TL_NOTE, loc, C_TEXT };
		if (c->org[0])
			l1[n1++] = (TileLine){ TL_NOTE, c->org, 0 };
		if (known) {
			int own = n1 < 4;
			l1[n1++] = (TileLine){ own ? (same ? TL_WARN : TL_OK) : (same ? TL_SIDE_WARN : TL_SIDE_OK),
				tr(same ? S_IP_SAME : S_IP_NOT_ISP), 0 };
		}
	} else if (a->chk.state == BK_CHECK_FAILED) {
		l1[1] = (TileLine){ TL_BIG, tr(S_DASH), C_MUTED };
		l1[2] = (TileLine){ TL_WARN, error_cause(a->chk.fail_reason), 0 };
	} else {
		l1[1] = (TileLine){ TL_BIG, tr(S_ELLIPSIS), C_MUTED };
		l1[2] = (TileLine){ TL_NOTE, tr(S_CHECKING), 0 };
	}
	/* DNS tile */
	l2[1] = (TileLine){ TL_BIG, p->dns[0] ? fmt_ip(p->dns[0], dns, sizeof(dns)) : tr(S_DNS_NETWORK),
		p->dns[0] ? 0 : C_MUTED };
	/* The local DNS verdict comes first in a check and stands even when the
	 * exit-IP fetch then fails */
	switch (a->chk.state == BK_CHECK_DONE || a->chk.state == BK_CHECK_FAILED ? (int)a->chk.dns : -1) {
	case BK_DNS_OK:
		l2[2] = (TileLine){ TL_OK, tr(S_DNS_INSIDE), 0 };
		break;
	case BK_DNS_LEAK:
		l2[2] = (TileLine){ TL_WARN, tr(S_DNS_LEAKS), 0 };
		break;
	case BK_DNS_SPLIT:
		l2[2] = (TileLine){ TL_NOTE, tr(S_DNS_SPLIT_NOTE), 0 };
		break;
	case BK_DNS_UNMANAGED:
		l2[2] = (TileLine){ TL_NOTE, tr(S_DNS_UNMANAGED_NOTE), 0 };
		break;
	default:
		l2[2] = (TileLine){ TL_NOTE, tr(S_CHECKING), 0 };
		break;
	}
	int done = a->chk.state == BK_CHECK_DONE;
	tile_pair(292, l1, n1, done && a->chk.isp_ip && a->chk.exit_ip == a->chk.isp_ip,
		l2, 3, done && a->chk.dns == BK_DNS_LEAK, A_OPEN, A_OPEN, SCR_LEAK, SCR_LEAK);

	hint(a, BTN_CROSS, tr(S_HINT_DISCONNECT), A_DISCONNECT_ASK, BTN_CROSS, 0);
	hint(a, BTN_TRIANGLE, tr(S_HINT_MIGRATE), A_OPEN, SCR_MIGRATE, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_DETAILS), A_OPEN, SCR_DETAILS, 1);
}

/* Down, held, blocked or paused: the dashed tube and a cause */
static void home_down(App *a, HomeView v)
{
	char buf[160], t[24], slab[96], n1[24], n2[24];
	Span sp[4];
	int n;
	const char *word, *eyebrow = NULL, *value = NULL;
	const char *tube_text = tr(S_TUBE_DOWN), *slab_text;
	int far_disc = DISC_BAD;
	const char *far_label = tr(S_NODE_DIRECT);
	Colour far_colour = C_ORANGE;
	TileLine l1[3], l2[3];
	int warn1 = 0;

	switch (v) {
	case HV_HELD:
		word = tr(S_STATE_HELD);
		n = sub_line(a, S_SUB_HELD, sp, buf, sizeof(buf));
		eyebrow = tr(S_RETRY_IN);
		value = fmt_clock(a->st.retry_in_s, t, sizeof(t));
		slab_text = tr(S_SLAB_NO_ANSWER);
		far_label = tr(S_NODE_HELD);
		break;
	case HV_BLOCKED:
		word = tr(S_STATE_BLOCKED);
		n = sub_line(a, S_SUB_BLOCKED, sp, buf, sizeof(buf));
		eyebrow = tr(S_TRYING_FOR);
		value = fmt_clock(a->st.state_s, t, sizeof(t));
		tube_text = tr(S_TUBE_RECONNECTING);
		snprintf(n1, sizeof(n1), "%u", a->st.attempts);
		slab_text = trf(slab, sizeof(slab), S_SLAB_ATTEMPT_NOTHING, n1, NULL);
		far_label = tr(S_NODE_HELD);
		break;
	case HV_PAUSED:
		word = tr(S_STATE_PAUSED);
		n = sub_line(a, S_SUB_DIRECT, sp, buf, sizeof(buf));
		eyebrow = tr(S_RESUMES_IN);
		value = fmt_clock(a->st.pause_left_s, t, sizeof(t));
		tube_text = tr(S_TUBE_PAUSED);
		slab_text = tr(S_SLAB_PAUSED);
		break;
	default:
		word = tr(S_STATE_NOT_PROTECTED);
		n = sub_line(a, S_SUB_DIRECT, sp, buf, sizeof(buf));
		eyebrow = tr(S_RETRY_IN);
		value = fmt_clock(a->st.retry_in_s, t, sizeof(t));
		slab_text = a->st.last_error == BK_ERR_NO_ANSWER || a->st.last_error == BK_ERR_NONE
			? tr(S_SLAB_NO_ANSWER) : error_cause(a->st.last_error);
		break;
	}
	status_block(word, C_ORANGE, sp, n, eyebrow, value, NULL);

	PipeItem pipe[] = {
		node(150, DISC_OFF, -1, 0, tr(S_NODE_VITA), 0),
		tube(TUBE_DASHED, tube_text, slab_text, &ST_SLAB),
		node(150, far_disc, -1, 0, far_label, far_colour),
	};
	ui_pipe(pipe, 3, M, 191, CW, &PIPE_LARGE, NULL);

	/* left tile: why */
	l1[0] = (TileLine){ TL_EYEBROW, tr(S_LIKELY_CAUSE), 0 };
	switch (v) {
	case HV_BLOCKED:
		l1[0].s = tr(S_WHY);
		l1[1] = (TileLine){ TL_TEXT, tr(S_BLOCKED_WHY), 0 };
		l1[2] = (TileLine){ TL_MUTED, tr(S_BLOCKED_WHY_NOTE), 0 };
		break;
	case HV_PAUSED:
		l1[0].s = tr(S_PAUSE);
		l1[1] = (TileLine){ TL_TEXT, tr(S_PAUSED_WHAT), 0 };
		l1[2] = (TileLine){ TL_MUTED, tr(S_PAUSED_NOTE), 0 };
		break;
	default:
		warn1 = 1;
		switch (a->st.last_error) {
		case BK_ERR_NONE:
		case BK_ERR_NO_ANSWER:
			l1[1] = (TileLine){ TL_TEXT, tr(S_CAUSE_PORT), 0 };
			l1[2] = (TileLine){ TL_MUTED, tr(S_CAUSE_PORT_NOTE), 0 };
			break;
		default:
			l1[1] = (TileLine){ TL_TEXT, error_cause(a->st.last_error), 0 };
			l1[2] = (TileLine){ TL_MUTED, a->st.last_error_detail, 0 };
			break;
		}
		break;
	}
	/* right tile: kill switch, or what lockdown is holding back */
	if (v == HV_BLOCKED) {
		snprintf(n2, sizeof(n2), "%u", a->st.lock_drops);
		l2[0] = (TileLine){ TL_EYEBROW, tr(S_HELD_BACK), 0 };
		l2[1] = (TileLine){ TL_TEXT, trf(slab, sizeof(slab), S_PACKETS, n2, NULL), 0 };
		l2[2] = (TileLine){ TL_MUTED, tr(a->st.lockdown ? S_HELD_BACK_NOTE_LD : S_HELD_BACK_NOTE), 0 };
	} else {
		int ks = ks_effective(&a->st) && v != HV_PAUSED;
		l2[0] = (TileLine){ TL_EYEBROW, tr(S_KILL_SWITCH), 0 };
		l2[1] = (TileLine){ TL_TEXT, tr(ks ? S_ON : S_OFF), 0 };
		l2[2] = (TileLine){ TL_MUTED, tr(v == HV_PAUSED ? S_KS_PAUSED_NOTE
			: a->st.lockdown ? S_KS_LOCKDOWN_NOTE : ks ? S_KS_ON_NOTE : S_KS_OFF_NOTE), 0 };
	}
	tile_pair(292, l1, 3, warn1, l2, 3, 0, A_OPEN, v == HV_BLOCKED ? 0 : A_KILL_SWITCH, SCR_DETAILS, 0);

	switch (v) {
	case HV_PAUSED:
		hint(a, BTN_CROSS, tr(S_HINT_RESUME), A_RESUME, 0, 0);
		break;
	case HV_BLOCKED:
		hint(a, BTN_CROSS, tr(S_HINT_RETRY), A_RETRY, 0, 0);
		hint(a, BTN_TRIANGLE, tr(S_HINT_PAUSE), A_PAUSE, 0, 0);
		if (a->st.lockdown)
			hint_dim(a);   /* refused under Lockdown; pressing it says why */
		break;
	default:
		hint(a, BTN_CROSS, tr(S_HINT_RETRY), A_RETRY, 0, 0);
		/* under Lockdown the kill switch setting changes nothing here */
		if (!a->st.lockdown)
			hint(a, BTN_TRIANGLE, tr(a->st.kill_switch ? S_HINT_KS_OFF : S_HINT_KS_ON), A_KILL_SWITCH, 0, 0);
		break;
	}
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_DISCONNECT), A_DISCONNECT_ASK, BTN_CIRCLE, 1);
}

static void home_connecting(App *a)
{
	char buf[160], t[24], slab[96], n1[24], ep[32];
	Span sp[4];
	int n = sub_line(a, ks_effective(&a->st) ? S_SUB_CONNECTING_HELD : S_SUB_CONNECTING, sp, buf, sizeof(buf));
	status_block(tr(S_STATE_CONNECTING), C_TEXT, sp, n, tr(S_TRYING_FOR), fmt_clock(a->st.state_s, t, sizeof(t)), NULL);

	snprintf(n1, sizeof(n1), "%u", a->st.attempts);
	PipeItem pipe[] = {
		node(150, DISC_ON, ICON_VITA, C_TEXT, tr(S_NODE_VITA), 0),
		line(80, tr(S_SEG_WIFI)),
		tube(TUBE_DASHED, tr(S_TUBE_HANDSHAKING), trf(slab, sizeof(slab), S_SLAB_ATTEMPT, n1, NULL), NULL),
		node(150, DISC_OFF, ICON_HOUSE, C_MUTED, tr(S_NODE_EXIT), 0),
		line(50, NULL),
		node(120, DISC_OFF, ICON_GLOBE, C_MUTED, tr(S_NODE_INTERNET), 0),
	};
	pipe[2].tube_icon = -1;
	ui_pipe(pipe, 6, M, 191, CW, &PIPE_LARGE, NULL);

	const BkProfile *p = active(a);
	TileLine l1[3] = {
		{ TL_EYEBROW, tr(S_SERVER), 0 },
		{ TL_BIG, fmt_ip_port(a->st.endpoint_ip, a->st.endpoint_port, ep, sizeof(ep)), 0 },
		{ TL_NOTE, p ? p->endpoint_host : "", 0 },
	};
	TileLine l2[3] = {
		{ TL_EYEBROW, tr(S_KILL_SWITCH), 0 },
		{ TL_TEXT, tr(ks_effective(&a->st) ? S_ON : S_OFF), 0 },
		{ TL_MUTED, tr(a->st.lockdown ? S_KS_LOCKDOWN_NOTE : a->st.kill_switch ? S_KS_ON_NOTE : S_KS_CONNECTING_NOTE), 0 },
	};
	tile_pair(292, l1, 3, 0, l2, 3, 0, A_OPEN, A_KILL_SWITCH, SCR_DETAILS, 0);

	hint(a, BTN_CROSS, tr(S_HINT_CANCEL), A_DISCONNECT, 0, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_DETAILS), A_OPEN, SCR_DETAILS, 1);
}

static void home_idle(App *a)
{
	char buf[160], cnt[16];
	Span sp[4];
	const BkProfile *p = active(a);
	int n = p ? sub_line(a, a->st.lockdown_wait ? S_SUB_LOCKDOWN_WAIT : S_SUB_READY, sp, buf, sizeof(buf))
		: spans_template(tr(S_SUB_NO_PROFILE), &ST_SUB_MUTED, NULL, NULL, NULL, NULL, sp, 4, buf, sizeof(buf));
	/* The public address websites see now (the idle check), else the last
	 * one learnt with the tunnel down */
	char ipv[24];
	const char *ipval = NULL;
	if (a->chk.state == BK_CHECK_DONE && a->chk.exit_ip)
		ipval = fmt_ip(a->chk.exit_ip, ipv, sizeof(ipv));
	else if (a->chk.state == BK_CHECK_RUNNING)
		ipval = tr(S_ELLIPSIS);
	else if (a->chk.isp_ip)
		ipval = fmt_ip(a->chk.isp_ip, ipv, sizeof(ipv));
	status_block(tr(S_STATE_OFF), C_TEXT, sp, n, ipval ? tr(S_YOUR_IP) : NULL, ipval, NULL);

	PipeItem pipe[] = {
		node(150, DISC_OFF, ICON_VITA, C_MUTED, tr(S_NODE_VITA), 0),
		line(0, tr(S_SEG_WIFI)),
		node(150, DISC_OFF, ICON_GLOBE, C_MUTED, tr(S_NODE_INTERNET), 0),
	};
	pipe[1].slab_st = &ST_SLAB_MONO;
	ui_pipe(pipe, 3, M, 191, CW, &PIPE_LARGE, NULL);

	snprintf(cnt, sizeof(cnt), "%d", a->nprof);
	TileLine l1[3] = {
		{ TL_EYEBROW, tr(S_PROFILES_TITLE), 0 },
		{ TL_TEXT, trf(buf, sizeof(buf), S_N_FOUND, cnt, NULL), 0 },
		{ TL_MUTED, bk_profiles_folder(), 0 },
	};
	TileLine l2[3] = {
		{ TL_EYEBROW, tr(S_KILL_SWITCH), 0 },
		{ TL_TEXT, tr(ks_effective(&a->st) ? S_ON : S_OFF), 0 },
		{ TL_MUTED, tr(a->st.lockdown ? S_KS_LOCKDOWN_NOTE : a->st.kill_switch ? S_KS_ON_NOTE : S_KS_OFF_NOTE), 0 },
	};
	tile_pair(292, l1, 3, 0, l2, 3, 0, A_OPEN, A_KILL_SWITCH, SCR_PROFILES, 0);

	hint(a, BTN_CROSS, tr(S_HINT_CONNECT), A_CONNECT, -1, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_START, tr(S_HINT_HELP), A_OPEN, SCR_HELP, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_DETAILS), A_OPEN, SCR_DETAILS, 1);
}

/* Disconnected under Lockdown: the block is in effect,
 * nothing but the tunnel may leave; Connect is the way online */
static void home_lockdown(App *a)
{
	char buf[160];
	Span sp[4];
	int n = sub_line(a, S_SUB_LOCKDOWN, sp, buf, sizeof(buf));
	status_block(tr(S_STATE_BLOCKED), C_ORANGE, sp, n, NULL, NULL, NULL);

	PipeItem pipe[] = {
		node(150, DISC_OFF, -1, 0, tr(S_NODE_VITA), 0),
		tube(TUBE_DASHED, tr(S_TUBE_LOCKDOWN), tr(S_SLAB_LOCKDOWN), &ST_SLAB),
		node(150, DISC_BAD, -1, 0, tr(S_NODE_BLOCKED), C_ORANGE),
	};
	ui_pipe(pipe, 3, M, 191, CW, &PIPE_LARGE, NULL);

	TileLine l1[3] = {
		{ TL_EYEBROW, tr(S_LOCKDOWN), 0 },
		{ TL_TEXT, tr(S_LOCKDOWN_WHAT), 0 },
		{ TL_MUTED, tr(a->st.allow_lan ? S_LOCKDOWN_LAN_NOTE : S_LOCKDOWN_NO_LAN_NOTE), 0 },
	};
	TileLine l2[3] = {
		{ TL_EYEBROW, tr(S_TO_GO_ONLINE), 0 },
		{ TL_TEXT, tr(S_LOCKDOWN_HOWTO), 0 },
		{ TL_MUTED, tr(a->st.lockdown_ep_unresolved ? S_LOCKDOWN_EP_FIRST : S_LOCKDOWN_OFF_WHERE), 0 },
	};
	tile_pair(292, l1, 3, 0, l2, 3, 0, A_OPEN, A_OPEN, SCR_DETAILS, SCR_DETAILS);

	hint(a, BTN_CROSS, tr(S_HINT_CONNECT), A_CONNECT, -1, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_START, tr(S_HINT_HELP), A_OPEN, SCR_HELP, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_DETAILS), A_OPEN, SCR_DETAILS, 1);
}

/* The kernel module is missing or out of date, or the SceShell plugin is
 * not loaded */
static void home_missing(App *a)
{
	char buf[160];
	Span sp[4];
	int module_ok = a->comp.module_loaded && a->comp.module_api_ok;
	StrId sub = module_ok ? (a->comp.shell_guard ? S_SUB_PLUGIN_GUARD : S_SUB_PLUGIN_MISSING)
		: a->comp.module_loaded ? S_SUB_MODULE_OLD : S_SUB_MODULE_MISSING;
	int n = spans_template(tr(sub), &ST_SUB_MUTED, NULL, NULL, NULL, NULL, sp, 4, buf, sizeof(buf));
	status_block(tr(S_STATE_NOT_INSTALLED), C_ORANGE, sp, n, NULL, NULL, NULL);

	PipeItem pipe[] = {
		node(150, DISC_OFF, ICON_VITA, C_MUTED, tr(S_NODE_VITA), 0),
		tube(TUBE_DASHED, tr(S_TUBE_NO_MODULE), tr(module_ok ? S_SLAB_NO_PLUGIN : S_SLAB_NO_MODULE), &ST_SLAB),
		node(150, DISC_OFF, ICON_GLOBE, C_MUTED, tr(S_NODE_INTERNET), 0),
	};
	ui_pipe(pipe, 3, M, 191, CW, &PIPE_LARGE, NULL);

	TileLine l1[3] = {
		{ TL_EYEBROW, tr(S_KERNEL_MODULE), 0 },
		{ TL_TEXT, tr(S_MODULE_FILE), 0 },
		{ TL_MUTED, tr(module_ok ? S_MODULE_LOADED : S_MODULE_HOWTO), 0 },
	};
	TileLine l2[3] = {
		{ TL_EYEBROW, tr(S_QUICK_MENU_PLUGIN), 0 },
		{ TL_TEXT, tr(S_PLUGIN_FILE), 0 },
		{ TL_MUTED, tr(a->comp.shell_loaded ? S_PLUGIN_LOADED
			: a->comp.shell_guard ? S_PLUGIN_GUARD : S_PLUGIN_HOWTO), 0 },
	};
	tile_pair(292, l1, 3, !module_ok, l2, 3, !a->comp.shell_loaded, 0, 0, 0, 0);

	/* the boot guard: one press clears it and restarts the console */
	if (module_ok && a->comp.shell_guard)
		hint(a, BTN_CROSS, tr(S_HINT_REENABLE), A_GUARD_RESET, 0, 0);
	else
		hint(a, BTN_CROSS, tr(S_HINT_CHECK_AGAIN), A_CHECK_AGAIN, 0, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_PROFILES), A_OPEN, SCR_PROFILES, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_ABOUT), A_OPEN, SCR_ABOUT, 1);
}

void screen_home(App *a)
{
	HomeView v = home_view(a);
	header_chips(a);
	if (v != HV_MISSING && v != HV_IDLE && !active(a))
		v = HV_IDLE;
	switch (v) {
	case HV_LOCKDOWN:
		home_lockdown(a);
		break;
	case HV_PROTECTED:
		home_protected(a);
		break;
	case HV_CONNECTING:
		home_connecting(a);
		break;
	case HV_IDLE:
		home_idle(a);
		break;
	case HV_MISSING:
		home_missing(a);
		break;
	default:
		home_down(a, v);
		break;
	}
	ui_footer(a->hints, a->nhints);
}

/* ---- profiles ---------------------------------------------------------- */

#define ROW_H    82
#define ROW_GAP  8
#define HEAD_H   34
#define LIST_TOP 73
#define LIST_BOT (FOOT_Y - 12)

/* The miniature pipe: full tunnel solid, split tunnel outlined */
static void mini_pipe(float x, float y, int lit, int full)
{
	Colour dot = lit ? C_LIME : C_DISC;
	draw_ring(x + 7, y + 7, 7, 2, dot);
	draw_rect(x + 14, y + 6, 16, 2, C_DISC);
	if (full)
		draw_rrect(x + 30, y, 52, 14, 7, C_LIME);
	else
		draw_rrect_stroke(x + 30, y, 52, 14, 7, 2, C_LIME);
	draw_ring(x + 89, y + 7, 7, 2, dot);
}

static int is_duplicate(const App *a, int i)
{
	for (int j = 0; j < a->nprof; j++)
		if (j != i && !strcmp(a->prof[j].name, a->prof[i].name) &&
		    !strcmp(a->prof[j].group, a->prof[i].group))
			return 1;
	return 0;
}

static void profile_row(App *a, int i, float y, int focused, int pos)
{
	const BkProfile *p = &a->prof[i];
	char detail[160], routes[48], fit[96], file[80], st_buf[64], n1[16];
	float b = focused ? 2 : 1;
	ui_card(M, y, CW, ROW_H, focused);
	ui_hit(M, y, CW, ROW_H, A_SELECT, pos);

	int is_active = i == a->st.profile && a->st.state != BK_IDLE;
	mini_pipe(M + b + 18, y + 34, is_active && a->st.state == BK_UP, p->full_tunnel);

	float nx = M + b + 18 + 96 + 20;
	float nw = CW - 2 * b - 36 - 96 - 250 - 40;
	draw_text(&ST_NAME, nx, y + 14 + (29.9f - text_height(&ST_NAME)) / 2,
		ui_fit(&ST_NAME, profile_display(p), nw, fit, sizeof(fit)));

	/* routes · where: the file name if nicknamed, or the endpoint; an
	 * invalid profile has no routes, so just its file */
	profile_routes(p, routes, sizeof(routes));
	const char *where = p->endpoint_host;
	if (p->nickname[0] || (p->warnings & BK_WARN_INVALID)) {
		snprintf(file, sizeof(file), "%s.conf", p->name);
		where = file;
	}
	if (p->warnings & BK_WARN_INVALID)
		snprintf(detail, sizeof(detail), "%s", file);
	else if (p->full_tunnel || p->nickname[0])
		trf(detail, sizeof(detail), S_JOIN, routes, where);
	else
		snprintf(detail, sizeof(detail), "%s", routes);
	if (is_duplicate(a, i)) {
		char d2[160];
		snprintf(d2, sizeof(d2), "%s", detail);
		trf(detail, sizeof(detail), S_JOIN, d2, p->storage);
	}
	draw_text(&ST_DETAIL, nx, y + 14 + 29.9f + 2, ui_fit(&ST_DETAIL, detail, nw, fit, sizeof(fit)));

	/* status column */
	const char *s = tr(S_ROW_READY);
	Colour c = C_MUTED;
	int icon = -1;
	if (is_active) {
		switch (a->st.state) {
		case BK_UP:
			s = tr(S_ROW_CONNECTED), c = C_LIME, icon = ICON_CHECK;
			break;
		case BK_HANDSHAKING:
			s = tr(a->st.locked ? S_ROW_BLOCKED : S_ROW_CONNECTING);
			break;
		case BK_PAUSED:
			s = tr(S_ROW_PAUSED);
			break;
		default:
			s = tr(S_ROW_NOT_ANSWERING), c = C_ORANGE, icon = ICON_WARN;
			break;
		}
	} else if (p->warnings & BK_WARN_INVALID) {
		s = tr(S_ROW_INVALID), c = C_ORANGE, icon = ICON_WARN;
	} else if (p->warnings & BK_WARN_CLASH) {
		s = tr(S_ROW_CLASH), c = C_ORANGE, icon = ICON_WARN;
	} else if (p->warnings & BK_WARN_IGNORED) {
		snprintf(n1, sizeof(n1), "%d", p->ignored_lines);
		s = trf(st_buf, sizeof(st_buf), p->ignored_lines == 1 ? S_ROW_IGNORED_1 : S_ROW_IGNORED_N, n1, NULL);
		c = C_ORANGE, icon = ICON_WARN;
	}
	TextStyle st = st_col(&ST_ST, c);
	float right = M + CW - b - 18;
	float tw = text_width(&st, s);
	draw_text(&st, right - tw, y + 29, s);
	if (icon >= 0)
		draw_icon(icon, right - tw - 8 - 18, y + 32, 18, 18, c);
}

void screen_profiles(App *a)
{
	char buf[96], cnt[16];
	snprintf(cnt, sizeof(cnt), "%d", a->nprof);
	Span right[1] = { { &ST_DETAIL, trf(buf, sizeof(buf),
		bk_profiles_storages() == BK_STORE_UX0 ? S_FOUND_ON_CARD : S_N_FOUND, cnt, NULL) } };
	ui_header_title(tr(S_PROFILES_TITLE), right, 1);

	if (a->nprof == 0) {
		/* where to put files, and the other way in: a QR code */
		char para[256];
		Span sp[4];
		int n = spans_template(tr(S_NO_PROFILES), &ST_TEXT22, bk_profiles_folder(), &ST_MONO18,
			NULL, NULL, sp, 4, para, sizeof(para));
		draw_text(&ST_OPT_NAME, M, 100, tr(S_NO_PROFILES_TITLE));
		float h = spans_para(sp, n, M, 150, 700, 30, ALIGN_LEFT, 1);
		Span qr[1] = { { &ST_SUB_MUTED, tr(S_NO_PROFILES_QR) } };
		float h2 = spans_para(qr, 1, M, 150 + h + 14, 700, 30, ALIGN_LEFT, 1);
		/* this is a client: say so where a newcomer looks first */
		TextStyle warn = st_col(&ST_TEXT20, C_ORANGE);
		Span byo[1] = { { &warn, tr(S_NO_PROFILES_BYO) } };
		spans_para(byo, 1, M, 150 + h + 14 + h2 + 18, 700, 28, ALIGN_LEFT, 1);
	} else {
		/* positions without scrolling, then scroll the selection into view */
		float ys[MAX_ITEMS], y = LIST_TOP, sel_top = 0, sel_bot = 0;
		int row = 0;
		for (int i = 0; i < a->nitems; i++) {
			ys[i] = y;
			if (a->items[i].header) {
				y += HEAD_H;
			} else {
				if (row == a->sel) {
					sel_top = (i > 0 && a->items[i - 1].header) ? ys[i - 1] : y;
					sel_bot = y + ROW_H;
				}
				row++;
				y += ROW_H + ROW_GAP;
			}
		}
		if (sel_top - a->scroll < LIST_TOP)
			a->scroll = sel_top - LIST_TOP;
		if (sel_bot - a->scroll > LIST_BOT)
			a->scroll = sel_bot - LIST_BOT;
		if (a->scroll < 0)
			a->scroll = 0;

		draw_clip(0, MAIN_Y, SCREEN_W, FOOT_Y - MAIN_Y);
		row = 0;
		for (int i = 0; i < a->nitems; i++) {
			float iy = ys[i] - a->scroll;
			if (a->items[i].header) {
				if (iy + HEAD_H > MAIN_Y && iy < FOOT_Y)
					draw_text(&ST_EYEBROW, M + 2, iy + 4, a->items[i].title);
				continue;
			}
			if (iy + ROW_H > MAIN_Y && iy < FOOT_Y)
				profile_row(a, a->items[i].profile, iy, row == a->sel, row);
			row++;
		}
		draw_unclip();
	}

	if (a->nprof) {
		hint(a, BTN_CROSS, tr(S_HINT_CONNECT), A_CONNECT_SEL, 0, 0);
		hint(a, BTN_TRIANGLE, tr(S_HINT_DETAILS), A_PROFILE_DETAILS, 0, 0);
	} else {
		hint(a, BTN_CROSS, tr(S_HINT_SCAN_QR), A_SCAN_QR, 0, 0);
	}
	hint(a, BTN_SQUARE, tr(S_HINT_RESCAN), A_RESCAN, 0, 0);
	if (a->nprof)
		hint(a, BTN_START, tr(S_HINT_SCAN_QR), A_SCAN_QR, 0, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}

/* ---- migrate ----------------------------------------------------------- */

static void mig_pipe(const App *a, int target, int seamless, float x, float cy)
{
	char from[24], to[24];
	const BkProfile *cur = active(a);
	profile_exit_label(&a->prof[target], to, sizeof(to));
	if (cur)
		profile_exit_label(cur, from, sizeof(from));
	else
		from[0] = 0;
	TextStyle lime = st_col(&ST_TEXT18, C_LIME), muted = st_col(&ST_TEXT18, C_MUTED);
	float ew = text_width(&ST_TEXT18, to);
	if (seamless && text_width(&ST_TEXT18, from) > ew)
		ew = text_width(&ST_TEXT18, from);
	ew += 26;
	float ex = x + 250 - ew;

	draw_ring(x + 9, cy, 9, 2, C_LIME);
	draw_rect(x + 18, cy - 1, 18, 2, C_DISC);
	float fx0 = x + 36, fx1 = ex - 10;
	if (seamless) {
		draw_rrect(fx0, cy - 8, fx1 - fx0, 16, 8, C_LIME);
		/* old exit dimmed above, new exit lit below */
		draw_ring(ex + 9, cy - 15 + 11, 9, 2, C_DISC);
		draw_text(&muted, ex + 26, cy - 26, from);
		draw_ring(ex + 9, cy + 15 + 0, 9, 2, C_LIME);
		draw_text(&lime, ex + 26, cy + 4, to);
	} else {
		float part = (fx1 - fx0 - 28) / 2;
		draw_rrect(fx0, cy - 8, part, 16, 8, C_LIME);
		draw_icon(ICON_BREAK, fx0 + part + 3, cy - 11, 22, 22, C_ORANGE);
		draw_rrect_stroke(fx0 + part + 28, cy - 8, part, 16, 8, 2, C_LIME);
		draw_ring(ex + 9, cy, 9, 2, C_LIME);
		draw_text(&ST_TEXT18, ex + 26, cy - 11, to);
	}
}

void screen_migrate(App *a)
{
	char buf[128], para[256], label[96];
	Span right[2];
	const BkProfile *cur = active(a);
	int n = spans_template(tr(S_NOW_ON), &ST_HEAD_RIGHT, cur ? profile_display(cur) : tr(S_DASH),
		&ST_TEXT20, NULL, NULL, right, 2, buf, sizeof(buf));
	ui_header_title(tr(S_MIGRATE_TITLE), right, n);
	draw_text(&ST_EYEBROW, M, 77, tr(S_MOVE_TO));

	if (a->nmig == 0) {
		draw_text(&ST_TEXT22, M, 120, tr(S_NO_OTHER_PROFILES));
	} else {
		/* measure, scroll, draw */
		float hs[BK_MAX_PROFILES], ys[BK_MAX_PROFILES], y = 110;
		for (int i = 0; i < a->nmig; i++) {
			int seamless = bk_migrate_kind(a->mig[i]) == BK_MIGRATE_SEAMLESS;
			StrId text = seamless ? S_MIG_SEAMLESS_TEXT
				: ks_effective(&a->st) ? S_MIG_RECONNECT_HELD_TEXT : S_MIG_RECONNECT_TEXT;
			Span sp[1] = { { &ST_TEXT22, tr(text) } };
			float b = i == a->mig_sel ? 2 : 1;
			hs[i] = 2 * b + 32 + 37 + 6 + spans_para(sp, 1, 0, 0, CW - 2 * b - 40 - 24 - 250, 27.5f, ALIGN_LEFT, 0);
			ys[i] = y;
			y += hs[i] + 12;
		}
		float top = ys[a->mig_sel], bot = ys[a->mig_sel] + hs[a->mig_sel];
		if (top - a->mig_scroll < 110)
			a->mig_scroll = top - 110;
		if (bot - a->mig_scroll > FOOT_Y - 16)
			a->mig_scroll = bot - (FOOT_Y - 16);
		if (a->mig_scroll < 0)
			a->mig_scroll = 0;

		draw_clip(0, 100, SCREEN_W, FOOT_Y - 100);
		for (int i = 0; i < a->nmig; i++) {
			float oy = ys[i] - a->mig_scroll;
			if (oy > FOOT_Y || oy + hs[i] < 100)
				continue;
			int p = a->mig[i];
			int seamless = bk_migrate_kind(p) == BK_MIGRATE_SEAMLESS;
			float b = i == a->mig_sel ? 2 : 1;
			ui_card(M, oy, CW, hs[i], i == a->mig_sel);
			ui_hit(M, oy, CW, hs[i], A_SELECT, i);
			float cx = M + b + 20, cy = oy + b + 16;
			float tw = CW - 2 * b - 40 - 24 - 250;
			char fit[96];
			float nw = text_width(&ST_OPT_NAME, ui_fit(&ST_OPT_NAME, profile_display(&a->prof[p]),
				tw - 160, fit, sizeof(fit)));
			draw_text(&ST_OPT_NAME, cx, cy, fit);
			TextStyle vs = st_col(&ST_VERDICT, seamless ? C_LIME : C_ORANGE);
			draw_text_baseline(&vs, cx + nw + 14, cy + text_ascent(&ST_OPT_NAME),
				tr(seamless ? S_SEAMLESS : S_RECONNECT));
			StrId text = seamless ? S_MIG_SEAMLESS_TEXT
				: ks_effective(&a->st) ? S_MIG_RECONNECT_HELD_TEXT : S_MIG_RECONNECT_TEXT;
			Span sp[1] = { { &ST_TEXT22, tr(text) } };
			spans_para(sp, 1, cx, cy + 37 + 6, tw, 27.5f, ALIGN_LEFT, 1);
			mig_pipe(a, p, seamless, cx + tw + 24, oy + hs[i] / 2);
		}
		draw_unclip();
	}

	if (a->nmig) {
		hint(a, BTN_CROSS, trf(label, sizeof(label), S_HINT_MIGRATE_TO,
			profile_display(&a->prof[a->mig[a->mig_sel]]), NULL), A_MIGRATE_SEL, 0, 0);
	}
	hint(a, BTN_CIRCLE, tr(S_HINT_CANCEL), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
	(void)para;
}

/* ---- leak check -------------------------------------------------------- */

void screen_leak(App *a)
{
	char buf[256], ip[24], ip2[24];
	const BkProfile *p = active(a);
	const BkCheck *c = &a->chk;
	Span right[1] = { { &ST_HEAD_RIGHT, p ? profile_display(p) : "" } };
	ui_header_title(tr(S_LEAK_TITLE), right, p ? 1 : 0);

	int up = a->st.state == BK_UP && a->st.profile >= 0;
	int leak = up && c->state == BK_CHECK_DONE && c->dns == BK_DNS_LEAK;
	const char *word;
	Colour wc = C_TEXT;
	StrId text = S_LEAK_TEXT_OK;
	const char *arg = NULL;
	if (!up) {
		word = tr(S_LEAK_NOT_CONNECTED);
		text = S_LEAK_TEXT_NOT_CONNECTED;
	} else if (c->state == BK_CHECK_RUNNING || c->state == BK_CHECK_NOT_RUN) {
		word = tr(S_CHECKING_WORD);
		text = S_LEAK_TEXT_RUNNING;
	} else if (c->state == BK_CHECK_FAILED) {
		word = tr(S_LEAK_CANT_CHECK);
		wc = C_ORANGE;
		text = c->fail_reason == BK_ERR_CLOCK ? S_LEAK_TEXT_CLOCK : S_LEAK_TEXT_FAILED;
	} else {
		switch (c->dns) {
		case BK_DNS_LEAK:
			word = tr(S_LEAK_DNS_LEAK);
			wc = C_ORANGE;
			text = S_LEAK_TEXT_LEAK;
			arg = p && p->dns[0] ? fmt_ip(p->dns[0], ip, sizeof(ip)) : "";
			break;
		case BK_DNS_SPLIT:
			word = tr(S_LEAK_SPLIT);
			text = S_LEAK_TEXT_SPLIT;
			break;
		case BK_DNS_UNMANAGED:
			word = tr(S_LEAK_UNMANAGED);
			wc = C_MUTED;
			text = S_LEAK_TEXT_UNMANAGED;
			break;
		default:
			word = tr(S_LEAK_NO_LEAK);
			wc = C_LIME;
			text = S_LEAK_TEXT_OK;
			arg = p && p->dns[0] ? fmt_ip(p->dns[0], ip, sizeof(ip)) : "";
			break;
		}
	}
	/* verdict on the left, explanation right-aligned beside it */
	Span sp[6];
	int n = spans_template(tr(text), &ST_TEXT22, arg, &ST_MONO18, NULL, NULL, sp, 6, buf, sizeof(buf));
	TextStyle ws = st_col(&ST_STATE_S, wc);
	fit_size(&ws, word, CW - 300);
	float pw = CW - text_width(&ws, word) - 24;
	if (pw > 520)
		pw = 520;
	float ph = spans_para(sp, n, 0, 0, pw, 27.5f, ALIGN_RIGHT, 0);
	float rh = ph > 52 ? ph : 52;
	draw_text(&ws, M, 77 + (rh - text_height(&ws)) / 2, word);
	spans_para(sp, n, SCREEN_W - M - pw, 77 + (rh - ph) / 2, pw, 27.5f, ALIGN_RIGHT, 1);

	/* the pipe, with DNS escaping over Wi-Fi when it leaks */
	float py = 77 + rh + 12;
	PipeItem pipe[] = {
		node(120, up ? DISC_ON : DISC_OFF, -1, 0, tr(S_NODE_VITA), 0),
		line(130, NULL),
		tube(up ? TUBE_SOLID : TUBE_DASHED, tr(up ? S_TUBE_ENCRYPTED : S_TUBE_DOWN), NULL, NULL),
		node(120, up ? DISC_ON : DISC_OFF, -1, 0, tr(S_NODE_EXIT), 0),
	};
	pipe[2].tube_icon = -1;
	ui_pipe(pipe, 4, M, py, CW, &PIPE_COMPACT, NULL);
	if (leak) {
		float bx = M + 186;
		draw_dashed(bx, py + 26, 3, 68, 7, 5, C_ORANGE);
		draw_dashed(bx, py + 92, 64, 3, 7, 5, C_ORANGE);
		draw_ring(bx + 68 + 19, py + 74 + 19, 19, 2, C_ORANGE);
		TextStyle o = st_col(&ST_TEXT20, C_ORANGE);
		draw_text(&o, bx + 118, py + 81, tr(S_DNS_ESCAPES));
	}

	/* local route check and remote resolver check, side by side */
	float ty = py + 122 + 12, th = FOOT_Y - 16 - ty;
	ui_tile(M, ty, TILE_W, th, 0);
	ui_tile(M + TILE_W + 14, ty, TILE_W, th, 0);
	float lx = M + 19, rx = M + TILE_W + 14 + 19, rw = TILE_W - 38;
	draw_text(&ST_EYEBROW, lx, ty + 13, tr(S_ON_THIS_VITA));
	draw_text(&ST_EYEBROW, rx, ty + 13, tr(S_SEEN_FROM_OUTSIDE));
	TextStyle org = st_col(&ST_TEXT20, C_ORANGE), lime = st_col(&ST_TEXT20, C_LIME),
		mut = st_col(&ST_TEXT20, C_MUTED);
	int done = c->state == BK_CHECK_DONE;
	for (int i = 0; done && i < c->local_count && i < 4; i++) {
		float ry = ty + 42 + i * 32;
		draw_text(&ST_MONO18, lx, ry + 2, fmt_ip(c->local[i].ip, ip2, sizeof(ip2)));
		const TextStyle *s;
		const char *t;
		if (c->local[i].via_tunnel) {
			s = &lime;
			t = tr(S_VIA_TUNNEL_OK);
		} else if (c->dns == BK_DNS_LEAK) {
			s = &org;
			t = tr(S_VIA_WIFI_LEAKS);
		} else {
			s = &mut;
			t = tr(S_VIA_WIFI);
		}
		draw_text(s, lx + rw - text_width(s, t), ry, t);
	}
	if (!done)
		draw_text(&mut, lx, ty + 42, tr(c->state == BK_CHECK_RUNNING ? S_CHECKING : S_NOT_CHECKED));
	/* outside: the exit address, where it is and whose network, one
	 * a line, then the resolvers the remote check saw, as many as fit */
	int row = 0;
	char fit[128], loc[100];
	if (done && c->exit_ip) {
		const char *ipt = fmt_ip(c->exit_ip, ip2, sizeof(ip2));
		draw_text(&ST_MONO18, rx, ty + 44, ipt);
		if (c->isp_ip) {
			int same = c->exit_ip == c->isp_ip;
			const TextStyle *s = same ? &org : &lime;
			float room = rw - text_width(&ST_MONO18, ipt) - 16;
			const char *t = ui_fit(s, tr(same ? S_IP_SAME : S_IP_NOT_ISP), room, fit, sizeof(fit));
			draw_text(s, rx + rw - text_width(s, t), ty + 42, t);
		}
		row = 1;
		if (location(c, loc, sizeof(loc)))
			draw_text(&ST_TEXT20, rx, ty + 42 + 32 * row++, ui_fit(&ST_TEXT20, loc, rw, fit, sizeof(fit)));
		if (c->org[0])
			draw_text(&mut, rx, ty + 42 + 32 * row++, ui_fit(&mut, c->org, rw, fit, sizeof(fit)));
	}
	for (int i = 0; done && c->remote_done && i < c->remote_count && row < 4; i++, row++) {
		float ry = ty + 42 + row * 32;
		draw_text(&ST_MONO18, rx, ry + 2, fmt_ip(c->remote[i].ip, ip2, sizeof(ip2)));
		const TextStyle *s = c->remote[i].kind == BK_RES_ISP ? &org
			: c->remote[i].kind == BK_RES_VPN ? &lime : &mut;
		const char *t = tr(c->remote[i].kind == BK_RES_ISP ? S_RES_ISP
			: c->remote[i].kind == BK_RES_VPN ? S_RES_VPN : S_RES_OTHER);
		draw_text(s, rx + rw - text_width(s, t), ry, t);
	}
	if (!row)
		draw_text(&mut, rx, ty + 42, tr(c->state == BK_CHECK_RUNNING ? S_CHECKING : S_NOT_CHECKED));

	hint(a, BTN_CROSS, tr(S_HINT_RUN_AGAIN), A_LEAK_RUN, 0, 0);
	hint(a, BTN_SQUARE, tr(S_HINT_NET_SETTINGS), A_NET_SETTINGS, 0, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}

/* ---- details ----------------------------------------------------------- */

typedef struct Row {
	const char *k, *v;
	Colour c;
} Row;

/* Rows 30 px apart, closer when that many would not fit; the y of row i */
static float row_y(float y, float h, int n, int i)
{
	float step = 30;
	if (n > 1 && 42 + (n - 1) * step + 34 > h)
		step = (h - 42 - 34) / (n - 1);
	return y + 42 + i * step;
}

static void rows_tile(float x, float y, float w, float h, const char *title, const Row *r, int n)
{
	char fit[96];
	ui_tile(x, y, w, h, 0);
	draw_text(&ST_EYEBROW, x + 19, y + 13, title);
	for (int i = 0; i < n; i++) {
		float ry = row_y(y, h, n, i);
		draw_text(&ST_KEY, x + 19, ry, r[i].k);
		TextStyle st = st_col(&ST_MONO18, r[i].c ? r[i].c : C_TEXT);
		float kw = text_width(&ST_KEY, r[i].k) + 16;
		const char *v = ui_fit(&st, r[i].v, w - 38 - kw, fit, sizeof(fit));
		draw_text(&st, x + w - 19 - text_width(&st, v), ry + 2, v);
	}
}

static void toggle(float x, float y, float w, const char *key, const char *label, const char *value, int on, int action)
{
	ui_tile(x, y, w, 64, 0);
	ui_hit(x, y, w, 64, action, 0);
	/* shoulder button cap */
	TextStyle cap = st_col(&ST_EYEBROW, C_TEXT);
	cap.tracking = 0;
	float cw = 34;
	draw_rrect_stroke(x + 18, y + 20, cw, 24, 4, 1, C_MUTED);
	draw_text(&cap, x + 18 + (cw - text_width(&cap, key)) / 2, y + 20 + (24 - text_height(&cap)) / 2, key);
	draw_text(&ST_TEXT22, x + 18 + cw + 14, y + (64 - text_height(&ST_TEXT22)) / 2, label);
	TextStyle vs = st_col(&ST_TEXT22, on ? C_LIME : C_MUTED);
	draw_text(&vs, x + w - 19 - text_width(&vs, value), y + (64 - text_height(&vs)) / 2, value);
}

static const char *state_name(const BkStatus *s)
{
	if (s->locked)
		return tr(S_ST_BLOCKED);
	switch (s->state) {
	case BK_UP:
		return tr(S_ST_UP);
	case BK_HANDSHAKING:
		return tr(S_ST_HANDSHAKING);
	case BK_STALLED:
		return tr(S_ST_STALLED);
	case BK_ERROR:
		return tr(S_ST_ERROR);
	case BK_PAUSED:
		return tr(S_ST_PAUSED);
	default:
		return tr(S_ST_IDLE);
	}
}

void screen_details(App *a)
{
	char hs[96], n1[24], ep[32], tun[32], wl[32], rx[24], tx[24], ip[24];
	char file[96], folder[96], routes[64], dns[40], mtu[16], ka[24], ign[48];
	int pi = a->detail_profile >= 0 ? a->detail_profile : a->st.profile >= 0 ? a->st.profile : a->last_profile;
	const BkProfile *p = pi >= 0 && pi < a->nprof ? &a->prof[pi] : NULL;
	int is_active = p && pi == a->st.profile && a->st.state != BK_IDLE;

	Span right[1] = { { &ST_HEAD_RIGHT, p ? profile_display(p) : "" } };
	ui_header_title(tr(S_DETAILS_TITLE), right, p ? 1 : 0);

	toggle(M, 77, TILE_W, tr(S_KEY_L), tr(S_KILL_SWITCH_LABEL), tr(a->st.kill_switch ? S_ON : S_OFF),
		a->st.kill_switch, A_KILL_SWITCH);
	toggle(M + TILE_W + 14, 77, TILE_W, tr(S_KEY_R), tr(S_LOCAL_NETWORK), tr(a->st.allow_lan ? S_LAN_DIRECT : S_LAN_TUNNEL),
		0, A_ALLOW_LAN);

	float ty = 77 + 64 + 12, th = FOOT_Y - 16 - ty;
	Row t[9] = { { 0 } };
	int nt = 0, ld_row = -1;
	if (is_active || a->detail_profile < 0) {
		t[nt++] = (Row){ tr(S_ROW_STATE), state_name(&a->st), a->st.state == BK_UP && !a->st.locked ? C_LIME
			: a->st.state == BK_IDLE ? 0 : C_ORANGE };
		if (a->st.handshake_age_s == BK_NEVER) {
			t[nt++] = (Row){ tr(S_ROW_HANDSHAKE), tr(S_NEVER), 0 };
		} else {
			snprintf(n1, sizeof(n1), "%u", a->st.handshake_age_s);
			t[nt++] = (Row){ tr(S_ROW_HANDSHAKE), with_latency(&a->st,
				trf(hs, sizeof(hs), S_SECONDS_AGO, n1, NULL), hs, sizeof(hs)), 0 };
		}
		t[nt++] = (Row){ tr(S_ROW_ENDPOINT), a->st.endpoint_ip
			? fmt_ip_port(a->st.endpoint_ip, a->st.endpoint_port, ep, sizeof(ep)) : tr(S_DASH), 0 };
		snprintf(tun, sizeof(tun), "%s/%u", fmt_ip(a->st.tunnel_ip, ip, sizeof(ip)), a->st.tunnel_prefix);
		t[nt++] = (Row){ tr(S_ROW_TUNNEL_ADDR), a->st.tunnel_ip ? tun : tr(S_DASH), 0 };
		snprintf(wl, sizeof(wl), "%s/%u", fmt_ip(a->st.wlan_ip, ip, sizeof(ip)), a->st.wlan_prefix);
		t[nt++] = (Row){ tr(S_ROW_WIFI_ADDR), a->st.wlan_ip ? wl : tr(S_WIFI_OFF), 0 };
		t[nt++] = (Row){ tr(S_ROW_RECEIVED), fmt_bytes(a->st.rx_bytes, rx, sizeof(rx)), 0 };
		t[nt++] = (Row){ tr(S_ROW_SENT), fmt_bytes(a->st.tx_bytes, tx, sizeof(tx)), 0 };
		if (a->st.locked) {
			snprintf(n1, sizeof(n1), "%u", a->st.lock_drops);
			t[nt++] = (Row){ tr(S_ROW_DROPPED), n1, C_ORANGE };
		} else {
			t[nt++] = (Row){ tr(S_ROW_LAST_ERROR), a->st.last_error ? error_cause(a->st.last_error)
				: tr(S_NONE), a->st.last_error ? C_ORANGE : 0 };
		}
		if (a->st.lockdown_avail) {
			ld_row = nt;
			t[nt++] = (Row){ tr(S_ROW_LOCKDOWN), tr(a->st.lockdown_block ? S_LD_BLOCKING
				: a->st.lockdown ? S_ON : S_OFF), a->st.lockdown ? C_ORANGE : 0 };
		}
		rows_tile(M, ty, TILE_W, th, tr(S_TUNNEL), t, nt);
		if (ld_row >= 0) {
			float ry = row_y(ty, th, nt, ld_row);
			ui_hit(M, ry - 4, TILE_W, 30, A_LOCKDOWN, 0);
		}
	} else {
		/* a profile that is not running: what would stop it */
		ui_tile(M, ty, TILE_W, th, p && p->warnings);
		draw_text(&ST_EYEBROW, M + 19, ty + 13, tr(S_WARNINGS));
		float wy = ty + 42;
		char w1[96];
		TextStyle org = st_col(&ST_TEXT20, C_ORANGE), mut = st_col(&ST_TEXT20, C_MUTED);
		if (p && (p->warnings & BK_WARN_INVALID)) {
			draw_text(&org, M + 19, wy, tr(S_WARN_INVALID));
			wy += 28;
			/* the parser's own words (English), naming the line and key */
			TextStyle why = st_col(&ST_TEXT18, C_MUTED);
			Span ws[1] = { { &why, p->invalid_reason } };
			wy += spans_para(ws, 1, M + 19, wy, TILE_W - 38, 23, ALIGN_LEFT, 1) + 6;
		}
		if (p && (p->warnings & BK_WARN_CLASH)) {
			draw_text(&org, M + 19, wy, tr(S_WARN_CLASH));
			wy += 30;
		}
		if (p && (p->warnings & BK_WARN_IGNORED)) {
			snprintf(n1, sizeof(n1), "%d", p->ignored_lines);
			draw_text(&org, M + 19, wy, trf(w1, sizeof(w1),
				p->ignored_lines == 1 ? S_ROW_IGNORED_1 : S_ROW_IGNORED_N, n1, NULL));
			wy += 30;
		}
		if (!p || !p->warnings) {
			draw_text(&mut, M + 19, wy, tr(S_NO_WARNINGS));
			wy += 30;
		}
		Span sp[1] = { { &mut, tr(S_KEY_PLAIN_TEXT) } };
		spans_para(sp, 1, M + 19, wy + 8, TILE_W - 38, 26, ALIGN_LEFT, 1);
	}

	Row r[8] = { { 0 } };
	int nr = 0;
	if (p) {
		snprintf(file, sizeof(file), "%s.conf", p->name);
		r[nr++] = (Row){ tr(S_ROW_FILE), file, 0 };
		char grp[64];
		if (p->group[0])
			snprintf(grp, sizeof(grp), "%s%s", p->group, p->in_zip ? "" : "/");
		else
			snprintf(grp, sizeof(grp), "%s", tr(S_GROUP_TOP));
		trf(folder, sizeof(folder), S_JOIN, p->storage, grp);
		r[nr++] = (Row){ tr(S_ROW_FOLDER), folder, 0 };
	}
	/* an invalid profile has no settings to show, only where it is */
	if (p && !(p->warnings & BK_WARN_INVALID)) {
		r[nr++] = (Row){ tr(S_ROW_ROUTES), profile_routes(p, routes, sizeof(routes)), 0 };
		if (p->dns[0])
			fmt_ip(p->dns[0], dns, sizeof(dns));
		r[nr++] = (Row){ tr(S_ROW_DNS), p->dns[0] ? dns : tr(S_NOT_SET), p->dns[0] ? 0 : C_MUTED };
		snprintf(mtu, sizeof(mtu), "%u", p->mtu ? p->mtu : 1420);
		r[nr++] = (Row){ tr(S_ROW_MTU), mtu, 0 };
		if (p->keepalive_s) {
			snprintf(n1, sizeof(n1), "%u", p->keepalive_s);
			trf(ka, sizeof(ka), S_SECONDS, n1, NULL);
		}
		r[nr++] = (Row){ tr(S_ROW_KEEPALIVE), p->keepalive_s ? ka : tr(S_OFF), 0 };
		r[nr++] = (Row){ tr(S_ROW_CHECK), tr(p->check == BK_CHECK_URL ? S_CHECK_URL : S_CHECK_GENERIC), 0 };
		if (p->ignored_lines) {
			snprintf(n1, sizeof(n1), "%d", p->ignored_lines);
			trf(ign, sizeof(ign), p->ignored_lines == 1 ? S_LINES_1 : S_LINES_N, n1, NULL);
		}
		r[nr++] = (Row){ tr(S_ROW_IGNORED), p->ignored_lines ? ign : tr(S_NONE), p->ignored_lines ? C_ORANGE : 0 };
	}
	rows_tile(M + TILE_W + 14, ty, TILE_W, th, tr(S_PROFILE), r, nr);

	if (is_active || a->detail_profile < 0) {
		hint(a, BTN_CROSS, tr(S_HINT_LEAK_CHECK), A_OPEN, SCR_LEAK, 0);
		if (a->st.state == BK_PAUSED) {
			hint(a, BTN_TRIANGLE, tr(S_HINT_RESUME), A_RESUME, 0, 0);
		} else if (a->st.state != BK_IDLE && !a->st.lockdown) {
			/* refused under Lockdown: not offered here (the footer has
			 * no room for it beside the Lockdown hint); △ still says why */
			hint(a, BTN_TRIANGLE, tr(S_HINT_PAUSE), A_PAUSE, 0, 0);
		}
	} else {
		hint(a, BTN_CROSS, tr(S_HINT_CONNECT), A_CONNECT_SEL, 0, 0);
	}
	hint(a, BTN_SQUARE, tr(S_HINT_HELP), A_OPEN, SCR_HELP, 0);
	if (a->st.lockdown_avail)
		hint(a, BTN_START, tr(a->st.lockdown ? S_HINT_LD_OFF : S_HINT_LD_ON), A_LOCKDOWN, 0, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}

/* ---- about ------------------------------------------------------------- */

void screen_about(App *a)
{
	Span right[1] = { { &ST_CHIP, tr(S_VERSION) } };
	ui_header_title(tr(S_ABOUT_TITLE), right, 1);

	/* About is branding: the mark and the link take the brand colour (B, C) */
	Colour accent = BRAND_LEVEL >= 2 ? C_BRAND : C_LIME;
	draw_icon(ICON_MARK, M, 84, 40, 40, accent);
	draw_text(&ST_OPT_NAME, M + 56, 78, tr(S_APP_DISPLAY_NAME));
	TextStyle mut = st_col(&ST_TEXT20, C_MUTED);
	draw_text(&mut, M + 56, 112, tr(S_ABOUT_TAGLINE));

	/* A client, not a service: what people must bring */
	float y = 152;
	Span cn[1] = { { &ST_TEXT22, tr(S_CLIENT_NOTE) } };
	float h = spans_para(cn, 1, 0, 0, CW - 38, 28, ALIGN_LEFT, 0);
	float tile = 44 + h + 14;
	ui_tile(M, y, CW, tile, 0);
	draw_text(&ST_EYEBROW, M + 19, y + 15, tr(S_CLIENT_TITLE));
	spans_para(cn, 1, M + 19, y + 44, CW - 38, 28, ALIGN_LEFT, 1);

	TextStyle note = st_col(&ST_TEXT18, C_MUTED);
	Span fonts[1] = { { &note, tr(S_FONTS_CREDIT) } };
	spans_para(fonts, 1, M, y + tile + 18, CW, 24, ALIGN_LEFT, 1);
	Span lic[1] = { { &note, tr(S_LICENCE_NOTE) } };
	spans_para(lic, 1, M, y + tile + 18 + 26, CW, 24, ALIGN_LEFT, 1);

	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}

/* ---- help -------------------------------------------------------------- */

/* A tile with an eyebrow over a paragraph; returns its height (draw = 0
 * measures only) */
static float help_tile(float x, float y, float w, StrId eyebrow, StrId text, int draw)
{
	Span sp[1] = { { &ST_TEXT20, tr(text) } };
	float h = 44 + spans_para(sp, 1, 0, 0, w - 38, 26, ALIGN_LEFT, 0) + 14;
	if (draw) {
		if (draw == 1)            /* 2: the caller drew the tile (a pair of equal height) */
			ui_tile(x, y, w, h, 0);
		draw_text(&ST_EYEBROW, x + 19, y + 15, tr(eyebrow));
		spans_para(sp, 1, x + 19, y + 44, w - 38, 26, ALIGN_LEFT, 1);
	}
	return h;
}

void screen_help(App *a)
{
	ui_header_title(tr(S_HELP_TITLE), NULL, 0);

	float y = 76;
	y += help_tile(M, y, CW, S_HELP_BG_TITLE, S_HELP_BG_TEXT, 1) + 12;

	float w = (CW - 14) / 2;
	float h = help_tile(0, 0, w, S_HELP_SET_TITLE, S_HELP_SET_TEXT, 0);
	float h2 = help_tile(0, 0, w, S_HELP_QM_TITLE, S_HELP_QM_TEXT, 0);
	if (h2 > h)
		h = h2;
	ui_tile(M, y, w, h, 0);
	ui_tile(M + w + 14, y, w, h, 0);
	help_tile(M, y, w, S_HELP_SET_TITLE, S_HELP_SET_TEXT, 2);
	help_tile(M + w + 14, y, w, S_HELP_QM_TITLE, S_HELP_QM_TEXT, 2);
	y += h + 14;

	TextStyle note = st_col(&ST_TEXT18, C_MUTED);
	Span foot[1] = { { &note, tr(S_HELP_FOOT) } };
	y += spans_para(foot, 1, M, y, CW, 24, ALIGN_LEFT, 1) + 8;
	if (a->st.lockdown_avail) {
		Span ld[1] = { { &note, tr(S_HELP_LOCKDOWN) } };
		y += spans_para(ld, 1, M, y, CW, 24, ALIGN_LEFT, 1);
#ifdef HOST_BUILD
		if (y > FOOT_Y - 4)
			host_layout_problem("the Lockdown line runs into the footer");
#endif
	}

	hint(a, BTN_SQUARE, tr(S_HINT_ABOUT), A_OPEN, SCR_ABOUT, 0);
	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}

/* ---- dialogs ----------------------------------------------------------- */

void screen_dialog(App *a)
{
	char buf[1024], net[32], ip[16];
	const char *title = "";
	Span sp[6];
	int n = 0, warn = 0;
	draw_rect(0, 0, SCREEN_W, SCREEN_H, WITH_ALPHA(C_BG, 0xD0));

	if (a->dialog == DLG_CLASH && a->dialog_arg >= 0 && a->dialog_arg < a->nprof) {
		const BkProfile *p = &a->prof[a->dialog_arg];
		warn = 1;
		title = tr(S_DLG_CLASH_TITLE);
		int k = p->clash_prefix >= 0 && p->clash_prefix < p->allowed_count ? p->clash_prefix : 0;
		snprintf(net, sizeof(net), "%s/%u", fmt_ip(p->allowed[k].net, ip, sizeof(ip)), p->allowed[k].prefix);
		n = spans_template(tr(S_DLG_CLASH_TEXT), &ST_TEXT22, profile_display(p), &ST_TEXT22,
			net, &ST_MONO18, sp, 6, buf, sizeof(buf));
		hint(a, BTN_CROSS, tr(S_HINT_CONNECT_ANYWAY), A_CONNECT_ANYWAY, 0, 0);
		hint(a, BTN_CIRCLE, tr(S_HINT_CANCEL), A_DIALOG_CANCEL, 0, 1);
	} else if (a->dialog == DLG_DISCONNECT) {
		/* confirmed with the same button that asked; the other one cancels */
		int b = a->dialog_arg == BTN_CIRCLE ? BTN_CIRCLE : BTN_CROSS;
		warn = 1;
		title = tr(S_DLG_DISC_TITLE);
		/* Disconnect restores native networking, kill switch or not, unless
		 * Lockdown is on */
		n = spans_template(tr(a->st.lockdown ? S_DLG_DISC_TEXT_LD : S_DLG_DISC_TEXT), &ST_TEXT22,
			NULL, NULL, NULL, NULL, sp, 6, buf, sizeof(buf));
		hint(a, b, tr(S_HINT_DISCONNECT), A_DISCONNECT, 0, b == BTN_CIRCLE);
		hint(a, b == BTN_CROSS ? BTN_CIRCLE : BTN_CROSS, tr(S_HINT_CANCEL), A_DIALOG_CANCEL, 0,
			b == BTN_CROSS);
	} else if (a->dialog == DLG_LOCKDOWN) {
		warn = 1;
		title = tr(S_DLG_LD_TITLE);
		n = spans_template(tr(S_DLG_LD_TEXT), &ST_TEXT22, NULL, NULL, NULL, NULL, sp, 6, buf, sizeof(buf));
		hint(a, BTN_CROSS, tr(S_HINT_LD_TURN_ON), A_LOCKDOWN_ON, 0, 0);
		hint(a, BTN_CIRCLE, tr(S_HINT_CANCEL), A_DIALOG_CANCEL, 0, 1);
	} else {
		title = tr(S_DLG_KEY_TITLE);
		n = spans_template(tr(S_DLG_KEY_TEXT), &ST_TEXT22, NULL, NULL, NULL, NULL, sp, 6, buf, sizeof(buf));
		hint(a, BTN_CROSS, tr(S_HINT_OK), A_DIALOG_OK, 0, 0);
	}

	float w = 680, x = (SCREEN_W - w) / 2;
	float th = spans_para(sp, n, 0, 0, w - 56, 29, ALIGN_LEFT, 0);
	float h = 26 + 34 + 12 + th + 16 + 64 + 12;
	float y = (SCREEN_H - h) / 2;
	draw_rrect(x, y, w, h, 6, C_TILE);
	draw_rrect_stroke(x, y, w, h, 6, 2, warn ? C_ORANGE : C_LIME);
	float tx = x + 28;
	if (warn) {
		draw_icon(ICON_WARN, tx, y + 31, 24, 24, C_ORANGE);
		tx += 36;
	}
	draw_text(&ST_NAME, tx, y + 26, title);
	spans_para(sp, n, x + 28, y + 26 + 34 + 12, w - 56, 29, ALIGN_LEFT, 1);
	/* the dialog's buttons along its bottom */
	float hy = y + h - 64 - 12, hx = x + 12;
	for (int i = 0; i < a->nhints; i++)
		if (!a->hints[i].right) {
			ui_hint_draw(&a->hints[i], hx, hy);
			hx += ui_hint_width(&a->hints[i]) + 4;
		}
	hx = x + w - 12;
	for (int i = a->nhints - 1; i >= 0; i--)
		if (a->hints[i].right) {
			hx -= ui_hint_width(&a->hints[i]);
			ui_hint_draw(&a->hints[i], hx, hy);
		}
}

/* ---- QR import ----------------------------------------- */

#define CAM_X   M
#define CAM_Y   (MAIN_Y + 12)
#define CAM_H   (FOOT_Y - 12 - CAM_Y)
#define CAM_W   (CAM_H * 4.0f / 3.0f)

/* Corner marks of the framing guide: a square 85% of the frame's height */
static void guide(float x, float y, float s, Colour c)
{
	float k = s / 8, t = 3;
	for (int i = 0; i < 4; i++) {
		float cx = x + (i & 1 ? s - t : 0), cy = y + (i & 2 ? s - t : 0);
		float hx = i & 1 ? x + s - k : x, vy = i & 2 ? y + s - k : y;
		draw_rect(hx, cy, k, t, c);
		draw_rect(cx, vy, t, k, c);
	}
}

void screen_scan(App *a)
{
	ui_header_title(tr(S_SCAN_TITLE), NULL, 0);

	cam_draw(CAM_X, CAM_Y, CAM_W, CAM_H);
	draw_rrect_stroke(CAM_X, CAM_Y, CAM_W, CAM_H, 4, 1, C_RULE);
	float gs = CAM_H * 0.85f;
	/* lime while a code is in view */
	guide(CAM_X + (CAM_W - gs) / 2, CAM_Y + (CAM_H - gs) / 2, gs,
		a->scan_seen < 10 ? C_LIME : WITH_ALPHA(C_TEXT, 0xC0));
	if (a->cam_error) {
		TextStyle o = st_col(&ST_TEXT20, C_ORANGE);
		Span e[1] = { { &o, tr(S_SCAN_NO_CAMERA) } };
		spans_para(e, 1, CAM_X + 24, CAM_Y + CAM_H / 2 - 14, CAM_W - 48, 28, ALIGN_CENTRE, 1);
	}

	/* the panel beside the view: how to scan, then what happened */
	float px = CAM_X + CAM_W + 24, pw = SCREEN_W - M - px, y = CAM_Y + 4;
	float bottom = FOOT_Y - 12;
	TextStyle note = st_col(&ST_TEXT18, C_MUTED);
	Span g[1] = { { &ST_TEXT22, tr(S_SCAN_GUIDE) } };
	Span st[1] = { { &ST_SUB_MUTED, tr(S_SCAN_STEADY) } };
	Span pv[1] = { { &note, tr(S_SCAN_PRIVATE) } };
	float gh = spans_para(g, 1, 0, 0, pw, 28, ALIGN_LEFT, 0) + 10;
	float sh = spans_para(st, 1, 0, 0, pw, 27, ALIGN_LEFT, 0) + 10;
	float vh = spans_para(pv, 1, 0, 0, pw, 23, ALIGN_LEFT, 0) + 18;

	/* what happened, at the bottom of the panel */
	TextStyle why = st_col(&ST_TEXT18, C_MUTED);
	Span ws[1] = { { &why, a->scan_reason } };
	Span ns[1] = { { &ST_TEXT18, tr(S_SCAN_NOTHING_SAVED) } };
	TextStyle o = st_col(&ST_TEXT20, C_ORANGE);
	Span os[1] = { { &o, tr(S_SCAN_NOT_PROFILE) } };
	float inner = pw - 32, oh = 0, th = 0, h = 0;
	if (a->scan_invalid) {
		oh = spans_para(os, 1, 0, 0, inner - 28, 26, ALIGN_LEFT, 0);
		th = spans_para(ws, 1, 0, 0, inner, 23, ALIGN_LEFT, 0);
		h = 14 + oh + 6 + th + 6 + spans_para(ns, 1, 0, 0, inner, 23, ALIGN_LEFT, 0) + 14;
	}
	/* the guidance gives way to the reason when both do not fit */
	int show_private = y + gh + sh + vh + h <= bottom;
	int show_steady = y + gh + sh + (show_private ? vh : 0) + h <= bottom;
	spans_para(g, 1, px, y, pw, 28, ALIGN_LEFT, 1);
	y += gh;
	if (show_steady) {
		spans_para(st, 1, px, y, pw, 27, ALIGN_LEFT, 1);
		y += sh;
	}
	if (show_private) {
		spans_para(pv, 1, px, y, pw, 23, ALIGN_LEFT, 1);
		y += vh;
	}

	if (a->scan_invalid) {
		float ty = bottom - h;
		if (ty < y)
			ty = y;
#ifdef HOST_BUILD
		if (ty + h > bottom + 0.5f)
			host_layout_problem("the reason runs past the panel");
#endif
		ui_tile(px, ty, pw, h, 1);
		draw_icon(ICON_WARN, px + 16, ty + 17, 20, 20, C_ORANGE);
		spans_para(os, 1, px + 44, ty + 14, inner - 28, 26, ALIGN_LEFT, 1);
		spans_para(ws, 1, px + 16, ty + 14 + oh + 6, inner, 23, ALIGN_LEFT, 1);
		spans_para(ns, 1, px + 16, ty + 14 + oh + 6 + th + 6, inner, 23, ALIGN_LEFT, 1);
	} else if (!a->cam_error) {
		TextStyle mut = st_col(&ST_TEXT20, C_MUTED);
		draw_text(&mut, px, FOOT_Y - 12 - text_height(&mut), tr(S_SCAN_LOOKING));
	}

	hint(a, BTN_CIRCLE, tr(S_HINT_BACK), A_BACK, 0, 1);
	ui_footer(a->hints, a->nhints);
}
