/*
 * tn_shim.h - the kernel environment the OpenBSD protocol core expects.
 *
 * The vendored files in third_party/openbsd are compiled unmodified, with
 * this header force-included (-include tn_shim.h) and shim/include ahead of
 * the system headers, so that their <sys/systm.h>, <sys/mutex.h>, ... land
 * on one-line stubs that only pull this file in.
 *
 * Two back ends, picked at compile time:
 *   TN_SHIM_HOST  pthreads, getentropy, clock_gettime (tests on the desk)
 *   TN_SHIM_VITA  ksce* kernel calls (the kernel module)
 *
 * Everything here is integer only: no float, no double.
 */
#ifndef TN_SHIM_H
#define TN_SHIM_H

#if !defined(TN_SHIM_HOST) && !defined(TN_SHIM_VITA)
#error "define TN_SHIM_HOST or TN_SHIM_VITA"
#endif

/*
 * System headers first, before any of the renaming macros below, so that
 * their own declarations (index, bzero, arc4random_buf, ...) are already
 * seen and guarded when the macros take effect.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#ifdef TN_SHIM_HOST
#include <stdio.h>
#include <stdlib.h>
#endif
/* OpenBSD's own, vendored: wg_noise.h uses SLIST without including it. */
#include <sys/queue.h>

/* ---- compiler attributes ------------------------------------------------ */

#ifndef __aligned
#define __aligned(x)		__attribute__((__aligned__(x)))
#endif
#ifndef __packed
#define __packed		__attribute__((__packed__))
#endif
#undef __bounded
#define __bounded(x)
/* curve25519.c defines its own; drop the libc one so it does not clash. */
#undef __always_inline

/* ---- types the BSD kernel takes for granted ----------------------------- */

#define in_addr_t		tn_in_addr_t
#define in_port_t		tn_in_port_t
#define sa_family_t		tn_sa_family_t
typedef uint32_t		tn_in_addr_t;
typedef uint16_t		tn_in_port_t;
typedef uint8_t			tn_sa_family_t;

/* BSD 4.4 layout, the same as SceNetSockaddr / SceNetSockaddrIn. */
struct sockaddr {
	uint8_t		sa_len;
	sa_family_t	sa_family;
	char		sa_data[14];
};

struct in_addr {
	in_addr_t	s_addr;
};

struct sockaddr_in {
	uint8_t		sin_len;
	sa_family_t	sin_family;
	in_port_t	sin_port;
	struct in_addr	sin_addr;
	in_port_t	sin_vport;
	char		sin_zero[6];
};

#ifndef AF_INET
#define AF_INET			2
#endif
#ifndef AF_INET6
#define AF_INET6		24
#endif

#define satosin(sa)		((struct sockaddr_in *)(sa))
#define sintosa(sin)		((struct sockaddr *)(sin))
#define satosin6(sa)		((struct sockaddr_in6 *)(sa))
#define sin6tosa(sin6)		((struct sockaddr *)(sin6))

#ifdef INET6
#error "IPv6 is out of scope for v1: build without INET6"
#endif

/* ---- errno values used by the core and not in every libc ---------------- */

#ifndef ESTALE
#define ESTALE			70
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED		61
#endif
#ifndef ECONNRESET
#define ECONNRESET		54
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT		47
#endif
#ifndef ENOBUFS
#define ENOBUFS			55
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT		60
#endif

/* ---- misc kernel macros ------------------------------------------------- */

#ifndef MIN
#define MIN(a, b)		(((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)		(((a) > (b)) ? (a) : (b))
#endif
#ifndef nitems
#define nitems(a)		(sizeof((a)) / sizeof((a)[0]))
#endif

/* Interrupt priority levels and sleep priorities: meaningless here. */
#define IPL_NONE		0
#define IPL_NET			1
#define PWAIT			0

/* malloc(9) flags, only passed through to hashinit(). */
#define M_DEVBUF		0
#define M_NOWAIT		0x0002
#define M_WAITOK		0x0001
#define M_ZERO			0x0008

/* pool(9) flags. */
#define PR_WAITOK		0x0001
#define PR_NOWAIT		0x0002
#define PR_ZERO			0x0008

/* ---- byte order (both targets are little-endian) ------------------------ */

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the shim assumes a little-endian target"
#endif

#undef htole16
#undef htole32
#undef htole64
#undef le16toh
#undef le32toh
#undef le64toh
#undef htobe16
#undef htobe32
#undef htobe64
#undef be16toh
#undef be32toh
#undef be64toh
#undef letoh16		/* musl defines these too */
#undef letoh32
#undef letoh64
#define htole16(x)		((uint16_t)(x))
#define htole32(x)		((uint32_t)(x))
#define htole64(x)		((uint64_t)(x))
#define le16toh(x)		((uint16_t)(x))
#define le32toh(x)		((uint32_t)(x))
#define le64toh(x)		((uint64_t)(x))
#define letoh16(x)		le16toh(x)
#define letoh32(x)		le32toh(x)
#define letoh64(x)		le64toh(x)
#define htobe16(x)		__builtin_bswap16((uint16_t)(x))
#define htobe32(x)		__builtin_bswap32((uint32_t)(x))
#define htobe64(x)		__builtin_bswap64((uint64_t)(x))
#define be16toh(x)		htobe16(x)
#define be32toh(x)		htobe32(x)
#define be64toh(x)		htobe64(x)

/* Load and store through possibly unaligned pointers. */
static inline uint64_t
tnshim_lemtoh64(const void *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return le64toh(v);
}

static inline void
tnshim_htolem64(void *p, uint64_t v)
{
	v = htole64(v);
	memcpy(p, &v, sizeof(v));
}

#define lemtoh64(p)		tnshim_lemtoh64(p)
#define htolem64(p, v)		tnshim_htolem64((p), (v))

/* ---- string helpers ----------------------------------------------------- */

#undef bzero
#undef bcmp
#undef bcopy
#define bzero(p, n)		((void)memset((p), 0, (n)))
#define bcmp(a, b, n)		memcmp((a), (b), (n))
#define bcopy(s, d, n)		((void)memmove((d), (s), (n)))

#define explicit_bzero		tnshim_explicit_bzero
#define timingsafe_bcmp		tnshim_timingsafe_bcmp
void	tnshim_explicit_bzero(void *, size_t);
int	tnshim_timingsafe_bcmp(const void *, const void *, size_t);

/* ---- random numbers ----------------------------------------------------- */

/*
 * arc4random_buf: every caller in the core fills a secret (ephemeral private
 * keys, the cookie secret, the SipHash key, XChaCha20 nonces), so it takes
 * the genuine hardware source. arc4random and arc4random_uniform (indices,
 * timer jitter) take the pseudo-random one.
 */
#define arc4random		tnshim_random32
#define arc4random_buf		tnshim_random_buf
#define arc4random_uniform	tnshim_random_uniform
uint32_t tnshim_random32(void);
void	tnshim_random_buf(void *, size_t);
uint32_t tnshim_random_uniform(uint32_t);

/* ---- time --------------------------------------------------------------- */

#define getnanouptime		tnshim_nanouptime
#define nanouptime		tnshim_nanouptime
#define getnanotime		tnshim_nanotime
#define nanotime		tnshim_nanotime
struct timeval;
void	tnshim_nanouptime(struct timespec *);	/* monotonic, since boot */
void	tnshim_nanotime(struct timespec *);	/* wall clock, Unix epoch */

#undef timespecclear
#undef timespecisset
#undef timespeccmp
#undef timespecadd
#undef timespecsub
#define timespecclear(tsp)	(tsp)->tv_sec = (tsp)->tv_nsec = 0
#define timespecisset(tsp)	((tsp)->tv_sec || (tsp)->tv_nsec)
#define timespeccmp(tsp, usp, cmp)					\
	(((tsp)->tv_sec == (usp)->tv_sec) ?				\
	    ((tsp)->tv_nsec cmp (usp)->tv_nsec) :			\
	    ((tsp)->tv_sec cmp (usp)->tv_sec))
#define timespecadd(tsp, usp, vsp)					\
	do {								\
		(vsp)->tv_sec = (tsp)->tv_sec + (usp)->tv_sec;		\
		(vsp)->tv_nsec = (tsp)->tv_nsec + (usp)->tv_nsec;	\
		if ((vsp)->tv_nsec >= 1000000000L) {			\
			(vsp)->tv_sec++;				\
			(vsp)->tv_nsec -= 1000000000L;			\
		}							\
	} while (0)
#define timespecsub(tsp, usp, vsp)					\
	do {								\
		(vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;		\
		(vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;	\
		if ((vsp)->tv_nsec < 0) {				\
			(vsp)->tv_sec--;				\
			(vsp)->tv_nsec += 1000000000L;			\
		}							\
	} while (0)

/* Only the self-tests sleep. */
#define tsleep_nsec(ident, prio, wmesg, nsecs)				\
	tnshim_sleep_nsec((uint64_t)(nsecs))
void	tnshim_sleep_nsec(uint64_t);

/* ---- locks -------------------------------------------------------------- */

/*
 * Every struct mutex maps to one module-wide recursive lock, and every
 * struct rwlock to a second one. The structs carry no kernel object, so the
 * core may copy them (noise_remote_begin_session copies an initialised
 * keypair, counter mutex included) and re-initialise them over bzero'd
 * memory without leaking. Lock order is always rwlock before mutex.
 * Thread context only: never call into the core from an interrupt handler.
 */
struct mutex {
	const char	*mtx_name;
};

struct rwlock {
	const char	*rwl_name;
};

enum { TNSHIM_LOCK_MTX = 0, TNSHIM_LOCK_RW = 1, TNSHIM_NLOCKS = 2 };

void	tnshim_lock_enter(int);
void	tnshim_lock_exit(int);
int	tnshim_lock_held(int);

#define mtx_init(m, ipl)		((m)->mtx_name = "")
#define mtx_init_flags(m, ipl, n, f)	((m)->mtx_name = (n))
#define mtx_enter(m)		((void)(m), tnshim_lock_enter(TNSHIM_LOCK_MTX))
#define mtx_leave(m)		((void)(m), tnshim_lock_exit(TNSHIM_LOCK_MTX))
#define MUTEX_ASSERT_LOCKED(m)	KASSERT(tnshim_lock_held(TNSHIM_LOCK_MTX))

#define rw_init(rw, n)		((rw)->rwl_name = (n))
#define rw_enter_read(rw)	((void)(rw), tnshim_lock_enter(TNSHIM_LOCK_RW))
#define rw_exit_read(rw)	((void)(rw), tnshim_lock_exit(TNSHIM_LOCK_RW))
#define rw_enter_write(rw)	((void)(rw), tnshim_lock_enter(TNSHIM_LOCK_RW))
#define rw_exit_write(rw)	((void)(rw), tnshim_lock_exit(TNSHIM_LOCK_RW))
#define rw_assert_wrlock(rw)	KASSERT(tnshim_lock_held(TNSHIM_LOCK_RW))
#define rw_assert_rdlock(rw)	KASSERT(tnshim_lock_held(TNSHIM_LOCK_RW))

/* ---- atomics ------------------------------------------------------------ */

#define atomic_inc_long_nv(p)	__atomic_add_fetch((p), 1UL, __ATOMIC_SEQ_CST)
#define atomic_inc_int_nv(p)	__atomic_add_fetch((p), 1U, __ATOMIC_SEQ_CST)
#define atomic_dec_long_nv(p)	__atomic_sub_fetch((p), 1UL, __ATOMIC_SEQ_CST)
#define atomic_dec_int_nv(p)	__atomic_sub_fetch((p), 1U, __ATOMIC_SEQ_CST)
#define atomic_load_int(p)	__atomic_load_n((p), __ATOMIC_RELAXED)
#define atomic_store_int(p, v)	__atomic_store_n((p), (v), __ATOMIC_RELAXED)

/* ---- pools and hash tables: fixed static storage ------------------------ */

/*
 * pool(9) over a static arena: TN_POOL_SLOTS pools may exist at once, each
 * with room for TN_POOL_ITEMS items of at most TN_POOL_ITEM_MAX bytes. An
 * empty pool returns NULL, which the rate limiter treats as "refuse".
 */
#ifndef TN_POOL_SLOTS
#define TN_POOL_SLOTS		1
#endif
#ifndef TN_POOL_ITEMS
#define TN_POOL_ITEMS		256
#endif
#ifndef TN_POOL_ITEM_MAX
#define TN_POOL_ITEM_MAX	64
#endif

struct pool_allocator;
struct pool {
	unsigned char	*pr_base;
	void		*pr_free;
	size_t		 pr_size;
	unsigned int	 pr_nitems;
	unsigned int	 pr_nout;
	int		 pr_slot;
	const char	*pr_wchan;
};

void	pool_init(struct pool *, size_t, u_int, int, int, const char *,
	    struct pool_allocator *);
void	pool_destroy(struct pool *);
void	*pool_get(struct pool *, int);
void	pool_put(struct pool *, void *);

/*
 * hashinit(9) from TN_HASH_SLOTS static tables of TN_HASH_SIZE buckets
 * (wg_cookie.c asks for RATELIMIT_SIZE, 8192, per rate limiter).
 */
#ifndef TN_HASH_SLOTS
#define TN_HASH_SLOTS		1
#endif
#ifndef TN_HASH_SIZE
#define TN_HASH_SIZE		8192
#endif

void	*hashinit(int, int, int, u_long *);
void	hashfree(void *, int, int);

/* ---- assertions and diagnostics ----------------------------------------- */

void	tnshim_panic(const char *, const char *, int);

#ifdef TN_DIAGNOSTIC
#define KASSERT(e)	((e) ? (void)0 : tnshim_panic(#e, __FILE__, __LINE__))
#else
#define KASSERT(e)	((void)0)
#endif
#define KASSERTMSG(e, ...)	KASSERT(e)
#define panic(...)	tnshim_panic("panic", __FILE__, __LINE__)

#ifdef WGTEST
/* The OpenBSD self-tests report through printf; the harness counts them. */
#undef printf
#define printf		tntest_printf
int	tntest_printf(const char *, ...) __attribute__((format(__printf__, 1, 2)));
/* wg_noise.c's self-test has a static named index, as does strings.h. */
#define index		tntest_index
#endif

/* ---- set-up ------------------------------------------------------------- */

/*
 * tnshim_init creates the two locks; call it once before any core function
 * (module_start on the Vita). tnshim_fini undoes it. Both return 0 or a
 * negative error from the back end.
 */
int	tnshim_init(void);
void	tnshim_fini(void);

/* Times the genuine random source failed and the pseudo one stood in. */
unsigned int tnshim_rng_fallbacks(void);

#ifdef TN_SHIM_HOST
/* Tests only: move both clocks forward without sleeping. */
void	tnshim_host_advance_ns(uint64_t);
#endif

#ifdef TN_SHIM_VITA
/*
 * Monotonic wall-clock floor for TAI64N handshake timestamps. The module
 * seeds the floor from its persisted state file at start and reads it back on
 * a timer to persist it; the back end never does file I/O itself.
 */
void		tnshim_vita_wall_floor_set(uint64_t ns);
uint64_t	tnshim_vita_wall_floor_get(void);
#endif

/* Back-end entry points (tn_shim_host.c or tn_shim_vita.c). */
int	tnshim_be_init(void);
void	tnshim_be_fini(void);
void	tnshim_be_lock(int);
void	tnshim_be_unlock(int);
uintptr_t tnshim_be_self(void);
int	tnshim_be_random_genuine(void *, size_t);
int	tnshim_be_random_pseudo(void *, size_t);
void	tnshim_be_uptime_ns(uint64_t *);
void	tnshim_be_walltime_ns(uint64_t *);
void	tnshim_be_sleep_ns(uint64_t);
void	tnshim_be_panic(const char *, const char *, int) __attribute__((noreturn));

#endif /* TN_SHIM_H */
