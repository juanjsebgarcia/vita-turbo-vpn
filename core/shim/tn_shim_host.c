/*
 * tn_shim_host.c - host back end of the shim (macOS and other POSIX
 * systems): recursive pthread mutexes, getentropy, clock_gettime.
 * Used by the desk tests only; nothing here reaches the Vita build.
 */
#include "tn_shim.h"

#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

static pthread_mutex_t g_mtx[TNSHIM_NLOCKS];
static uint64_t g_skew_ns;	/* added to both clocks by the tests */

void
tnshim_host_advance_ns(uint64_t ns)
{
	__atomic_add_fetch(&g_skew_ns, ns, __ATOMIC_SEQ_CST);
}

int
tnshim_be_init(void)
{
	pthread_mutexattr_t attr;
	int i;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	for (i = 0; i < TNSHIM_NLOCKS; i++)
		if (pthread_mutex_init(&g_mtx[i], &attr) != 0)
			return -1;
	pthread_mutexattr_destroy(&attr);
	return 0;
}

void
tnshim_be_fini(void)
{
	int i;

	for (i = 0; i < TNSHIM_NLOCKS; i++)
		pthread_mutex_destroy(&g_mtx[i]);
}

void
tnshim_be_lock(int id)
{
	pthread_mutex_lock(&g_mtx[id]);
}

void
tnshim_be_unlock(int id)
{
	pthread_mutex_unlock(&g_mtx[id]);
}

uintptr_t
tnshim_be_self(void)
{
	return (uintptr_t)pthread_self();
}

int
tnshim_be_random_genuine(void *buf, size_t n)
{
	unsigned char *p = buf;

	/* getentropy takes at most 256 bytes per call. */
	while (n > 0) {
		size_t k = n > 256 ? 256 : n;
		if (getentropy(p, k) != 0)
			return -1;
		p += k;
		n -= k;
	}
	return 0;
}

int
tnshim_be_random_pseudo(void *buf, size_t n)
{
	return tnshim_be_random_genuine(buf, n);
}

void
tnshim_be_uptime_ns(uint64_t *ns)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	*ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec +
	    __atomic_load_n(&g_skew_ns, __ATOMIC_SEQ_CST);
}

void
tnshim_be_walltime_ns(uint64_t *ns)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	*ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec +
	    __atomic_load_n(&g_skew_ns, __ATOMIC_SEQ_CST);
}

void
tnshim_be_sleep_ns(uint64_t ns)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ns / 1000000000ULL);
	ts.tv_nsec = (long)(ns % 1000000000ULL);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

void
tnshim_be_panic(const char *what, const char *file, int line)
{
	fprintf(stderr, "tunnel core: %s (%s:%d)\n", what, file, line);
	abort();
}
