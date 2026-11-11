/*
 * vt_client_stub.c - vtc_* with scripted states and no kernel module, for
 * exercising the Settings page, the quick menu and the drop notification
 * on a console without the kernel module (CMake option VT_CLIENT_STUB).
 *
 * The "tunnel" is a small file both instances share, so a toggle in the
 * quick menu shows in Settings and the other way round:
 *   ur0:data/vpnturbo/stub_state.bin
 * Times are system time (shared by every process), in microseconds.
 *
 * Script, from the moment the toggle is switched on:
 *     0-4 s    Connecting
 *     4-60 s   Connected
 *    60-75 s   Not protected (stalled, kill switch off: fires the drop alert)
 *    75-79 s   Connecting
 *    79-120 s  Connected
 *   120-135 s  Blocked: reconnecting VPN (the lockdown wording)
 * then from 4 s again. Pause shows Paused with the minutes left for five
 * minutes (or until Resume now), then the script restarts at Connecting.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "vt_client.h"
#include "vtc_ext.h"
#include "log.h"

#define STUB_PATH  VTW_DATA_DIR "/stub_state.bin"
#define STUB_MAGIC 0x42545356u   /* "VSTB" */
#define SEC        1000000ll

typedef struct {
	uint32_t magic;
	uint32_t wanted;
	int64_t  t0;          /* when the script (re)started */
	int64_t  pause_end;   /* 0 = not paused */
	uint32_t lockdown;    /* Lockdown mode: blocks while off, refuses Pause */
	uint32_t pad;
} StubState;

static void load(StubState *s)
{
	memset(s, 0, sizeof(*s));
	SceUID fd = sceIoOpen(STUB_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		if (sceIoRead(fd, s, sizeof(*s)) != (int)sizeof(*s) || s->magic != STUB_MAGIC)
			memset(s, 0, sizeof(*s));
		sceIoClose(fd);
	}
}

static int save(const StubState *s)
{
	sceIoMkdir("ur0:data", 0777);
	sceIoMkdir(VTW_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(STUB_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int n = sceIoWrite(fd, s, sizeof(*s));
	sceIoClose(fd);
	return n == (int)sizeof(*s) ? 0 : -1;
}

static int64_t now_us(void)
{
	return (int64_t)sceKernelGetSystemTimeWide();
}

int vtc_is_stub(void)
{
	return 1;
}

int vtc_available(void)
{
	return VTC_READY;
}

int vtc_get_status(VtStatus *st)
{
	StubState s;
	int64_t now = now_us();

	memset(st, 0, sizeof(*st));
	st->size = sizeof(*st);
	st->api_version = VT_API_VERSION;
	st->handshake_age_s = VT_NEVER;
	st->latency_ms = VT_NEVER;
	st->allow_local = 1;
	st->mtu = 1420;
	strncpy(st->profile_name, "Test profile", sizeof(st->profile_name) - 1);

	load(&s);
	VT_ST_FEATURES(st) = (uint16_t)(VT_FEAT_LOCKDOWN | VT_FEAT_RESTORE);
	VT_ST_LOCKDOWN(st) = (uint8_t)(s.lockdown != 0);
	if (!s.wanted) {
		st->state = VT_STATE_IDLE;
		if (s.lockdown) {
			st->hold = VT_HOLD_LOCKDOWN_MODE;
			st->route_flags = VT_RT_TUNNEL;
		}
		return 0;
	}
	if (s.pause_end > now) {
		st->state = VT_STATE_PAUSED;
		st->pause_left_s = (uint32_t)((s.pause_end - now + SEC - 1) / SEC);
		return 0;
	}

	int64_t t0 = s.pause_end ? s.pause_end : s.t0;   /* a pause that ran out */
	int64_t e = (now - t0) / SEC;
	if (e < 0)
		e = 0;
	if (e < 4) {
		st->state = VT_STATE_HANDSHAKING;
		return 0;
	}
	int64_t c = (e - 4) % 131;
	if (c < 56) {
		st->state = VT_STATE_UP;
		st->handshake_age_s = (uint32_t)(c % 120);
	} else if (c < 71) {
		st->state = VT_STATE_STALLED;
		st->last_error = VT_ERR_NO_ANSWER;
		st->handshake_age_s = (uint32_t)(180 + c - 56);
	} else if (c < 75) {
		st->state = VT_STATE_HANDSHAKING;
	} else if (c < 116) {
		st->state = VT_STATE_UP;
		st->handshake_age_s = (uint32_t)(c - 75);
	} else {
		st->state = VT_STATE_LOCKED;
	}
	if (st->state == VT_STATE_UP) {
		st->latency_ms = 23;
		st->latency_source = VT_LAT_PING;
	}
	st->rx_packets = (uint32_t)(e * 40);
	st->tx_packets = (uint32_t)(e * 35);
	st->rx_bytes = (uint64_t)st->rx_packets * 900u;
	st->tx_bytes = (uint64_t)st->tx_packets * 300u;
	return 0;
}

int vtc_connect(void)
{
	StubState s;
	load(&s);
	s.magic = STUB_MAGIC;
	s.wanted = 1;
	s.t0 = now_us();
	s.pause_end = 0;
	VTW_LOG("stub: connect");
	return save(&s);
}

int vtc_disconnect(void)
{
	StubState s;
	load(&s);
	s.magic = STUB_MAGIC;
	s.wanted = 0;
	s.t0 = s.pause_end = 0;
	VTW_LOG("stub: disconnect");
	return save(&s);
}

int vtc_pause(uint32_t seconds)
{
	StubState s;
	load(&s);
	if (!s.wanted)
		return -1;
	if (s.lockdown)
		return -(int)VT_ERR_LOCKDOWN;
	s.pause_end = now_us() + (int64_t)seconds * SEC;
	VTW_LOG("stub: pause %u s", (unsigned)seconds);
	return save(&s);
}

int vtc_resume(void)
{
	StubState s;
	load(&s);
	if (!s.wanted)
		return -1;
	s.t0 = now_us();
	s.pause_end = 0;
	VTW_LOG("stub: resume");
	return save(&s);
}

/* Lent threads: block as the real calls do, without doing anything. */
static int lend_forever(void)
{
	for (;;)
		sceKernelDelayThread(60 * 1000 * 1000);
	return 0;
}

int vtc_lend_rx(void *buf, uint32_t size)
{
	(void)buf; (void)size;
	return lend_forever();
}

int vtc_lend_tx(void *buf, uint32_t size)
{
	(void)buf; (void)size;
	return lend_forever();
}

/* Preload is a no-op in the scripted build, but vtw_preload_last() still
 * references vtSetProfile; provide a harmless stand-in so the stub build links.
 * vtw_preload_last() returns early via vtc_is_stub() before ever calling it. */
int vtSetProfile(const VtProfile *p)
{
	(void)p;
	return -1;
}

/* Likewise for the endpoint resolver (resolve.c): no host is ever set in the
 * scripted build, so this is never reached. */
int vtSetEndpoint(uint32_t ip, uint32_t port)
{
	(void)ip; (void)port;
	return -1;
}

/* The network-readiness reporter (shell.c): the scripted build never waits. */
int vtNetReady(uint32_t address, uint32_t gateway)
{
	(void)address; (void)gateway;
	return VT_NET_IDLE;
}

/* Overlay settings: kept in a small file both instances share, so
 * a switch in Settings shows in the quick menu. Nothing is drawn in the
 * scripted build (the drawing is the kernel module's). */
#define STUB_OVL_PATH VTW_DATA_DIR "/stub_overlay.bin"

int vtc_get_overlay(VtOverlay *o)
{
	memset(o, 0, sizeof(*o));
	SceUID fd = sceIoOpen(STUB_OVL_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		int n = sceIoRead(fd, o, sizeof(*o));
		sceIoClose(fd);
		if (n == (int)sizeof(*o) && o->size == sizeof(*o))
			return 0;
	}
	memset(o, 0, sizeof(*o));            /* the module's defaults */
	o->size = sizeof(*o);
	o->padlock_on = 1;
	o->corner = VT_CORNER_BOTTOM_LEFT;
	o->lang = 1;
	return 0;
}

int vtc_set_overlay(const VtOverlay *o)
{
	sceIoMkdir("ur0:data", 0777);
	sceIoMkdir(VTW_DATA_DIR, 0777);
	SceUID fd = sceIoOpen(STUB_OVL_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return fd;
	int n = sceIoWrite(fd, o, sizeof(*o));
	sceIoClose(fd);
	VTW_LOG("stub: overlay padlock=%u hud=%u corner=%u games=%u",
	        o->padlock_on, o->hud_on, o->corner, o->in_games);
	return n == (int)sizeof(*o) ? 0 : -1;
}

int vtc_overlay_register(void)
{
	return 0;
}

int vtc_set_lockdown(int on)
{
	StubState s;
	load(&s);
	s.magic = STUB_MAGIC;
	s.lockdown = on ? 1 : 0;
	if (on)
		s.pause_end = 0;   /* Lockdown ends a pause */
	VTW_LOG("stub: lockdown %s", on ? "on" : "off");
	return save(&s);
}

int vtc_set_sendcore(int fast)
{
	VTW_LOG("stub: faster sending %s (not kept)", fast ? "on" : "off");
	return 0;
}
