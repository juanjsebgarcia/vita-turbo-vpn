/*
 * strand_hook.c - the hook body, and nothing else: this object file is checked
 * whole by tests/check_strand.sh. See strand_hook.h. Installing is strand.c.
 */
#include <taihen.h>

#include "strand_core.h"
#include "strand_hook.h"

#define REF_SPINS 200000u        /* loads, no system call: about a millisecond at most */

volatile tai_hook_ref_t strand_ref;

uint32_t strand_hook(uint32_t desc)
{
	tai_hook_ref_t ref;
	uint32_t spins = REF_SPINS, pkt;

	while ((ref = strand_ref) == 0 && --spins)
		;
	if (!ref)
		return 0;
	pkt = TAI_CONTINUE(uint32_t, ref, desc);
	if (pkt && __atomic_load_n(&strand_st.addr, __ATOMIC_RELAXED))   /* strand_capture looks again, ordered */
		strand_capture(desc, pkt);
	return pkt;
}
