/*
 * vpnturbo.h - control API of the vita-turbo-vpn kernel module
 * (syscall library SceVpnTurbo), shared by the module, the app
 * and the SceShell plugin.
 *
 * Every structure is fixed-size and starts with its own size, which the
 * caller sets to sizeof() before a call; the module rejects sizes it does
 * not know, so a later version can grow a structure without breaking older
 * callers. Addresses are IPv4 in host byte order.
 */
#ifndef VPNTURBO_H
#define VPNTURBO_H

#include <stdint.h>

/* The product's version, logged by every module's start banner so a log pins
 * the build; CMake reads it from here, and the .yml files carry its major and
 * minor. */
#define VT_VERSION_STR "0.1.0"

#define VT_API_VERSION 3u   /* 2: VtStatus gained the latency fields
                             * 3: VtStatus gained lock_drops, hold, route_flags
                             *    (resume lockdown, kill-switch hold, allow local) */

#define VT_KEY_LEN  32
#define VT_NAME_MAX 32  /* including the terminating NUL */

/*
 * AllowedIPs prefixes per profile. The usual profile has one (0.0.0.0/0) or
 * a few (a home LAN and the tunnel subnet). The largest common case is the
 * "exclude private addresses" list that the official clients generate and
 * many guides copy: 30 IPv4 prefixes, plus often a /32 for the DNS server.
 * 32 covers that with a spare, costs 256 bytes here, and keeps the route
 * table the module writes small. A profile with more is rejected with a
 * clear message rather than routed partially.
 */
#define VT_MAX_ALLOWED 32

/* Compile-time size checks; the layouts below are part of the ABI. */
#define VT_STATIC_ASSERT(name, cond) typedef char vt_assert_##name[(cond) ? 1 : -1]

typedef struct VtPrefix {
	uint32_t net;           /* host bits always zero */
	uint8_t  prefix;        /* 0-32 */
	uint8_t  reserved[3];
} VtPrefix;

/* VtProfile.flags */
#define VT_PROFILE_NO_ROUTES 0x0001u  /* Table = off: add no tunnel routes */

typedef struct VtProfile {
	uint32_t size;                    /* sizeof(VtProfile) */
	uint8_t  private_key[VT_KEY_LEN]; /* write-only: never returned */
	uint8_t  peer_public[VT_KEY_LEN];
	uint8_t  preshared[VT_KEY_LEN];   /* all zero = none */
	uint32_t address;                 /* tunnel address */
	uint8_t  address_prefix;
	uint8_t  allowed_count;           /* 1 to VT_MAX_ALLOWED */
	uint16_t mtu;                     /* 0 = 1420 */
	VtPrefix allowed[VT_MAX_ALLOWED];
	uint32_t endpoint_ip;             /* resolved by the app */
	uint16_t endpoint_port;
	uint16_t keepalive_s;             /* 0 = off */
	uint32_t dns[2];                  /* 0 = not managed */
	uint16_t listen_port;             /* UDP source port, 0 = random */
	uint16_t flags;                   /* VT_PROFILE_* */
	char     name[VT_NAME_MAX];       /* UTF-8, NUL-terminated */
} VtProfile;

VT_STATIC_ASSERT(prefix_size, sizeof(VtPrefix) == 8);
VT_STATIC_ASSERT(profile_size, sizeof(VtProfile) == 416);

typedef struct VtOptions {
	uint32_t size;               /* sizeof(VtOptions) */
	uint8_t  kill_switch;        /* 0 = off, the default */
	uint8_t  allow_local;        /* 1 = local subnet reached directly, the default */
	uint8_t  keepalive_override; /* 1 = use keepalive_s instead of the profile's */
	uint8_t  reserved;
	uint16_t keepalive_s;        /* with keepalive_override: 0 = off */
	uint16_t reserved2;
} VtOptions;

VT_STATIC_ASSERT(options_size, sizeof(VtOptions) == 12);

/* Tunnel states. */
enum {
	VT_STATE_IDLE = 0,
	VT_STATE_HANDSHAKING,
	VT_STATE_UP,
	VT_STATE_STALLED,
	VT_STATE_ERROR,
	VT_STATE_PAUSED,
	VT_STATE_LOCKED,   /* resume lockdown: after a wake (or a Wi-Fi
	                    * change) with the tunnel wanted, traffic is held -
	                    * routed into the tunnel and dropped there - until a
	                    * fresh handshake completes, or the user disconnects
	                    * or pauses. "Blocked: reconnecting VPN". */
};

/* VtStatus.last_error. Plain-language wording is the app's. */
enum {
	VT_ERR_NONE = 0,
	VT_ERR_BAD_PROFILE,     /* the module refused the profile */
	VT_ERR_NO_PROFILE,      /* connect without a profile */
	VT_ERR_NO_WIFI,         /* wlan0 has no address */
	VT_ERR_NO_ANSWER,       /* no handshake response: wrong key, server down or port blocked */
	VT_ERR_SOCKET,          /* transport socket could not be opened or bound */
	VT_ERR_ROUTE,           /* a route could not be added */
	VT_ERR_DENIED,          /* caller is not the app or SceShell */
};

#define VT_NEVER 0xFFFFFFFFu  /* VtStatus.handshake_age_s before the first handshake,
                               * VtStatus.latency_ms when unknown */

/* VtStatus.latency_source: where latency_ms comes from. */
enum {
	VT_LAT_NONE = 0,       /* unknown (not up, or nothing measured yet) */
	VT_LAT_HANDSHAKE,      /* round trip of the last handshake we initiated */
	VT_LAT_PING,           /* smoothed round trip of the module's own pings
	                        * through the tunnel, sent once a second while the
	                        * app is reading the status */
};

/* Mirrors the status row of the UI. Never contains key material. */
typedef struct VtStatus {
	uint32_t size;            /* sizeof(VtStatus) */
	uint32_t api_version;     /* VT_API_VERSION of the loaded module */
	uint32_t state;           /* VT_STATE_* */
	uint32_t last_error;      /* VT_ERR_* */
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint32_t rx_packets;
	uint32_t tx_packets;
	uint32_t handshake_age_s; /* seconds since the last handshake, or VT_NEVER */
	uint32_t pause_left_s;    /* seconds until a pause ends by itself */
	uint32_t endpoint_ip;
	uint16_t endpoint_port;
	uint8_t  kill_switch;
	uint8_t  allow_local;
	uint32_t tunnel_address;
	uint8_t  tunnel_prefix;
	uint8_t  wlan_prefix;
	uint16_t mtu;
	uint32_t wlan_address;    /* 0 = Wi-Fi has no address */
	uint32_t wlan_gateway;
	char     profile_name[VT_NAME_MAX];
	uint32_t latency_ms;      /* tunnel round trip, or VT_NEVER (API 2) */
	uint8_t  latency_source;  /* VT_LAT_* */
	uint8_t  reserved[3];
	uint32_t ping_loss;       /* pings lost since this connection began */
	uint32_t reserved2;       /* keeps the size a multiple of 8 (the uint64s) */
	/* API 3 */
	uint32_t lock_drops;      /* outbound packets dropped since connect because
	                           * traffic was held (VT_HOLD_*), not leaked */
	uint8_t  hold;            /* VT_HOLD_*: why traffic is held, 0 = it is not */
	uint8_t  route_flags;     /* VT_RT_*: which routes are in place */
	uint16_t reserved3;
} VtStatus;

VT_STATIC_ASSERT(status_size, sizeof(VtStatus) == 128);

/* VtStatus.hold. While held, everything the tunnel would carry is routed
 * into it and dropped there; only the tunnel's own UDP (the endpoint /32 via
 * the Wi-Fi gateway) and the Wi-Fi's own traffic (DHCP, ARP) go out. */
enum {
	VT_HOLD_NONE = 0,
	VT_HOLD_LOCKDOWN,      /* state LOCKED: after a wake or a Wi-Fi change */
	VT_HOLD_KILL_SWITCH,   /* kill switch on and the tunnel not up: connecting,
	                        * stalled, error, or disconnected with the routes
	                        * kept; turning the kill switch off releases */
};

/* VtStatus.route_flags */
#define VT_RT_ENDPOINT      0x01u  /* endpoint /32 via the Wi-Fi gateway */
#define VT_RT_TUNNEL        0x02u  /* tunnel routes (0/1 + 128/1, or AllowedIPs) */
#define VT_RT_LOCAL         0x04u  /* allow local off: the Wi-Fi subnet goes into
                                    * the tunnel too */
#define VT_RT_LOCAL_UNKNOWN 0x08u  /* allow local off asked for, but the Wi-Fi
                                    * subnet is not known yet: the LAN is still
                                    * reached directly */

/* The smallest VtStatus a caller may pass (API 2's); the module fills only
 * as many bytes as the caller's size says, so an older client is never
 * overrun and sees api_version to tell it is out of date. */
#define VT_STATUS_SIZE_V2 120u

/* Interface for vtGetRouteFor(). */
enum {
	VT_IFACE_NONE = 0,  /* no route */
	VT_IFACE_WLAN,
	VT_IFACE_TUNNEL,
};

/*
 * Lent transport threads. The SceShell plugin starts two threads at
 * boot, each of which makes one of these calls and is expected never to
 * return while the module is loaded (the plugin calls it again after every
 * step). Inside the call the module creates the UDP transport socket (so
 * SceShell's process owns it, which lets recvfrom land in that process's
 * memory), binds it, and runs the receive or send loop. A return value
 * is logged by the plugin and the call retried after a pause, so a transient
 * failure (no Wi-Fi yet) is harmless.
 *
 * Only the receive call uses its buffer: for the datagram the socket
 * delivers, for the routing-socket replies (read in at most 256 bytes; a
 * longer message is cut by the stack and dropped unparsed) and for the
 * route dump (the rest of the buffer after a 64-byte argument block; a dump
 * that does not fit ends with ENOMEM and is handled as cut short). The module
 * uses at most VT_LEND_RX_BUF_SIZE bytes whatever size the caller says, and
 * refuses a caller that says less (-VT_ERR_BUFFER), so a datagram is never
 * cut by a buffer that is too small. The send call never touches its buffer
 * (the datagrams to send wait in kernel memory); any valid pointer will do.
 */
#define VT_LEND_RX_BUF_SIZE 2048u
/* (VT_API_VERSION is unchanged: a caller that passes more still works, the
 * module only reads and writes this much.) */

/*
 * Syscalls. All return 0 or a negative error code. Control calls (all but
 * vtGetStatus and vtGetRouteFor) are accepted only from the app (VPNT00001)
 * and SceShell; the Settings app (NPXS10015) may call connect, disconnect,
 * pause, resume and the status/route reads only.
 */
#ifndef VPNTURBO_NO_PROTOTYPES
int vtSetProfile(const VtProfile *p);
int vtConnect(void);
/* The endpoint address after the caller resolved a hostname endpoint (the
 * profile carries 0 for those); restarts a pending handshake. */
int vtSetEndpoint(uint32_t ip, uint32_t port);
int vtDisconnect(void);
int vtMigrate(const VtProfile *p);
int vtSetOptions(const VtOptions *o);
int vtPause(uint32_t seconds);
int vtResume(void);
int vtGetStatus(VtStatus *s);
int vtGetRouteFor(uint32_t ip, int *iface);
int vtTransportRx(void *buf, uint32_t size);
int vtTransportTx(void *buf, uint32_t size);
#endif

/* ---- DNS override ----------------------------------------
 *
 * The module watches net_ps's one global DNS record (the system's, written
 * by the network settings after DHCP or a static setup). While the tunnel is
 * UP with a full-tunnel profile that sets DNS =, every resolver that reads
 * the record is handed the profile's servers instead; the system's own stay
 * in the record untouched, so nothing has to be put back on disconnect,
 * pause or a drop. Each process is told to flush its resolver cache once
 * after every change. While LOCKED resolvers get no server at all.
 * A process that set its own servers with sceNetSetDnsInfo is not covered.
 */
#define VT_DNS_HOOKED   0x01u  /* the module sees the DNS record (hook in place) */
#define VT_DNS_OVERRIDE 0x02u  /* effective[] are the profile's servers */
#define VT_DNS_BLOCKED  0x04u  /* lockdown: resolvers get no server */
#define VT_DNS_SYSTEM   0x08u  /* system[] holds what the system last set */

typedef struct VtDns {
	uint32_t size;          /* sizeof(VtDns) */
	uint32_t effective[2];  /* what resolvers are given now; 0 = none or unknown */
	uint32_t system[2];     /* the network's own servers (DHCP or static) */
	uint32_t profile[2];    /* the loaded profile's DNS = (0 = not managed) */
	uint32_t flags;         /* VT_DNS_* */
} VtDns;

VT_STATIC_ASSERT(dns_size, sizeof(VtDns) == 32);

/* ---- On-screen overlay: status-bar padlock and HUD ------
 *
 * One framebuffer hook, two optional overlays. The padlock is drawn centred
 * in the status bar on the home screen, LiveArea and Settings only, never
 * over a game: white while connected, Signal red while traffic is held
 * (LOCKED, or Lockdown Mode with the tunnel not up; not while paused),
 * nothing otherwise.
 * The HUD (Compact: the state; Extended: the state, TUNN:, EXIT:) is drawn in
 * the chosen corner on the same screens, and over games too when in_games
 * is set. Settings are kept in
 * ur0:data/vpnturbo/overlay.txt and read at module start.
 */
enum {
	VT_CORNER_TOP_LEFT = 0,
	VT_CORNER_TOP_RIGHT,
	VT_CORNER_BOTTOM_LEFT,
	VT_CORNER_BOTTOM_RIGHT,
};

/* VtOverlay.hud_on: the HUD mode (a saved 1 from before the modes means Compact). */
enum {
	VT_HUD_OFF = 0,
	VT_HUD_COMPACT,
	VT_HUD_EXTENDED,
};

#define VT_LANG_KEEP 0xFFu    /* VtOverlay.lang: leave the language as it is */

typedef struct VtOverlay {
	uint32_t size;          /* sizeof(VtOverlay) */
	uint8_t  padlock_on;    /* 1 = status-bar padlock (the default) */
	uint8_t  hud_on;        /* VT_HUD_*: the HUD mode (default off) */
	uint8_t  corner;        /* VT_CORNER_*, default bottom left */
	uint8_t  in_games;      /* 1 = HUD over games and apps too (default 0) */
	uint8_t  lang;          /* system language index for the HUD's words, or VT_LANG_KEEP */
	uint8_t  reserved[3];
} VtOverlay;

VT_STATIC_ASSERT(overlay_size, sizeof(VtOverlay) == 12);

#ifndef VPNTURBO_NO_PROTOTYPES
/* The effective, system and profile DNS servers (anyone). */
int vtGetDns(VtDns *d);
/* Overlay settings (app, SceShell, Settings). Also registers the caller's
 * process as a screen the padlock and HUD may draw on when it is SceShell or
 * Settings; o == NULL does only that. Nothing is drawn before the first call. */
int vtSetOverlay(const VtOverlay *o);
int vtGetOverlay(VtOverlay *o);
/* The exit address from the app's last exit-IP check for the HUD's
 * EXIT: line; 0 = no check or it failed. Cleared when the tunnel goes off. */
int vtSetExitIp(uint32_t ip);
#endif

/* ---- Network readiness after a wake (added within API 3) -----------------
 *
 * After a wake (and after a suspend called off past the point where net_ps
 * suspended itself) the module makes NO network call at all - no socket
 * close or reopen, no route message, no send - until it is told the network
 * is ready, because work done while the network driver and netctl were
 * still resuming can stall net_ps.
 * Tunnel traffic stays held meanwhile (VT_STATE_LOCKED when it was armed).
 *
 * The SceShell plugin is the reporter: it calls vtNetReady(0, 0) once a
 * second (a query, which also tells the module a reporter exists) and, while
 * the answer is VT_NET_WAITING and netctl's connection state is "connected"
 * (an address obtained, on whichever interface netctl manages: Wi-Fi, or
 * Ethernet on a PS TV or a USB adapter), passes that address and netctl's
 * default route. Only then are the sockets rebuilt, the routes checked and
 * the handshake started. If no reporter has ever called, the module ends
 * the wait by itself 20 s after the wake (an older plugin).
 */
enum {
	VT_NET_IDLE = 0,       /* not waiting: nothing to report */
	VT_NET_WAITING,        /* waiting for the network; report it when connected */
	VT_NET_RELEASED,       /* report accepted: the wake work starts now */
};

/* vtNetReady's address when the reporter cannot read netctl's: the network
 * is taken as ready with the address and gateway unknown (learnt from the
 * routing table instead). */
#define VT_NET_ADDR_UNKNOWN 0xFFFFFFFFu

#ifndef VPNTURBO_NO_PROTOTYPES
/* address == 0: query (VT_NET_IDLE or VT_NET_WAITING). Otherwise netctl
 * reports connected with this address and default gateway (host order,
 * gateway 0 = not known): VT_NET_RELEASED if the wait ended, VT_NET_WAITING
 * if it is too early after the wake (ask again), VT_NET_IDLE if there was no
 * wait. App and SceShell only. */
int vtNetReady(uint32_t address, uint32_t gateway);
#endif


/* ---- Lockdown mode, and Disconnect restores everything (within API 3) ---
 *
 * No structure changes size and no field moves: the bytes below were reserved (zero)
 * before, so an older client keeps working and simply never asks for
 * Lockdown. Check VT_FEAT_LOCKDOWN in the status before offering it.
 *
 * Disconnect, with Lockdown off, returns the console to native networking:
 * no route of the module's, the tunnel interface down without an address
 * (the SceShell plugin removes it), the DNS record passed through
 * untouched, no session, empty queues, nothing held. The kill switch
 * matters only while the tunnel is wanted (connecting, dropped,
 * reconnecting, after a wake); turning it on while disconnected changes
 * nothing until the next connect.
 *
 * Lockdown on (a setting, kept across reboots with the kill switch and
 * allow local in ur0:data/vpnturbo/options.txt): only tunnel traffic
 * may leave. Whenever the tunnel is not wanted (disconnected, or the console
 * booted with it off) everything is routed into the tunnel interface and
 * dropped there (VT_HOLD_LOCKDOWN_MODE); the Wi-Fi's own DHCP and ARP, and
 * the LAN when allow local is on, stay direct. It implies the kill
 * switch while the tunnel is wanted. Pause is refused (VT_ERR_LOCKDOWN).
 * Needs a loaded profile (its tunnel address; the plugin preloads the last
 * one at boot). Recovery: turn it off (app, Settings), hold L at boot
 * (no plugins load), or create ur0:data/vpnturbo/lockdown_off: at the
 * next module start Lockdown is turned off, saved, and the file deleted.
 */

/* VtOptions.reserved: the Lockdown request. */
#define VT_LOCKDOWN_KEEP 0u   /* leave it as it is (every older caller) */
#define VT_LOCKDOWN_OFF  1u
#define VT_LOCKDOWN_ON   2u
#define VT_OPT_LOCKDOWN(o)  ((o)->reserved)

/*
 * Faster sending. Where the SceShell plugin runs the
 * tunnel's sending thread: by default on the system core, with the
 * receiving thread (core 3; the sending one has the higher priority, so
 * a datagram to send interrupts a receive step); on: on any of the cores
 * games use (0-2, whichever is free when it wakes), so it never waits for
 * the receive step or the system's own network work - lower and steadier
 * send latency and a little more throughput, for a share of those cores'
 * time while it sends (small for game streaming, whose traffic is mostly
 * received). Kept in options.txt and applied when the plugin creates the
 * thread, at boot: a change restarts the console (the module shows a
 * "restarting" banner for 3 s, then restarts it; moving the running thread
 * can freeze or crash the console). The Settings app
 * may change it (as Lockdown).
 */
#define VT_SENDCORE_KEEP   0u   /* leave it as it is (every older caller) */
#define VT_SENDCORE_SYSTEM 1u
#define VT_SENDCORE_GAME   2u
#define VT_OPT_SENDCORE(o)  ((o)->reserved2)

/* VtStatus.reserved[0]: the Lockdown setting (0 off, 1 on).
 * VtStatus.reserved3: VT_FEAT_* this module has. */
#define VT_ST_LOCKDOWN(s)   ((s)->reserved[0])
/* VtStatus.reserved[2]: faster sending (0 off, 1 on; VT_FEAT_SENDCORE). */
#define VT_ST_SENDCORE(s)   ((s)->reserved[2])
#define VT_ST_FEATURES(s)   ((s)->reserved3)
#define VT_FEAT_LOCKDOWN    0x0001u  /* the Lockdown request above is understood */
#define VT_FEAT_RESTORE     0x0002u  /* Disconnect restores native networking */

/* VtStatus.hold: Lockdown with the tunnel not wanted ("Lockdown: traffic
 * blocked"). In effect once route_flags has VT_RT_TUNNEL; until then (the
 * interface waits for its address) traffic is not yet blocked. */
#define VT_HOLD_LOCKDOWN_MODE 3u

/* vtPause's result while Lockdown is on (negated, as every error). */
#define VT_ERR_LOCKDOWN 8u

/* vtSetOptions callers: the app and SceShell may set every field; the
 * Settings app (NPXS10015) may change Lockdown only (its kill_switch and
 * allow_local are ignored). */


/* ---- Lent threads: who may lend (within API 3) ---------------------------
 *
 * vtTransportRx / vtTransportTx are accepted only from SceShell (title id
 * "main"), and only from the first SceShell process that called (pinned
 * until it is gone); anyone else gets -VT_ERR_DENIED. One call per
 * direction at a time: a second concurrent one gets -VT_ERR_BUSY. The
 * plugin's retry after a failed step covers both.
 */
#define VT_ERR_BUSY 9u

/* vtTransportRx: the buffer is smaller than VT_LEND_RX_BUF_SIZE. */
#define VT_ERR_BUFFER 11u

/* vtConnect: no SceShell thread has been lent this boot, a minute or more
 * after boot (the plugin lends them about 15 s in). Without them nothing
 * carries the tunnel's packets, so the connect is refused instead of
 * handshaking forever. The usual cause is the plugin's boot guard: after
 * two starts that did not finish it stays inert for the boot (the app can
 * see ur0:data/vpnturbo/bootguard), otherwise the *main line of
 * config.txt. Also reported as VT_NOTE_NO_SHELL. */
#define VT_ERR_NO_SHELL 10u


/* ---- DNS while traffic is held (within API 3) -----------------------------
 *
 * While traffic is held (VT_HOLD_KILL_SWITCH, VT_HOLD_LOCKDOWN_MODE, or the
 * tunnel routes kept while it re-handshakes) no resolver is given a server
 * outside the tunnel: the profile's DNS = if every one is routed into the
 * tunnel, otherwise none (VT_DNS_BLOCKED). LOCKED stays "none". The
 * exception is the endpoint lookup: the app (VPNT00001) and the one
 * SceShell thread that registered with
 *     vtSetEndpoint(0, VT_EP_LOOKUP_THREAD)
 * (SceShell only; it replaces an earlier registration) keep the system's
 * servers while held, never while LOCKED. Before this, vtSetEndpoint
 * refused ip 0, so an older module answers -VT_ERR_BAD_PROFILE.
 */
#define VT_EP_LOOKUP_THREAD 0x10000u

/* VtStatus.route_flags: the profile's DNS server(s), which sit in the
 * Wi-Fi subnet, are routed into the tunnel (a /31 each) rather than
 * reached directly through the LAN route (allow local on). */
#define VT_RT_DNS 0x10u


/* ---- Further status bits (within API 3) -----------------------------------
 *
 * No structure changes size and no field moves; the bytes used were
 * reserved (zero) before. Check the VT_FEAT_* bit before relying on one.
 */

/* VtStatus.route_flags: traffic is to be held but the tunnel interface
 * cannot take it yet (a connect with the kill switch, Lockdown at boot):
 * the tunnel's destinations are REJECT routes via the loopback meanwhile,
 * so senders fail at once ("no route") instead of leaving directly. Counts
 * as "the block is in effect" wherever VT_RT_TUNNEL does (VtStatus.hold,
 * VT_HOLD_LOCKDOWN_MODE). Replaced by VT_RT_TUNNEL once the interface has
 * its address. */
#define VT_RT_BLOCK 0x20u

/* VtStatus.reserved[1]: VT_NOTE_* bits, conditions the UI should explain. */
#define VT_ST_NOTES(s)      ((s)->reserved[1])
/* Lockdown is on and the loaded profile's endpoint is a hostname the module
 * has no address for yet (no vtSetEndpoint since the profile was loaded).
 * Under Lockdown resolvers reach only a LAN (allow local on) or gateway
 * resolver, and the plugin's endpoint cache covers a profile seen before;
 * a first connect with an off-LAN resolver cannot resolve. Suggested
 * wording: "Can't look up the server's name while Lockdown is on. Turn
 * Lockdown off once to connect; the address is then remembered." */
#define VT_NOTE_EP_UNRESOLVED 0x01u
/* No SceShell thread lent this boot, a minute or more after boot: a
 * connect would be refused with VT_ERR_NO_SHELL (see there). */
#define VT_NOTE_NO_SHELL 0x02u

/* VtStatus.reserved3 (VT_ST_FEATURES): the two above exist. */
#define VT_FEAT_BLOCK_ROUTES 0x0004u
#define VT_FEAT_NOTES        0x0008u
#define VT_FEAT_NO_SHELL     0x0010u   /* VT_ERR_NO_SHELL, VT_NOTE_NO_SHELL */
#define VT_FEAT_SENDCORE     0x0020u   /* VT_OPT_SENDCORE, VT_ST_SENDCORE */

#endif
