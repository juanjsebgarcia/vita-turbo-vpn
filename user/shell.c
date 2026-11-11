/*
 * shell.c - the SceShell instance.
 *
 * module_start only starts one worker thread and returns: no calls into
 * other plugins, no module loads, no kernel calls. Everything waits on the
 * worker until well after SceShell has started, as Wi-Fi Turbo does: work
 * done too early in SceShell's boot can stall the whole boot (calling into
 * QuickMenuReborn from module_start did exactly that). The quick-menu rows
 * are a separate module that QuickMenuReborn loads itself (quickmenu.c).
 *
 * The worker then:
 *   1. checks for the kernel module (vtc_available); without it, or with a
 *      different API version, nothing else is started and the plugin stays
 *      inert;
 *   2. lends two threads to the kernel module: each makes one call,
 *      vtTransportRx or vtTransportTx, that does not return while the module
 *      is loaded. The receive call has a small buffer in SceShell memory, the
 *      send call needs none;
 *   3. registers SceShell with the on-screen overlay: the
 *      padlock and the HUD may draw on its frames, the HUD's words follow
 *      the system language, and the kernel's drawing is armed from here;
 *   4. starts the network-readiness reporter (its own thread, below);
 *   5. starts the notification sender and polls the status once a second
 *      for the drop alert: Connected -> Stalled or Error with the
 *      kill switch (and Lockdown) off sends one notification, and no other
 *      until the tunnel has been Connected again. The notification library
 *      itself is loaded only when the first notification is due (notify.c);
 *   6. owns the tunnel interface's address (ifaddr.c): set while the tunnel
 *      is wanted or the module holds traffic (Lockdown), removed - and the
 *      interface taken down - when it is off or paused (a
 *      Disconnect restores native networking, kill switch or not), with
 *      the interface half of the restore check around it ("ifsnap" and
 *      "restore check (interfaces, ...)" lines);
 *   7. the FTP probe (diagnostics, only while
 *      ur0:data/vpnturbo/debug_ftp exists): at those same moments and
 *      every 60 s, a TCP connect to the FTP plugin's port (1337) on 127.0.0.1
 *      and on the console's own address, waiting 3 s for the greeting, and
 *      SceShell's free memory. Its own thread, created only while the file
 *      exists (looked for at start and once a minute);
 *   8. the netctl watch (below, diagnostics, only while
 *      ur0:data/vpnturbo/debug_netwatch exists): every netctl
 *      Inet event other plugins receive, and every change of
 *      sceNetCtlInetGetState, logged on the clock kernel.log uses, with a
 *      "mark:" line for each of our own interface actions. With
 *      ur0:data/vpnturbo/debug_keep_ifaddr present (optional, off by
 *      default) the tunnel interface keeps its address and IFF_UP across
 *      Disconnect and Pause, so a reconnect makes no interface ioctl at all.
 */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <string.h>
#include <taihen.h>

#include "shell.h"
#include "notify.h"
#include "vt_client.h"
#include "log.h"
#include "ifaddr.h"
#include "resolve.h"
#include "i18n.h"

#define BOOT_DELAY_US   (10 * 1000 * 1000)   /* QuickMenuReborn waits 4-5 s too */
#define POLL_US         (1000 * 1000)
#define LEND_STACK      0x2000
#define LEND_RETRY_MAX  60      /* seconds between retries when a call returns */

/*
 * RX step pacing. The RX step never blocks, so the time between steps
 * is the latency every received datagram may wait for. Before: 2 ms after
 * every step while traffic flowed, 20 ms after 50 idle steps - each
 * datagram waited up to 2 ms plus scheduling, and every one of them is a
 * TCP segment whose ACK waits with it. Now:
 *   - a step that took datagrams (or kept one for a lock busy a moment):
 *     a 100 us nap, then the next step (the socket is drained per step, so
 *     the nap only lets SceShell's other threads in);
 *   - the first RX_FAST_STEPS empty steps after that: 500 us apart
 *     (~10 ms of quick polling covers the gaps inside a TCP flight);
 *   - then 2 ms, as before, for RX_IDLE_STEPS more; then 20 ms, as before.
 */
#define RX_NAP_DATA_US  100
#define RX_NAP_FAST_US  500
#define RX_NAP_SLOW_US  (2 * 1000)
#define RX_NAP_IDLE_US  (20 * 1000)
#define RX_FAST_STEPS   20
#define RX_IDLE_STEPS   (RX_FAST_STEPS + 50)

/*
 * The nap after a data step is what a datagram that
 * arrives just after the step waits (asked 100 us, got 0.5-1.5 ms).
 * ur0:data/vpnturbo/rx_nap_us (a number, 0-2000; read when the lent
 * RX thread starts) overrides RX_NAP_DATA_US without a
 * rebuild, for tuning; 0 = sceKernelDelayThread(0). What the naps really took is logged
 * every 30 s while data flows ("lend rx: naps ..."). Tunnel off: the kernel
 * step itself waits up to 500 ms for a change, so the 20 ms poll below no longer runs 50 times a second.
 */
#define RX_NAP_FILE     VTW_DATA_DIR "/rx_nap_us"

static unsigned rx_nap_data_us(void)
{
	char b[8] = { 0 };
	unsigned v = 0;
	SceUID fd = sceIoOpen(RX_NAP_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return RX_NAP_DATA_US;
	sceIoRead(fd, b, sizeof(b) - 1);
	sceIoClose(fd);
	if (b[0] < '0' || b[0] > '9')
		return RX_NAP_DATA_US;
	for (int i = 0; b[i] >= '0' && b[i] <= '9'; i++)
		v = v * 10 + (unsigned)(b[i] - '0');
	return v > 2000 ? 2000 : v;
}

/* What the data naps really took (lent RX thread only). Every 30 s the
 * thread hands a copy to the worker, which writes the line: a log write is
 * file I/O and must never run on the lent RX thread (a jitter spike). */
struct nap_stats { unsigned n, b[5], max, asked; uint64_t sum; };
static struct nap_stats g_nap, g_nap_pub;
static volatile int g_nap_ready;

static void nap_log_pending(void)   /* worker */
{
	if (!__atomic_load_n(&g_nap_ready, __ATOMIC_ACQUIRE))
		return;
	struct nap_stats c = g_nap_pub;
	__atomic_store_n(&g_nap_ready, 0, __ATOMIC_RELEASE);
	if (c.n)
		VTW_LOG("lend rx: naps %u asked %u us: avg %u <250 %u <500 %u <1000 %u <2000 %u more %u max %u",
		        c.n, c.asked, (unsigned)(c.sum / c.n), c.b[0], c.b[1], c.b[2], c.b[3], c.b[4], c.max);
}
static void nap_note(unsigned us)
{
	g_nap.n++;
	g_nap.sum += us;
	g_nap.b[us < 250 ? 0 : us < 500 ? 1 : us < 1000 ? 2 : us < 2000 ? 3 : 4]++;
	if (us > g_nap.max)
		g_nap.max = us;
}

static volatile int g_run = 0;
static SceUID g_worker = -1;
static SceUID g_lend[2] = { -1, -1 };
static SceUID g_netready = -1;

static const char *state_name(uint32_t s)
{
	static const char *const n[] = { "idle", "handshaking", "up", "stalled", "error", "paused", "locked" };
	return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

/* Sleep in short steps so a stop is noticed. */
static void nap_s(unsigned s)
{
	for (unsigned i = 0; i < s * 4 && g_run; i++)
		sceKernelDelayThread(250 * 1000);
}

/* ---- lent threads --------------------------------------------------- */

#define LEND_RX_BLOCK ((VT_LEND_RX_BUF_SIZE + 0xFFFu) & ~0xFFFu)   /* whole pages */

static int lend_thread(SceSize args, void *argp)
{
	int rx = (args >= sizeof(int)) ? *(const int *)argp : 0;
	const char *dir = rx ? "rx" : "tx";
	void *buf = NULL;
	SceUID blk = -1;
	/* The send call never touches its buffer (vpnturbo.h), so TX passes a
	 * few bytes of its own stack instead of a memblock (the system hands
	 * out whole pages). */
	char tx_unused[8];

	if (rx) {
		/* A user memblock is a whole number of 4 KB pages: a smaller size
		 * is refused (0x80024B05), so ask for one page; the module uses
		 * the first VT_LEND_RX_BUF_SIZE bytes of it. */
		blk = sceKernelAllocMemBlock("VtwLendRx", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
		                             LEND_RX_BLOCK, NULL);
		if (blk < 0 || sceKernelGetMemBlockBase(blk, &buf) < 0) {
			VTW_LOG("lend rx: buffer failed 0x%08X", (unsigned)blk);
			return 0;
		}
	} else {
		buf = tx_unused;
	}

	/* Each call is one short step and returns (a SceShell thread must not
	 * stay inside the module: that was followed by a restart).
	 * RX never blocks: paced as above (100 us after data, then 500 us,
	 * 2 ms, 20 ms when idle). TX waits up to 20 ms inside the module for
	 * packets to send. */
	unsigned nap_data = rx ? rx_nap_data_us() : 0;
	VTW_LOG("lend %s: stepping%s", dir, rx && nap_data != RX_NAP_DATA_US ? " (rx_nap_us file)" : "");
	if (rx)
		VTW_LOG("lend rx: %u us after a data step", nap_data);
	unsigned errs = 0, idle = 0;
	SceUInt64 nap_next = 0;
	while (g_run) {
		SceUInt64 s0 = rx ? 0 : sceKernelGetProcessTimeWide();
		int r = rx ? vtc_lend_rx(buf, VT_LEND_RX_BUF_SIZE) : vtc_lend_tx(buf, sizeof(tx_unused));
		/* TX: the module waits inside the step for work, so an empty
		 * step that came back at once is never repeated at once. This
		 * thread has SceShell's highest priority; spinning here starved
		 * SceShell's UI to a freeze during a pause. */
		if (!rx && r == 0 && sceKernelGetProcessTimeWide() - s0 < 1000)
			sceKernelDelayThread(1000);
		if (r < 0) {
			if (errs++ < 5)
				VTW_LOG("lend %s: step 0x%08X", dir, (unsigned)r);
			nap_s(1);
			continue;
		}
		if (rx) {
			idle = r > 0 ? 0 : idle + 1;
			if (r > 0) {
				SceUInt64 t0 = sceKernelGetProcessTimeWide();
				sceKernelDelayThread(nap_data);
				SceUInt64 t1 = sceKernelGetProcessTimeWide();
				nap_note((unsigned)(t1 - t0));
				if (!nap_next)
					nap_next = t1 + 30ull * 1000 * 1000;
				if (t1 >= nap_next) {
					if (!__atomic_load_n(&g_nap_ready, __ATOMIC_ACQUIRE)) {
						g_nap.asked = nap_data;
						g_nap_pub = g_nap;
						__atomic_store_n(&g_nap_ready, 1, __ATOMIC_RELEASE);
					}
					memset(&g_nap, 0, sizeof(g_nap));
					nap_next = t1 + 30ull * 1000 * 1000;
				}
			} else {
				sceKernelDelayThread(idle <= RX_FAST_STEPS ? RX_NAP_FAST_US :
				                     idle <= RX_IDLE_STEPS ? RX_NAP_SLOW_US : RX_NAP_IDLE_US);
			}
		}
	}
	if (blk >= 0)
		sceKernelFreeMemBlock(blk);
	return 0;
}

/*
 * Where the lent threads run. They inherit
 * SceShell's core, the system core (core 3), which games do not use. There
 * the TX thread used to wait behind a whole RX step (same priority, up to
 * ~10 ms), so it has the higher priority now: a datagram to send gets the
 * core at the RX step's next preemption point (worst wait ~3.5 ms, was
 * ~13). Faster sending (VT_OPT_SENDCORE, Settings) lets the TX thread run
 * on any of the game cores (0-2), so the scheduler wakes it on whichever is
 * free at that moment (device test: wakes spread evenly, worst ~3 ms, the same
 * throughput as pinning it to one core, and a game that keeps one core
 * busy is not fought there), for a share of their time while it sends.
 * Read here, once, when the threads are created: the module restarts the
 * console when the setting changes (moving the running thread can freeze
 * or crash the console).
 */
#define LEND_RX_PRIORITY  0x48
#define LEND_TX_PRIORITY  0x40
#define LEND_TX_GAME_MASK 0x70000      /* SCE_KERNEL_CPU_MASK_USER_0 | _1 | _2 */

static void lend_start(void)
{
	VtStatus st;
	int fast = vtc_get_status(&st) == 0 && (VT_ST_FEATURES(&st) & VT_FEAT_SENDCORE) &&
	           VT_ST_SENDCORE(&st);
	VTW_LOG("lend: tx thread on %s", fast ? "the game cores (faster sending)" : "the system core");
	for (int i = 0; i < 2; i++) {
		int rx = (i == 0);
		g_lend[i] = sceKernelCreateThread(rx ? "VtwTransportRx" : "VtwTransportTx",
		                                  lend_thread, rx ? LEND_RX_PRIORITY : LEND_TX_PRIORITY,
		                                  LEND_STACK, 0, rx || !fast ? 0 : LEND_TX_GAME_MASK, NULL);
		if (g_lend[i] < 0 || sceKernelStartThread(g_lend[i], sizeof(rx), &rx) < 0)
			VTW_LOG("lend %s: thread failed 0x%08X", rx ? "rx" : "tx", (unsigned)g_lend[i]);
	}
}


/* ---- network readiness after a wake (vtNetReady, vpnturbo.h) --------------------
 *
 * After a wake the kernel module makes no network call until it is told the
 * network is ready. This thread is the teller: once a second it asks the
 * module (vtNetReady(0, 0): no network call, never blocks); only while the
 * module is waiting does it read netctl's connection state, which covers
 * whichever interface netctl manages (Wi-Fi, or Ethernet on a PS TV or a USB
 * adapter). "Connected" (an address obtained) must be seen on 3 polls in a
 * row with the same address (2 if a not-connected state was seen since the
 * wait began), so a state left over from before the sleep is not taken for
 * a fresh connection. Then the address and the default route go to the
 * module. If netctl cannot be read for 30 s while waiting, the network is
 * reported ready with the address unknown, so the tunnel is not held for
 * good by a reporter that cannot see netctl. Its own thread, so a netctl
 * call that waits on the daemon (busy reconnecting) holds up nothing else.
 */
#define NETCTL_ALREADY_INITED 0x80412102u
#define NETREADY_POLL_US      (500 * 1000)

static uint32_t parse_ip4(const char *s)
{
	uint32_t ip = 0;
	for (int part = 0; part < 4; part++) {
		unsigned v = 0, digits = 0;
		while (*s >= '0' && *s <= '9' && digits < 4) {
			v = v * 10 + (unsigned)(*s++ - '0');
			digits++;
		}
		if (!digits || v > 255)
			return 0;
		ip = ip << 8 | v;
		if (part < 3 && *s++ != '.')
			return 0;
	}
	return *s == 0 ? ip : 0;
}

static uint32_t netctl_ip(int code)
{
	SceNetCtlInfo info;
	memset(&info, 0, sizeof(info));
	if (sceNetCtlInetGetInfo(code, &info) < 0)
		return 0;
	info.ip_address[sizeof(info.ip_address) - 1] = 0;
	return parse_ip4(info.ip_address);   /* the same 16 bytes for default_route */
}

static int netready_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	int inited = 0, waiting = 0, seen_down = 0, streak = 0, last_state = -2;
	unsigned bad_polls = 0, beat = 0;
	uint32_t last_ip = 0;

	while (g_run) {
		int w = vtNetReady(0, 0);
		if (w != VT_NET_WAITING) {
			if (waiting)
				VTW_LOG("network: the module is no longer waiting");
			waiting = seen_down = streak = 0;
			bad_polls = 0;
			last_state = -2;
			sceKernelDelayThread(1000 * 1000);   /* one wakeup a second (was four) */
			continue;
		}
		if (!waiting) {
			waiting = 1;
			beat = 0;
			VTW_LOG("network: the module waits for the network (after a wake)");
		}
		if (!inited) {
			int r = sceNetCtlInit();
			inited = (r >= 0 || (unsigned)r == NETCTL_ALREADY_INITED) ? 1 : 0;
			if (!inited && beat == 0)
				VTW_LOG("network: netctl init 0x%08X", (unsigned)r);
		}
		int state = -1;
		int r = inited ? sceNetCtlInetGetState(&state) : -1;
		beat++;
		if (r < 0) {
			if (bad_polls++ == 0)
				VTW_LOG("network: netctl state unreadable 0x%08X", (unsigned)r);
			if (bad_polls == 60) {   /* 30 s */
				int k = vtNetReady(VT_NET_ADDR_UNKNOWN, 0);
				VTW_LOG("network: netctl unreadable for 30 s: reporting ready, address unknown -> %d", k);
			}
			sceKernelDelayThread(NETREADY_POLL_US);
			continue;
		}
		bad_polls = 0;
		if (state != last_state) {
			VTW_LOG("network: netctl state %d", state);
			last_state = state;
		}
		uint32_t ip = state == SCE_NETCTL_STATE_CONNECTED ? netctl_ip(SCE_NETCTL_INFO_GET_IP_ADDRESS) : 0;
		if (!ip) {
			if (state != SCE_NETCTL_STATE_CONNECTED)
				seen_down = 1;
			streak = 0;
		} else {
			streak = ip == last_ip ? streak + 1 : 1;
			last_ip = ip;
		}
		/* Polls are 1 s apart here (0.5 s nap below + 0.5 s). */
		if (streak >= (seen_down ? 2 : 3)) {
			uint32_t gw = netctl_ip(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE);
			int k = vtNetReady(ip, gw);
			VTW_LOG("network: connected (address known, gateway %s): reported -> %d",
			        gw ? "known" : "unknown", k);
			if (k == VT_NET_RELEASED || k == VT_NET_IDLE) {
				waiting = 0;
				streak = 0;
			}   /* VT_NET_WAITING: too soon after the wake, report again next poll */
		} else if (beat % 20 == 0) {
			VTW_LOG("network: still waiting (netctl state %d)", state);
		}
		sceKernelDelayThread(NETREADY_POLL_US);
		sceKernelDelayThread(NETREADY_POLL_US);
	}
	return 0;
}

static void netready_start(void)
{
	g_netready = sceKernelCreateThread("VtwNetReady", netready_thread, 0xA0, 0x2000, 0, 0, NULL);
	if (g_netready < 0 || sceKernelStartThread(g_netready, 0, NULL) < 0)
		VTW_LOG("network: reporter thread failed 0x%08X", (unsigned)g_netready);
}

/* ---- netctl watch (optional diagnostics: other apps' network events) -----
 *
 * Why: other SceShell plugins (a homebrew FTP / command server among them)
 * register a netctl Inet callback, pump it with sceNetCtlCheckCallback and
 * tear their servers down whenever an event comes with
 * sceNetCtlInetGetState != 3. If connecting the tunnel made netctl emit an
 * event, or made its state read anything but 3 even briefly, this shows it,
 * with the time on the clock kernel.log uses, next to our own actions.
 *
 * Two sources:
 *   1. a callback registered with sceNetCtlInetRegisterCallback. It is NOT
 *      pumped from here: calling sceNetCtlCheckCallback would also run every
 *      other plugin's callback on this thread, at this thread's moment,
 *      which would change what those plugins do. It therefore fires in the
 *      same pump as theirs (whichever SceShell thread pumps), and records
 *      exactly the events they see. The callback does what such a callback
 *      does anyway (one sceNetCtlInetGetState) and stores the result in a
 *      small ring; this thread writes the log lines. If nothing in SceShell
 *      pumps, no event is ever recorded (the log says so once);
 *   2. sceNetCtlInetGetState polled by this thread: every 100 ms for 30 s
 *      after each of our marks, every 1 s otherwise. Every change is
 *      logged, so a short flap is caught even without an event.
 *
 * Log lines: "netctl: EVENT <type> (state <n>, sys <ms>)", "netctl: state
 * <a> -> <b> (poll, sys <ms>)", "mark: <what> (sys <ms>)".
 */
#define NW_RING                  32
#define FAST_WINDOW_MS        30000u

static volatile int g_nw_run;
static SceUID g_nw_thread = -1;
static volatile uint32_t g_fast_until;   /* sys ms; 0 = slow polling */

static struct {
	volatile uint32_t seq;           /* index + 1 once the slot is filled */
	uint32_t ms;
	int ev, st, r;
} g_ring[NW_RING];
static volatile uint32_t g_head;
static volatile uint32_t g_events;

static uint32_t vtw_sys_ms(void)
{
	return (uint32_t)((uint64_t)sceKernelGetSystemTimeWide() / 1000u);
}

static void vtw_netwatch_mark(const char *what)
{
	uint32_t now = vtw_sys_ms();
	g_fast_until = now + FAST_WINDOW_MS;
	if (!g_fast_until)
		g_fast_until = 1;
	VTW_LOG("mark: %s (sys %u ms)", what, (unsigned)now);
}

/* Runs on whichever SceShell thread pumps netctl's callbacks. */
static void on_event(int event_type, void *arg)
{
	(void)arg;
	int st = -1;
	int r = sceNetCtlInetGetState(&st);
	uint32_t i = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
	uint32_t k = i % NW_RING;
	g_ring[k].ms = vtw_sys_ms();
	g_ring[k].ev = event_type;
	g_ring[k].st = st;
	g_ring[k].r = r;
	__atomic_store_n(&g_ring[k].seq, i + 1, __ATOMIC_RELEASE);
	__atomic_fetch_add(&g_events, 1, __ATOMIC_RELAXED);
	g_fast_until = vtw_sys_ms() + FAST_WINDOW_MS;
}

static int watch_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	int cid = -1, last = -2, lastr = 0, told_none = 0;
	uint32_t tail = 0, next_poll = 0, started = vtw_sys_ms();

	int r = sceNetCtlInit();   /* in SceShell: "already initialised" */
	if (r < 0 && (unsigned)r != NETCTL_ALREADY_INITED)
		VTW_LOG("netctl watch: init 0x%08X", (unsigned)r);
	r = sceNetCtlInetRegisterCallback(on_event, NULL, &cid);
	VTW_LOG("netctl watch: callback %s (0x%08X, id %d; recorded when another plugin pumps)",
	        r >= 0 ? "registered" : "NOT registered", (unsigned)r, cid);
	if (r < 0)
		cid = -1;

	while (g_nw_run) {
		/* Events recorded by the callback, in order. */
		while ((int32_t)(__atomic_load_n(&g_head, __ATOMIC_RELAXED) - tail) > 0) {
			uint32_t k = tail % NW_RING;
			if (__atomic_load_n(&g_ring[k].seq, __ATOMIC_ACQUIRE) != tail + 1)
				break;   /* being filled (or overwritten): next round */
			if (g_ring[k].r < 0)
				VTW_LOG("netctl: EVENT %d (state unreadable 0x%08X, sys %u ms)",
				        g_ring[k].ev, (unsigned)g_ring[k].r, (unsigned)g_ring[k].ms);
			else
				VTW_LOG("netctl: EVENT %d (state %d%s, sys %u ms)", g_ring[k].ev,
				        g_ring[k].st, g_ring[k].st == SCE_NETCTL_STATE_CONNECTED ? "" :
				        " - NOT connected: plugins that react to events tear down now",
				        (unsigned)g_ring[k].ms);
			tail++;
		}
		if ((int32_t)(g_head - tail) > NW_RING) {
			VTW_LOG("netctl: %u events not logged (ring full)", (unsigned)(g_head - tail - NW_RING));
			tail = g_head - NW_RING;
		}

		uint32_t now = vtw_sys_ms();
		if (!told_none && !g_events && now - started > 120000u) {
			told_none = 1;
			VTW_LOG("netctl watch: no event recorded in 2 min (nothing pumps callbacks, or no event)");
		}
		int fast = g_fast_until && (int32_t)(g_fast_until - now) > 0;
		if (!fast)
			g_fast_until = 0;
		if (fast || (int32_t)(now - next_poll) >= 0) {
			next_poll = now + 1000u;
			int st = -1;
			int pr = sceNetCtlInetGetState(&st);
			if (pr < 0) {
				if (lastr >= 0)
					VTW_LOG("netctl: state unreadable 0x%08X (poll, sys %u ms)", (unsigned)pr,
					        (unsigned)now);
			} else if (st != last) {
				if (last == -2)
					VTW_LOG("netctl: state %d (poll, first, sys %u ms)", st, (unsigned)now);
				else
					VTW_LOG("netctl: state %d -> %d (poll%s, sys %u ms)", last, st,
					        fast ? ", fast" : "", (unsigned)now);
				last = st;
			}
			lastr = pr;
		}
		/* 100 ms only inside a fast window (it woke 10 times a
		 * second for ever). */
		sceKernelDelayThread(fast ? 100 * 1000 : 1000 * 1000);
	}
	if (cid >= 0)
		sceNetCtlInetUnregisterCallback(cid);
	return 0;
}

/* The watch is an optional diagnostic for troubleshooting network events and
 * runs only while this file exists (checked at start and
 * once a minute by the worker); the "mark:" lines are written either way. */
#define NETWATCH_FLAG VTW_DATA_DIR "/debug_netwatch"

static int netwatch_wanted(void)
{
	SceIoStat st;
	return sceIoGetstat(NETWATCH_FLAG, &st) >= 0;
}

static void vtw_netwatch_start(void)
{
	if (g_nw_thread >= 0)
		return;
	g_nw_run = 1;
	g_nw_thread = sceKernelCreateThread("VtwNetWatch", watch_thread, 0xA0, 0x2000, 0, 0, NULL);
	if (g_nw_thread < 0 || sceKernelStartThread(g_nw_thread, 0, NULL) < 0) {
		VTW_LOG("netctl watch: thread failed 0x%08X", (unsigned)g_nw_thread);
		g_nw_run = 0;
		if (g_nw_thread >= 0)
			sceKernelDeleteThread(g_nw_thread);
		g_nw_thread = -1;
	}
}

static void vtw_netwatch_stop(void)
{
	g_nw_run = 0;
	if (g_nw_thread >= 0) {
		sceKernelWaitThreadEnd(g_nw_thread, NULL, NULL);
		sceKernelDeleteThread(g_nw_thread);
		g_nw_thread = -1;
	}
}

/* ---- FTP probe (optional diagnostics: only while the debug_ftp file exists) --
 *
 * For troubleshooting FTP access to the console: tries the console's own FTP
 * server (a homebrew SceShell plugin on port 1337) over loopback
 * (127.0.0.1: no route of ours, no Wi-Fi) and over the console's own Wi-Fi
 * address. "greeting" on both while a LAN PC gets none = the problem is on
 * the LAN path; "connected, no greeting" on loopback = the server is not
 * accepting or not greeting (its own thread), whatever the network does;
 * "refused" = nothing listens any more. SceShell's free user memory is logged alongside (a
 * failed thread or socket allocation in the server would look the same).
 */
#define FTP_FLAG    VTW_DATA_DIR "/debug_ftp"
#define MODE_FILE   VTW_DATA_DIR "/iface_mode"

/* What happens to the tunnel interface when the tunnel goes off or pauses
 * (ifaddr.c says why the default keeps it up). iface_mode holds one word:
 *   keep  address and IFF_UP stay (the default; also without the file)
 *   addr  the address goes, the interface stays up
 *   down  the address goes and the interface is taken down (the old way)
 * Read at each release, so a test can switch it without a reboot. */
enum { IF_KEEP, IF_ADDR, IF_DOWN };
static int iface_mode(void)
{
	char b[8] = { 0 };
	SceUID fd = sceIoOpen(MODE_FILE, SCE_O_RDONLY, 0);
	if (fd < 0)
		return IF_KEEP;
	sceIoRead(fd, b, sizeof(b) - 1);
	sceIoClose(fd);
	if (b[0] == 'a')
		return IF_ADDR;
	if (b[0] == 'd')
		return IF_DOWN;
	return IF_KEEP;
}
#define FTP_PORT    1337
#define FTP_WAIT_US (3 * 1000 * 1000)

static volatile const char *g_probe_why;
static volatile SceUID g_probe = -1;   /* the thread; -1 = none (it ends when this is -1) */

static void ftp_probe_one(const char *label, uint32_t ip)
{
	SceNetSockaddrIn a;
	char buf[48];
	int s = sceNetSocket("vpnturbo_ftpprobe", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
	if (s < 0) {
		VTW_LOG("ftp probe %s: socket 0x%08X", label, (unsigned)s);
		return;
	}
	int tmo = FTP_WAIT_US;
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
	sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = SCE_NET_AF_INET;
	a.sin_port = __builtin_bswap16(FTP_PORT);
	a.sin_addr.s_addr = __builtin_bswap32(ip);
	SceUInt64 t0 = sceKernelGetProcessTimeWide();
	int r = sceNetConnect(s, (const SceNetSockaddr *)&a, sizeof(a));
	if (r < 0) {
		VTW_LOG("ftp probe %s: connect 0x%08X%s", label, (unsigned)r,
		        (unsigned)r == SCE_NET_ERROR_ECONNREFUSED ? " (refused: nothing listens)" : "");
		sceNetSocketClose(s);
		return;
	}
	int n = sceNetRecv(s, buf, sizeof(buf) - 1, 0);
	unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
	if (n > 0) {
		buf[n] = 0;
		for (int i = 0; i < n; i++)
			if (buf[i] == '\r' || buf[i] == '\n' || (unsigned char)buf[i] < 0x20)
				buf[i] = buf[i] ? ' ' : 0;
		buf[24] = 0;
		VTW_LOG("ftp probe %s: greeting in %u ms ('%s')", label, ms, buf);
		sceNetSend(s, "QUIT\r\n", 6, 0);
	} else {
		VTW_LOG("ftp probe %s: connected, NO greeting in %u ms (recv 0x%08X)", label, ms, (unsigned)n);
	}
	sceNetSocketClose(s);
}

static void ftp_probe(const char *why)
{
	SceKernelFreeMemorySizeInfo m;
	memset(&m, 0, sizeof(m));
	m.size = sizeof(m);
	int r = sceKernelGetFreeMemorySize(&m);
	VTW_LOG("ftp probe (%s): SceShell free memory: user %u KB, cdram %u KB, phycont %u KB (0x%08X)",
	        why, (unsigned)m.size_user / 1024, (unsigned)m.size_cdram / 1024,
	        (unsigned)m.size_phycont / 1024, (unsigned)r);
	ftp_probe_one("127.0.0.1", 0x7F000001u);
	sceNetCtlInit();   /* already initialised in SceShell: harmless */
	uint32_t me = netctl_ip(SCE_NETCTL_INFO_GET_IP_ADDRESS);
	if (me)
		ftp_probe_one("own Wi-Fi address", me);
}

/* The probe thread exists only while debug_ftp does (the netwatch pattern):
 * the worker looks for the file at start and once a minute, and a probe that
 * is asked for while there is no thread is dropped. */
static int probe_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	SceUInt64 next = 0;
	while (g_run && g_probe >= 0) {
		sceKernelDelayThread(500 * 1000);
		const char *why = (const char *)g_probe_why;
		g_probe_why = NULL;
		SceUInt64 now = sceKernelGetProcessTimeWide();
		if (why || now >= next) {
			next = now + 60ull * 1000 * 1000;
			ftp_probe(why ? why : "every 60 s");
		}
	}
	/* Deletes itself: the worker never waits for a probe that may be inside
	 * its 3 s connect timeouts. The worker looks at the flag once a minute,
	 * so a new probe thread cannot start while this one is still ending. */
	return sceKernelExitDeleteThread(0);
}

static int probe_wanted(void)
{
	SceIoStat st;
	return sceIoGetstat(FTP_FLAG, &st) >= 0;
}

static void probe_start(void)
{
	SceUID t;

	if (g_probe >= 0)
		return;
	t = sceKernelCreateThread("VtwFtpProbe", probe_thread, 0xA0, 0x2000, 0, 0, NULL);
	if (t < 0) {
		VTW_LOG("ftp probe: thread failed 0x%08X", (unsigned)t);
		return;
	}
	g_probe = t;
	if (sceKernelStartThread(t, 0, NULL) < 0) {
		VTW_LOG("ftp probe: thread failed to start");
		g_probe = -1;
		sceKernelDeleteThread(t);
	}
}

static void probe_stop(void)
{
	g_probe = -1;                       /* the thread leaves its loop and deletes itself */
	g_probe_why = NULL;
}

/* Worker: follow the flag file. */
static void probe_follow_flag(void)
{
	int want = probe_wanted();
	if (want && g_probe < 0) {
		VTW_LOG("ftp probe: on (debug_ftp)");
		probe_start();
	} else if (!want && g_probe >= 0) {
		VTW_LOG("ftp probe: off (debug_ftp removed)");
		probe_stop();
	}
}

/* A probe at an interface moment (before connect, after a disconnect); with
 * no probe thread there is nothing to do. */
static void probe_request(const char *why)
{
	if (g_probe >= 0)
		g_probe_why = why;
}

/* ---- overlay ------------------------------------------------------ */

static void overlay_boot(void)
{
	VtOverlay o;
	int r = vtc_get_overlay(&o);
	if (r == 0) {
		o.size = sizeof(o);
		o.lang = (uint8_t)vtw_i18n_lang();
		r = vtc_set_overlay(&o);
	} else {
		r = vtc_overlay_register();
	}
	VTW_LOG("overlay: SceShell registered (padlock %u, HUD %u) -> 0x%08X",
	        o.padlock_on, o.hud_on, (unsigned)r);
}

/* ---- firmware, for bug reports ------------------------------------------------ */

/* The firmware the console really runs. A version spoof changes what
 * sceKernelGetSystemSwVersion reports, but not SceShell itself, and each
 * firmware ships its own SceShell build. Nothing is gated on this: it is
 * only logged. */
static void log_shell_firmware(void)
{
	static const struct { unsigned nid; const char *ver; } builds[] = {
		{ 0x0552F692, "3.60" }, { 0x532155E5, "3.61" }, { 0xBB4B0A3E, "3.63" },
		{ 0x5549BF1F, "3.65" }, { 0x34B4D82E, "3.67" }, { 0x12DAC0F3, "3.68" },
		{ 0x0703C828, "3.69" }, { 0x2053B5A5, "3.70" }, { 0xF476E785, "3.71" },
		{ 0x939FFBE9, "3.72" }, { 0x734D476A, "3.73" }, { 0x51CB6207, "3.74" },
	};
	SceKernelSystemSwVersion sw;
	tai_module_info_t ti;

	memset(&sw, 0, sizeof(sw));
	sw.size = sizeof(sw);
	if (sceKernelGetSystemSwVersion(&sw) >= 0)
		VTW_LOG("firmware: reports %s (0x%08x)", sw.versionString, (unsigned)sw.version);
	ti.size = sizeof(ti);
	if (taiGetModuleInfo("SceShell", &ti) < 0)
		return;
	for (unsigned i = 0; i < sizeof(builds) / sizeof(builds[0]); i++)
		if (builds[i].nid == (unsigned)ti.module_nid) {
			VTW_LOG("firmware: home screen is the %s build", builds[i].ver);
			return;
		}
	VTW_LOG("firmware: home screen build 0x%08x not known", (unsigned)ti.module_nid);
}

/* ---- worker ---------------------------------------------------------------- */

static int worker(SceSize args, void *argp)
{
	(void)args; (void)argp;

	sceKernelDelayThread(BOOT_DELAY_US);
	if (!g_run)
		return 0;

	log_shell_firmware();
	int a = vtc_available();
	if (a != VTC_READY) {
		VTW_LOG("worker: module not usable (%d) - no lent threads, no drop alert", a);
		return 0;
	}

	vtw_preload_last();   /* load the last profile, do not connect */
	overlay_boot();
	lend_start();
	netready_start();
	if (netwatch_wanted())
		vtw_netwatch_start();
	else
		VTW_LOG("netctl watch: off (create debug_netwatch to turn it on)");
	vtw_notify_init();
	probe_follow_flag();   /* the probe thread only while debug_ftp exists */

	uint32_t prev = 0xFFFFFFFFu;
	int armed = 0;   /* Connected since the last alert */
	uint32_t applied_addr = 0;
	unsigned beat = 0;
	int sess = 0;            /* a connection seen: the interface before-snapshot is taken */
	int paused_checked = 0;  /* the after-pause check is done for this pause */
	int released = 0;        /* the off/paused release is done for this stop */
	int if_kept = 0;         /* the tunnel interface was left up at that release */
	static VtwIfSnap if_before, if_now;
	unsigned nw_beat = 0;
	while (g_run) {
		VtStatus st;
		if (++nw_beat % 60 == 0) {
			int want = netwatch_wanted();
			if (want && g_nw_thread < 0) {
				VTW_LOG("netctl watch: on (debug_netwatch)");
				vtw_netwatch_start();
			} else if (!want && g_nw_thread >= 0) {
				VTW_LOG("netctl watch: off (debug_netwatch removed)");
				vtw_netwatch_stop();
			}
			probe_follow_flag();
		}
		nap_log_pending();
		if (vtc_get_status(&st) == 0) {
			if (st.state != prev) {
				VTW_LOG("state %s -> %s (kill switch %s, error %u)",
				        prev == 0xFFFFFFFFu ? "-" : state_name(prev), state_name(st.state),
				        st.kill_switch ? "on" : "off", (unsigned)st.last_error);
				prev = st.state;
				beat = 0;
				vtw_netwatch_mark(state_name(st.state));
			} else if (++beat % 30 == 0 && st.state != VT_STATE_UP &&
			           st.state != VT_STATE_IDLE && st.state != VT_STATE_PAUSED) {
				/* Still alive while stuck in one state (LOCKED stays
				 * LOCKED whatever the core does): a silent log then
				 * means this worker stopped, not just no change. */
				VTW_LOG("worker: still %s (hold %u, held %u, handshake age %u s)",
				        state_name(st.state), (unsigned)st.hold, (unsigned)st.lock_drops,
				        (unsigned)st.handshake_age_s);
			}
			/* The tunnel interface's address, whoever connected (app,
			 * Settings or quick menu): set while the tunnel is wanted
			 * (not paused), or while the module holds traffic with it off
			 * (st.hold: Lockdown), so the hold routes point at a live
			 * interface. Removed, and the interface taken down, when it
			 * is off or paused with nothing held, with
			 * the interface half of the restore check around it. */
			vtw_endpoint_poll(st.endpoint_ip, st.state);   /* never blocks */
			int off = st.state == VT_STATE_IDLE || st.state == VT_STATE_PAUSED;
			int wanted = !off || st.hold != VT_HOLD_NONE;
			if (!off && !sess) {
				sess = 1;
				vtw_ifsnap_take(&if_before);
				vtw_ifsnap_log("before connect", &if_before);
				probe_request("before connect");
			}
			if (st.state != VT_STATE_PAUSED)
				paused_checked = 0;
			if (wanted && st.tunnel_address && st.tunnel_address != applied_addr) {
				if (applied_addr) {   /* another address: swap it, the interface stays up */
					vtw_netwatch_mark("ifaddr swap: SIOCDIFADDR (interface kept up)");
					vtw_ifaddr_release(1, 0);
					applied_addr = 0;
				}
				vtw_netwatch_mark("ifaddr apply: SIOCAIFADDR + IFF_UP on");
				/* Always /32: the module routes the tunnel's prefixes itself
				 * (AllowedIPs), and a wider mask would leave a connected
				 * subnet route on the kept-up interface after a Disconnect,
				 * blackholing a LAN that uses the same addresses. */
				if (vtw_ifaddr_apply(st.tunnel_address, 32) >= 0)
					applied_addr = st.tunnel_address;
				vtw_netwatch_mark("ifaddr apply done");
			} else if (!wanted && applied_addr && !released) {
				int m = iface_mode();
				released = 1;
				if_kept = m != IF_DOWN;
				if (m == IF_KEEP) {
					vtw_netwatch_mark("ifaddr kept: address and IFF_UP left on (iface_mode keep)");
				} else {
					vtw_netwatch_mark(m == IF_ADDR ? "ifaddr release: SIOCDIFADDR, interface kept up"
					                               : "ifaddr release: SIOCDIFADDR + IFF_UP off");
					vtw_ifaddr_release(1, m == IF_DOWN);
					applied_addr = 0;
					vtw_netwatch_mark("ifaddr release done");
				}
			}
			if (wanted)
				released = 0;
			if (sess && off && !paused_checked) {
				const char *what = st.state == VT_STATE_PAUSED ? "after pause" : "after disconnect";
				vtw_ifsnap_take(&if_now);
				vtw_ifsnap_log(what, &if_now);
				vtw_ifsnap_compare(what, &if_before, &if_now, if_kept);
				probe_request(what);
				if (st.state == VT_STATE_PAUSED)
					paused_checked = 1;   /* the session goes on after the resume */
				else
					sess = 0;
			}
			if (st.state == VT_STATE_UP) {
				armed = 1;
			} else if (armed && (st.state == VT_STATE_STALLED || st.state == VT_STATE_ERROR)) {
				armed = 0;
				if (!st.kill_switch && !VT_ST_LOCKDOWN(&st)) {
					VTW_LOG("drop: not protected - notifying");
					vtw_notify_drop();
				}
			} else if (st.state != VT_STATE_HANDSHAKING) {
				armed = 0;   /* off, paused or locked: not a drop */
			}
		}
		sceKernelDelayThread(POLL_US);
	}
	vtw_notify_fini();
	vtw_netwatch_stop();
	probe_stop();
	return 0;
}

void vtw_shell_start(void)
{
	g_run = 1;
	/* 8 KB (-fstack-usage, -O2): the deepest call chain is the one-off
	 * profile load at start, worker 264 + vtw_preload_last 2480 (the parsed
	 * profile and its result) + vtc_parse 2424 + vtw_log 280, about 5.5 KB
	 * before the system's own frames. Everything else the loop does stays
	 * under 1 KB. */
	g_worker = sceKernelCreateThread("VtwWorker", worker, 0x60, 0x2000, 0, 0, NULL);
	if (g_worker < 0 || sceKernelStartThread(g_worker, 0, NULL) < 0) {
		VTW_LOG("worker: thread failed 0x%08X", (unsigned)g_worker);
		g_run = 0;
	}
}

void vtw_shell_stop(void)
{
	g_run = 0;
	if (g_worker >= 0) {
		sceKernelWaitThreadEnd(g_worker, NULL, NULL);
		sceKernelDeleteThread(g_worker);
		g_worker = -1;
	}
	/* The lent threads are inside the kernel module and leave only when it
	 * lets them go; they are not waited for here. */
}
