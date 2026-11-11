/*
 * vita_linkcheck.c - not run anywhere: linked against libtncore.a and the
 * kernel stubs by vita.mk to prove every symbol the core needs resolves.
 */
#include <tn_core.h>
#include <tn_crypto.h>

static struct noise_local l;
static struct noise_remote r;
static struct cookie_checker cc;
static struct cookie_maker cm;
static struct pool pl;

static struct noise_remote *
get(void *a, uint8_t k[NOISE_PUBLIC_KEY_LEN])
{
	(void)a; (void)k;
	return &r;
}

static uint32_t
set(void *a, struct noise_remote *x)
{
	(void)a; (void)x;
	return arc4random();
}

static void
drop(void *a, uint32_t i)
{
	(void)a; (void)i;
}

int
module_start(unsigned int args, void *argp)
{
	struct noise_upcall up = { NULL, get, set, drop };
	struct tn_pkt_initiation init;
	uint8_t k[NOISE_PUBLIC_KEY_LEN];

	(void)args; (void)argp;
	if (tnshim_init() != 0)
		return 1;
	noise_local_init(&l, &up);
	arc4random_buf(k, sizeof(k));
	noise_local_lock_identity(&l);
	noise_local_set_private(&l, k);
	noise_local_unlock_identity(&l);
	noise_local_keys(&l, k, NULL);
	noise_remote_init(&r, k, &l);
	cookie_maker_init(&cm, k);
	pool_init(&pl, sizeof(struct ratelimit_entry), 0, IPL_NET, 0, "rl",
	    NULL);
	cookie_checker_init(&cc, &pl);
	noise_create_initiation(&r, &init.s_idx, init.ue, init.es, init.ets);
	cookie_maker_mac(&cm, &init.m, &init, sizeof(init) - sizeof(init.m));
	noise_remote_clear(&r);
	cookie_checker_deinit(&cc);
	pool_destroy(&pl);
	/* the crypto self-test and benchmark hook (tn_crypto.h) */
	{
		static struct tn_crypto_bench_row rows[8];

		if (tn_crypto_selftest() != 0 || tn_crypto_impl() == NULL)
			return 1;
		tn_crypto_bench(rows, 8, 1, 1420);
	}
	tnshim_fini();
	return 0;
}
