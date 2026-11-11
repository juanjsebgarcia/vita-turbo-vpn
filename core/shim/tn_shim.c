/*
 * tn_shim.c - back-end independent half of the OpenBSD kernel shim:
 * memory helpers, the two module locks, random numbers, time, pools and
 * hash tables. The back end (tn_shim_host.c or tn_shim_vita.c) supplies
 * the tnshim_be_* primitives.
 */
#include "tn_shim.h"

/* ---- memory ------------------------------------------------------------- */

void
tnshim_explicit_bzero(void *p, size_t n)
{
	memset(p, 0, n);
	/* Keep the compiler from dropping the memset as a dead store. */
	__asm__ __volatile__("" : : "r"(p) : "memory");
}

int
tnshim_timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const volatile unsigned char *x = a, *y = b;
	unsigned char d = 0;
	size_t i;

	for (i = 0; i < n; i++)
		d |= x[i] ^ y[i];
	return d != 0;
}

/* ---- locks -------------------------------------------------------------- */

/*
 * Holder and depth are only written by the thread holding the lock; other
 * threads read the holder just to see that it is not themselves.
 */
static struct {
	uintptr_t	holder;
	unsigned int	depth;
} g_lk[TNSHIM_NLOCKS];

void
tnshim_lock_enter(int id)
{
	tnshim_be_lock(id);
	if (g_lk[id].depth++ == 0)
		__atomic_store_n(&g_lk[id].holder, tnshim_be_self(),
		    __ATOMIC_RELAXED);
}

void
tnshim_lock_exit(int id)
{
	KASSERT(tnshim_lock_held(id));
	if (--g_lk[id].depth == 0)
		__atomic_store_n(&g_lk[id].holder, 0, __ATOMIC_RELAXED);
	tnshim_be_unlock(id);
}

int
tnshim_lock_held(int id)
{
	return __atomic_load_n(&g_lk[id].holder, __ATOMIC_RELAXED) ==
	    tnshim_be_self();
}

int
tnshim_init(void)
{
	memset(g_lk, 0, sizeof(g_lk));
	return tnshim_be_init();
}

void
tnshim_fini(void)
{
	tnshim_be_fini();
}

void
tnshim_panic(const char *what, const char *file, int line)
{
	tnshim_be_panic(what, file, line);
}

/* ---- random numbers ----------------------------------------------------- */

static unsigned int g_rng_fallbacks;

unsigned int
tnshim_rng_fallbacks(void)
{
	return __atomic_load_n(&g_rng_fallbacks, __ATOMIC_RELAXED);
}

void
tnshim_random_buf(void *buf, size_t n)
{
	int tries;

	for (tries = 0; tries < 3; tries++)
		if (tnshim_be_random_genuine(buf, n) == 0)
			return;
	/* The pseudo source is itself seeded from the hardware at boot. */
	__atomic_add_fetch(&g_rng_fallbacks, 1, __ATOMIC_RELAXED);
	for (tries = 0; tries < 3; tries++)
		if (tnshim_be_random_pseudo(buf, n) == 0)
			return;
	/* Never hand back a key buffer that was not filled. */
	tnshim_panic("no random source", __FILE__, __LINE__);
}

uint32_t
tnshim_random32(void)
{
	uint32_t v;

	if (tnshim_be_random_pseudo(&v, sizeof(v)) != 0)
		tnshim_random_buf(&v, sizeof(v));
	return v;
}

/* Uniform in [0, bound) without modulo bias (OpenBSD's method). */
uint32_t
tnshim_random_uniform(uint32_t bound)
{
	uint32_t r, min;

	if (bound < 2)
		return 0;
	min = -bound % bound;
	for (;;) {
		r = tnshim_random32();
		if (r >= min)
			break;
	}
	return r % bound;
}

/* ---- time --------------------------------------------------------------- */

static void
ns_to_timespec(uint64_t ns, struct timespec *ts)
{
	ts->tv_sec = (time_t)(ns / 1000000000ULL);
	ts->tv_nsec = (long)(ns % 1000000000ULL);
}

void
tnshim_nanouptime(struct timespec *ts)
{
	uint64_t ns;

	tnshim_be_uptime_ns(&ns);
	ns_to_timespec(ns, ts);
}

void
tnshim_nanotime(struct timespec *ts)
{
	uint64_t ns;

	tnshim_be_walltime_ns(&ns);
	ns_to_timespec(ns, ts);
}

void
tnshim_sleep_nsec(uint64_t ns)
{
	tnshim_be_sleep_ns(ns);
}

/* ---- pools -------------------------------------------------------------- */

#define POOL_ITEM_ROUND(n)	(((n) + 7) & ~(size_t)7)

static unsigned char g_pool_arena[TN_POOL_SLOTS][TN_POOL_ITEMS *
    POOL_ITEM_ROUND(TN_POOL_ITEM_MAX)] __attribute__((aligned(8)));
static unsigned char g_pool_used[TN_POOL_SLOTS];

void
pool_init(struct pool *pp, size_t size, u_int align, int ipl, int flags,
    const char *wchan, struct pool_allocator *palloc)
{
	unsigned int i;
	int slot = -1;

	(void)align; (void)ipl; (void)flags; (void)palloc;
	memset(pp, 0, sizeof(*pp));
	pp->pr_slot = -1;
	pp->pr_wchan = wchan;
	size = POOL_ITEM_ROUND(size);
	KASSERT(size <= POOL_ITEM_ROUND(TN_POOL_ITEM_MAX));
	if (size > POOL_ITEM_ROUND(TN_POOL_ITEM_MAX))
		return;

	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	for (i = 0; i < TN_POOL_SLOTS; i++) {
		if (!g_pool_used[i]) {
			g_pool_used[i] = 1;
			slot = (int)i;
			break;
		}
	}
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
	KASSERT(slot >= 0);
	if (slot < 0)
		return;		/* an empty pool: every pool_get fails */

	pp->pr_slot = slot;
	pp->pr_base = g_pool_arena[slot];
	pp->pr_size = size;
	pp->pr_nitems = TN_POOL_ITEMS;
	/* Thread every item onto the free list. */
	for (i = TN_POOL_ITEMS; i-- > 0;) {
		void **item = (void **)(pp->pr_base + i * size);
		*item = pp->pr_free;
		pp->pr_free = item;
	}
}

void
pool_destroy(struct pool *pp)
{
	if (pp->pr_slot < 0)
		return;
	KASSERT(pp->pr_nout == 0);
	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	g_pool_used[pp->pr_slot] = 0;
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
	memset(pp, 0, sizeof(*pp));
	pp->pr_slot = -1;
}

void *
pool_get(struct pool *pp, int flags)
{
	void **item;

	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	item = pp->pr_free;
	if (item != NULL) {
		pp->pr_free = *item;
		pp->pr_nout++;
	}
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
	if (item != NULL && (flags & PR_ZERO))
		memset(item, 0, pp->pr_size);
	return item;
}

void
pool_put(struct pool *pp, void *v)
{
	void **item = v;

	KASSERT((unsigned char *)v >= pp->pr_base &&
	    (unsigned char *)v < pp->pr_base + pp->pr_nitems * pp->pr_size);
	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	*item = pp->pr_free;
	pp->pr_free = item;
	pp->pr_nout--;
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
}

/* ---- hash tables -------------------------------------------------------- */

/* A LIST_HEAD is one pointer, so a table is an array of pointers. */
static void *g_hash[TN_HASH_SLOTS][TN_HASH_SIZE];
static unsigned char g_hash_used[TN_HASH_SLOTS];

void *
hashinit(int elements, int type, int flags, u_long *hashmask)
{
	u_long size;
	unsigned int i;
	void *table = NULL;

	(void)type; (void)flags;
	for (size = 1; size < (u_long)elements; size <<= 1)
		;
	if (elements <= 0 || size > TN_HASH_SIZE)
		return NULL;

	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	for (i = 0; i < TN_HASH_SLOTS; i++) {
		if (!g_hash_used[i]) {
			g_hash_used[i] = 1;
			table = g_hash[i];
			break;
		}
	}
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
	if (table == NULL)
		return NULL;

	memset(table, 0, size * sizeof(void *));	/* LIST_INIT */
	*hashmask = size - 1;
	return table;
}

void
hashfree(void *table, int elements, int type)
{
	unsigned int i;

	(void)elements; (void)type;
	tnshim_lock_enter(TNSHIM_LOCK_MTX);
	for (i = 0; i < TN_HASH_SLOTS; i++)
		if (table == (void *)g_hash[i])
			g_hash_used[i] = 0;
	tnshim_lock_exit(TNSHIM_LOCK_MTX);
}
