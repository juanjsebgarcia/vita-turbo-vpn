/*
 * iface_rearm.h - the interface TX thread's per-wake decision (iface.c), as
 * pure functions so the host test (host_test/rearm_test.c) can check it.
 *
 * WHY (device test): after a sleep and wake with the SceShell plugin keeping
 * other0 UP across disconnects, no frame ever reached the driver again,
 * while the kernel's own ping (injected straight into the core) worked.
 * desc[0x10] init is no longer called on a connect (no IFF_UP toggle), so
 * the only link-up the interface ever got was the one at the first init.
 * net_ps sets IFF_RUNNING (and clears IFF_OACTIVE) only from the driver's
 * link-up (desc[9] soft bit 1); ether_output refuses a frame with ENETDOWN
 * unless UP|RUNNING, and if_start is not called while OACTIVE. The Wi-Fi
 * driver sends a link-up again after every wake (it re-associates); ours
 * did not. So the re-arm sends one.
 *
 * The re-arm (a wake's network-ready, every connect, a resume from pause):
 *   - desc[9] soft bit 1 (link up) from the TX thread, thread context,
 *     nothing held: sets IFF_RUNNING, clears IFF_OACTIVE (net_ps's soft
 *     thread applies it). desc[9] takes no lock;
 *   - one desc[8] dequeue pass, as for a kick, in case a kick was consumed
 *     while the thread was quiet (a kick that arrives while TN_QUIET() is
 *     remembered and served when the quiet ends; it used to be dropped).
 * Neither runs while TN_QUIET() (suspend, network wait, pause): a request
 * made then stays pending. This keeps the "never desc[8] on a timer" rule's
 * reason (desc[8] waits on net_ps locks that a suspended net_ps can
 * hold): the pass happens once per event, never while net_ps may be
 * suspended, never periodically.
 *
 * Self-heal: the liveness tick reads other0's ifnet flags (desc[0xd] is the
 * container, container+0x30 the ifnet, ifnet+0x30 its flags; read only)
 * and, while the interface is UP, re-arms if RUNNING is missing or OACTIVE
 * is set, at most once per liveness period.
 *
 * The structure (the same on 3.60-3.74): desc+0x34 holds the container,
 * container+0x30 the ifnet, and the two point back at each other
 * (container+0 = desc, ifnet+0 = container); ifnet+0x30 holds the flags
 * (0x8802 right after attach) and ifnet+0x14 the name, "other<unit>".
 * Link-up sets RUNNING and clears OACTIVE ((flags & ~0x400) | 0x40); a
 * frame is only sent with UP|RUNNING ((flags & 0x41) == 0x41).
 *
 * The first version read the right offsets but refused every pointer below
 * 0x40000000 (that is user space's floor; net_ps's heap is under it), so it
 * always returned 0. Now the pointers are only checked as mapped
 * (ksceKernelVAtoPA) and aligned, and the chain is accepted only when the
 * structure proves itself (ifprobe_verdict): both back-pointers, the name
 * "other<unit>", and attach's BROADCAST|SIMPLEX|MULTICAST bits (those are
 * IFF_CANTCHANGE: SIOCSIFFLAGS cannot clear them). Otherwise self-heal
 * stays off and the probe line says why.
 */
#ifndef IFACE_REARM_H
#define IFACE_REARM_H

#include <stdint.h>

#define IFF_UP_       0x1u
#define IFF_RUNNING_  0x40u
#define IFF_OACTIVE_  0x400u

enum {
	TXA_NONE   = 0,
	TXA_DRAIN  = 1,     /* a desc[8] dequeue pass */
	TXA_LINKUP = 2,     /* desc[9] soft bit 1 first */
};

struct txgate {
	volatile uint32_t req;     /* bumped by iface_rearm (any thread) */
	uint32_t done;             /* the request last served (TX thread) */
	int kick_lost;             /* a kick arrived while quiet (TX thread) */
};

/* One TX-thread wake: kicked = EV_KICK was seen; ok = may call net_ps
 * (running, attached, not TN_QUIET()). Returns TXA_* to perform. */
static inline unsigned
txgate_step(struct txgate *g, int kicked, int ok)
{
	unsigned act = TXA_NONE;
	uint32_t req = g->req;

	if (!ok) {
		if (kicked)
			g->kick_lost = 1;
		return TXA_NONE;
	}
	if (req != g->done) {
		g->done = req;
		act |= TXA_LINKUP | TXA_DRAIN;
	}
	if (kicked || g->kick_lost) {
		g->kick_lost = 0;
		act |= TXA_DRAIN;
	}
	return act;
}

/* 1 if other0's ifnet flags say output is blocked although it is UP. */
static inline int
ifflags_blocked(uint32_t flags)
{
	if (!(flags & IFF_UP_))
		return 0;               /* down on purpose (the plugin's pause) */
	return !(flags & IFF_RUNNING_) || (flags & IFF_OACTIVE_) != 0;
}

/* Attach's fixed flag bits (BROADCAST|SIMPLEX|MULTICAST, IFF_CANTCHANGE). */
#define IFF_ATTACH_BITS_ 0x8802u

/* What the probe read (iface.c fills it; *_ok = the address was mapped and
 * the word read). */
struct ifprobe {
	uint32_t desc;          /* our descriptor's address */
	uint32_t cont;          /* desc[0xd] */
	int      c0_ok;
	uint32_t c0;            /* container[0]: must be desc */
	int      ifp_ok;
	uint32_t ifp;           /* container+0x30 */
	int      i0_ok;
	uint32_t i0;            /* ifnet[0]: must be cont */
	int      name_ok;
	char     name[16];      /* ifnet+0x14 (not necessarily terminated) */
	int      flags_ok;
	uint32_t flags;         /* ifnet+0x30 */
};

/* NULL if the chain is other0's ifnet; else why not. */
static inline const char *
ifprobe_verdict(const struct ifprobe *p)
{
	if (p->cont == 0)
		return "desc[0xd] is empty (not attached?)";
	if (p->cont & 3)
		return "container pointer misaligned";
	if (!p->c0_ok)
		return "container not mapped";
	if (p->c0 != p->desc)
		return "container[0] is not our descriptor";
	if (!p->ifp_ok)
		return "container+0x30 not readable";
	if (p->ifp == 0 || (p->ifp & 3))
		return "ifnet pointer null or misaligned";
	if (!p->i0_ok)
		return "ifnet not mapped";
	if (p->i0 != p->cont)
		return "ifnet[0] is not the container";
	if (!p->name_ok)
		return "ifnet name not readable";
	if (p->name[0] != 'o' || p->name[1] != 't' || p->name[2] != 'h' ||
	    p->name[3] != 'e' || p->name[4] != 'r' ||
	    p->name[5] < '0' || p->name[5] > '7' || p->name[6] != 0)
		return "ifnet name is not other<0-7>";
	if (!p->flags_ok)
		return "ifnet+0x30 not readable";
	if ((p->flags & IFF_ATTACH_BITS_) != IFF_ATTACH_BITS_)
		return "flags lack attach's BROADCAST|SIMPLEX|MULTICAST";
	return NULL;
}

#endif /* IFACE_REARM_H */
