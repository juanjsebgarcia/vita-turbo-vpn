/*
 * tn_proto.h - the protocol layer of vpnturbo_kernel, over the OpenBSD
 * core in core/.
 *
 * This is the only part of the module that talks to the core, and it talks
 * to nothing Vita-specific: it compiles unchanged for the host (desk tests,
 * kernel/host_test) and for the Vita kernel. It owns the single
 * peer's noise_local / noise_remote / cookie state, the index table, the
 * OpenBSD-style timers and the state machine; the rest of the module gives
 * it two sinks (send a UDP datagram, deliver a decrypted inner packet) and
 * feeds it inbound datagrams and outbound inner packets.
 *
 * All buffers are caller-owned. Thread context only (the core uses kernel
 * mutexes and the RNG). Never logs key material or packet contents.
 */
#ifndef TN_PROTO_H
#define TN_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Tunnel state, mirrors VT_STATE_* in include/vpnturbo.h (kept as plain ints so
 * this header needs nothing from vpnturbo.h). */
enum {
	TNP_IDLE = 0,
	TNP_HANDSHAKING,
	TNP_UP,
	TNP_STALLED,
	TNP_ERROR,
};

/* AllowedIPs entry, host byte order, host bits zero. */
struct tnp_prefix {
	uint32_t net;
	uint8_t  prefix;
};

struct tnp_config {
	uint8_t  private_key[32];
	uint8_t  peer_public[32];
	uint8_t  preshared[32];        /* all zero = none */
	uint32_t tunnel_address;       /* host order, the interface's own address */
	struct tnp_prefix allowed[32];
	unsigned allowed_count;
	uint16_t keepalive_s;          /* 0 = off */
};

/*
 * Sinks the platform supplies (tnp_set_sinks):
 *   send    transmit one UDP datagram to the current endpoint; >=0 ok.
 *   deliver hand a decrypted inner IPv4 packet (14-byte-stripped, no Ethernet
 *           header) up to the interface.
 * Both run on the thread that called into the protocol layer (a lent thread,
 * the TX thread or the timer thread). send must not block on the protocol
 * locks; it just queues/sends on the transport socket.
 */
typedef int  (*tnp_send_fn)(void *ctx, const void *buf, uint32_t len);
typedef void (*tnp_deliver_fn)(void *ctx, const void *ip, uint32_t len);

/* Optional: the platform is told the source address of every handshake
 * response that completed our latest initiation (the only roam, proto.c
 * ROAMING); data, keepalives, cookie replies and initiations never call it.
 * The platform decides whether the address is a change. Called with the
 * protocol lock held: it must not block or call back into tnp_*. */
typedef void (*tnp_roam_fn)(void *ctx, uint32_t ip, uint16_t port);

int  tnp_init(void);     /* tnshim_init + one-time state; before any other call */
void tnp_fini(void);

void tnp_set_sinks(tnp_send_fn send, tnp_deliver_fn deliver, tnp_roam_fn roam,
	void *ctx);

/* Load the single peer. Returns 0; -1 if a key is refused (zero, or a peer
 * key with a zero shared secret), checked before anything is touched, so the
 * loaded peer and its session stay as they were; -2 if a later step failed
 * after the old peer was cleared (the core is then unconfigured). Wiping
 * cfg's copy of the keys is the caller's job.
 *
 * Locking: every tnp_* call except tnp_get_stats, tnp_set_sinks and
 * tnp_trace_timers takes the protocol lock (the core's recursive rwlock
 * lock; proto.c "THE PROTOCOL LOCK"); no net_ps call and no sink that could
 * block runs under it, and the deliver sink runs after it is released.
 * tnp_input must have one caller at a time. */
int  tnp_set_config(const struct tnp_config *cfg);

/* Drop keys and session (vtDisconnect / profile change). */
void tnp_clear(void);

/* The tunnel is no longer wanted (vtDisconnect): drop the session keys and
 * the handshake state, stop the timers, state IDLE. The peer's static
 * configuration stays, so a later tnp_start_handshake reconnects. */
void tnp_stop(void);

/* Force a fresh handshake even when a session is up (wake, Wi-Fi change,
 * migrate): the state goes to HANDSHAKING so the retransmit timer runs,
 * and the current session (if any) keeps carrying data until it is
 * replaced. A second call within a second of the last initiation is
 * ignored. */
void tnp_rehandshake(void);

/* 1 if priv is the private key of the loaded configuration (constant time;
 * used by vtMigrate to tell a same-key migration). */
int  tnp_same_private(const uint8_t priv[32]);

/* Log the next n timer actions (resends, keepalives; rekeys are always
 * logged), so the log shows the timers running again after a wake. */
void tnp_trace_timers(unsigned n);

/* Begin (or restart) a handshake: connect, resume, a pause ending. The
 * state goes to HANDSHAKING (unless UP) and the attempt count restarts;
 * the initiation goes out now unless one went out in the last
 * REKEY_TIMEOUT (then the retransmit timer sends it). */
void tnp_start_handshake(void);

/* Encrypt one outbound inner IPv4 packet and send it. Returns 0 on success,
 * <0 if there is no usable session; the packet is then dropped by the
 * caller, and the core itself starts a handshake if none is running (state
 * UP with an expired keypair -> HANDSHAKING), at most one initiation per
 * REKEY_TIMEOUT. The caller must not call tnp_start_handshake for it. */
int  tnp_encrypt_send(const void *ip, uint32_t len);

/* Feed one inbound UDP datagram from the transport. Dispatches by type. */
void tnp_input(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port);

/* Call about every 250 ms from the timer thread: retransmit, keepalive,
 * rekey, expiry. Returns the current state (TNP_*). */
int  tnp_timer_tick(void);

/* Current state and stats for vtGetStatus. handshake_age_s is seconds since
 * the last completed handshake, or 0xFFFFFFFF if never. handshake_rtt_ms is
 * the round trip of the last handshake we initiated (initiation sent to its
 * response processed), or 0xFFFFFFFF if none was measured since the profile
 * was loaded. */
struct tnp_stats {
	int      state;
	uint32_t handshake_age_s;
	uint64_t rx_bytes, tx_bytes;
	uint32_t rx_packets, tx_packets;
	uint32_t handshake_rtt_ms;
	uint32_t handshakes;      /* handshakes completed since tnp_init (monotonic;
	                           * the lockdown waits for this to move) */
};
/* Takes no lock (any thread, including hooks that must never wait). */
void tnp_get_stats(struct tnp_stats *s);

/* 1 if a packet's inner source (host order) is inside AllowedIPs. */
int  tnp_allowed(uint32_t ip);

/*
 * Never-waiting entries (proto.c "the gate"). Each
 * does what its waiting twin does when the protocol lock is free, and
 * returns -1 at once, having done nothing, when another thread holds it:
 *   tnp_input_try       the lent RX step (keeps the datagram, feeds it again
 *                       on its next step); 0 = processed;
 *   tnp_rehandshake_try the lent RX step's route work; 0 = done;
 *   tnp_timer_tick_try  the timer thread; the state (>= 0) when it ran.
 * Every waiting entry logs a watchdog line per second it waits, naming the
 * holder. tnp_lock_holder: the tnp_* entry holding the lock now and for how
 * long, or NULL when it is free (lock-free; for the platform's own lines).
 */
int  tnp_input_try(const void *buf, uint32_t len, uint32_t from_ip, uint16_t from_port);
int  tnp_rehandshake_try(void);
int  tnp_timer_tick_try(void);
const char *tnp_lock_holder(uint32_t *held_ms);

/*
 * Logging hook, supplied by the platform (klog.c on the Vita, stderr on the
 * host). State changes and errors only; never keys or packet contents.
 */
void tnp_log(const char *fmt, ...);

#endif /* TN_PROTO_H */
