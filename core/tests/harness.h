/*
 * harness.h - minimal check counting for the host tests.
 */
#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <string.h>

static int h_pass, h_fail;

#define CHECK(cond, name) do {						\
	if (cond) {							\
		h_pass++;						\
	} else {							\
		h_fail++;						\
		fprintf(stderr, "FAIL %s (%s:%d)\n", (name),		\
		    __FILE__, __LINE__);				\
	}								\
} while (0)

#define CHECK_MEM(a, b, n, name)	CHECK(memcmp((a), (b), (n)) == 0, (name))
#define CHECK_EQ(a, b, name)		CHECK((a) == (b), (name))

static inline int
harness_report(const char *suite)
{
	printf("%s: %d passed, %d failed\n", suite, h_pass, h_fail);
	return h_fail != 0;
}

#endif /* HARNESS_H */
