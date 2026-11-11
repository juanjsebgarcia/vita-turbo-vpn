/*
 * test_selftest.c - runs OpenBSD's own WGTEST self-tests (noise_test and
 * cookie_test, as if_wg.c's tnattach does) against the shim.
 *
 * They report through printf, which the shim routes here: every line is
 * echoed, and lines ending in "passed." or containing "failed" are counted.
 */
#include <net/wg_noise.h>
#include <net/wg_cookie.h>

#include <stdarg.h>

/* The five self-tests that print a verdict (the speed test only times). */
#define EXPECT_PASSED	5

static int s_passed, s_failed;

int
tntest_printf(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs("  ", stdout);
	fputs(line, stdout);
	if (strstr(line, "failed") != NULL)
		s_failed++;
	else if (strstr(line, "passed.") != NULL)
		s_passed++;
	return n;
}

int
main(void)
{
	if (tnshim_init() != 0)
		return 1;
	cookie_test();
	noise_test();
	tnshim_fini();
	/* fprintf: printf itself is routed to tntest_printf in this file. */
	fprintf(stdout, "selftest: %d passed, %d failed (expected %d passed)\n",
	    s_passed, s_failed, EXPECT_PASSED);
	return !(s_failed == 0 && s_passed == EXPECT_PASSED);
}
