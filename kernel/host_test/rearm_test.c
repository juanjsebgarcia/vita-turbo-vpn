/*
 * rearm_test.c - the interface TX thread's re-arm decision (iface_rearm.h)
 * on the host: kicks kept across a quiet spell, re-arm requests served
 * once, and the ifnet-flags check of the self-heal. rearm.sh builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iface_rearm.h"

static int fails;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int
main(void)
{
	struct txgate g = { 0, 0, 0 };

	/* Nothing asked, nothing kicked: nothing to do. */
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);
	/* A plain kick: one pass. */
	CHECK(txgate_step(&g, 1, 1) == TXA_DRAIN);
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);

	/* A kick while quiet (suspend, network wait, pause) is kept and
	 * served by the first wake after the quiet, kick or not (it used to
	 * be dropped with the event bit). */
	CHECK(txgate_step(&g, 1, 0) == TXA_NONE);
	CHECK(txgate_step(&g, 0, 0) == TXA_NONE);
	CHECK(txgate_step(&g, 0, 1) == TXA_DRAIN);
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);

	/* The device sequence: sleep (kicks lost), wake, network wait, then
	 * the network-ready re-arm: link-up and a pass, exactly once. */
	CHECK(txgate_step(&g, 1, 0) == TXA_NONE);       /* kick during suspend */
	g.req++;                                        /* iface_rearm("wake") ... */
	CHECK(txgate_step(&g, 0, 0) == TXA_NONE);       /* ... still net_wait */
	CHECK(txgate_step(&g, 1, 1) == (TXA_LINKUP | TXA_DRAIN));
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);
	CHECK(txgate_step(&g, 1, 1) == TXA_DRAIN);      /* later kicks: passes only */

	/* Several requests before the thread runs collapse into one re-arm. */
	g.req += 3;
	CHECK(txgate_step(&g, 0, 1) == (TXA_LINKUP | TXA_DRAIN));
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);

	/* A request served only once the thread may call net_ps, however
	 * long the quiet lasts. */
	g.req++;
	for (int i = 0; i < 100; i++)
		CHECK(txgate_step(&g, i & 1, 0) == TXA_NONE);
	CHECK(txgate_step(&g, 0, 1) == (TXA_LINKUP | TXA_DRAIN));
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);

	/* Counter wrap. */
	g.req = 0xffffffffu; g.done = 0xfffffffeu;
	CHECK(txgate_step(&g, 0, 1) == (TXA_LINKUP | TXA_DRAIN));
	g.req++;                                        /* wraps to 0 */
	CHECK(txgate_step(&g, 0, 1) == (TXA_LINKUP | TXA_DRAIN));
	CHECK(txgate_step(&g, 0, 1) == TXA_NONE);

	/* ifnet flags: UP|RUNNING (+ BROADCAST, SIMPLEX, MULTICAST) is fine;
	 * UP without RUNNING or with OACTIVE is blocked; down is not ours to
	 * fix (the plugin's pause takes it down on purpose). */
	CHECK(!ifflags_blocked(0x8843));
	CHECK(!ifflags_blocked(0x0041));
	CHECK(ifflags_blocked(0x8803));
	CHECK(ifflags_blocked(0x0001));
	CHECK(ifflags_blocked(0x8C43));
	CHECK(!ifflags_blocked(0x8842));
	CHECK(!ifflags_blocked(0x0000));

	/* The ifnet probe: the attach layout passes; each broken link fails. */
	{
		struct ifprobe good = {
			.desc = 0x00A01000u, .cont = 0x00B02000u, .c0_ok = 1, .c0 = 0x00A01000u,
			.ifp_ok = 1, .ifp = 0x00B03000u, .i0_ok = 1, .i0 = 0x00B02000u,
			.name_ok = 1, .name = "other0", .flags_ok = 1, .flags = 0x8802u,
		}, b;

		CHECK(ifprobe_verdict(&good) == NULL);
		b = good; b.flags = 0x8843u;            CHECK(ifprobe_verdict(&b) == NULL);
		b = good; b.name[5] = '3';              CHECK(ifprobe_verdict(&b) == NULL);
		b = good; b.cont = 0;                   CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.cont |= 2;                  CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.c0_ok = 0;                  CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.c0 = 0x00A01004u;           CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.ifp_ok = 0;                 CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.ifp = 0;                    CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.i0_ok = 0;                  CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.i0 = 0;                     CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.name_ok = 0;                CHECK(ifprobe_verdict(&b) != NULL);
		b = good; memcpy(b.name, "wlan0", 6);   CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.name[6] = '1';              CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.name[5] = '8';              CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.flags_ok = 0;               CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.flags = 0;                  CHECK(ifprobe_verdict(&b) != NULL);
		b = good; b.flags = 0x0043u;            CHECK(ifprobe_verdict(&b) != NULL);
	}

	if (fails) {
		printf("rearm_test: %d failure(s)\n", fails);
		return 1;
	}
	printf("rearm_test: all passed\n");
	return 0;
}
