# Protocol core

The tunnel protocol core of vita-turbo-vpn: OpenBSD's
`wg_noise`, `wg_cookie` and `sys/crypto` primitives, compiled **unmodified**
behind a small shim, with a host back end for desk tests and a Vita kernel
back end for the module.

The vendored sources and their licence review are in
[`../third_party/openbsd`](../third_party/openbsd/LICENSES.md) (OpenBSD commit
`eeba8a4a486f147098b87d8bc0796a524d7b41f2`, all ISC, BSD-3 or public domain).

## Layout

```
core/
  include/tn_shim.h      the kernel environment the OpenBSD files expect
  include/tn_core.h      what the interface layer includes: wg_noise.h,
                         wg_cookie.h, if_wg.c's timer constants, wire formats
  shim/include/...       one-line <sys/systm.h>, <sys/mutex.h>, ... that
                         only pull in tn_shim.h
  shim/tn_shim.c         memory helpers, locks, RNG, time, pools, hash tables
  shim/tn_shim_host.c    host back end: pthreads, getentropy, clock_gettime
  shim/tn_shim_vita.c    Vita back end: ksce* kernel calls
  include/tn_crypto.h    the ChaCha20-Poly1305 build switch, self-test, bench
  crypto/chachapoly_cg.c OpenBSD's AEAD entry points on CRYPTOGAMS assembly
  crypto/cg_*.S          the CRYPTOGAMS ChaCha20 / Poly1305, renamed
  crypto/tn_crypto_bench.c  self-test and benchmark (host and kernel)
  core.mk                sources and flags, shared by both builds
  Makefile               host library and tests (make test)
  vita.mk, vita.sh       Vita static library in the pinned VitaSDK image
  arm32.sh               the host tests as 32-bit ARMv7 Linux in Docker
  tests/                 vectors, OpenBSD self-tests, end-to-end, link check
```

## Building and testing

On the host (macOS clang or any POSIX cc):

```
make test          # builds build/host/libtncore.a and runs the four tests
make WARN_ALL=1    # also show the upstream warnings core.mk silences
```

For the Vita (needs Docker; the same pinned image as the other builds):

```
./vita.sh          # or: make vita
```

This builds `build/vita/libtncore.a`, writes an objdump listing of it to
`build/vita/libtncore.dis` and fails if any floating-point instruction
appears, then links `tests/vita_linkcheck.c` against the library and the
kernel stubs to show that every symbol resolves. A module using the core
links with `SceThreadmgrForDriver_stub`, `SceSblSsMgrForDriver_stub`,
`SceRtcForDriver_stub`, `SceSysclibForDriver_stub` (memcpy, memset, memcmp
and the 64-bit division helpers) and `SceDebugForDriver_stub`.

To run everything as 32-bit ARMv7 (Alpine armhf in Docker, under QEMU on a
non-ARM desk), which is how the CRYPTOGAMS assembly gets tested off the Vita
and how the 32-bit configuration (4-byte `unsigned long`) gets run:

```
./arm32.sh                               # make test bench, TN_CRYPTO=cryptogams
./arm32.sh test ASFLAGS_EXTRA=-marm      # the assembly in ARM mode
make bench                               # host: build/host/bench_crypto
```

Any file that includes `tn_core.h` must be compiled with the core's flags:
`-include tn_shim.h`, `-Iinclude -Ishim/include -I../third_party/openbsd/sys`,
and `-DTN_SHIM_VITA` (or `-DTN_SHIM_HOST`). `core.mk` has them as
`CORE_CPPFLAGS`.

## ChaCha20-Poly1305: CRYPTOGAMS assembly

`TN_CRYPTO` in `core.mk` selects the data-path AEAD:

| `TN_CRYPTO` | Default for | `chacha20poly1305_*`, `xchacha20poly1305_*` |
|---|---|---|
| `cryptogams` | `vita.mk`; an ARMv7 host | `crypto/chachapoly_cg.c` on the CRYPTOGAMS ARMv4/NEON assembly ([`../third_party/cryptogams`](../third_party/cryptogams/README.md), BSD 3-clause option). OpenBSD's `chachapoly.c` is still built with its four AEAD functions renamed `*_ref` (`-D` on that one file), as reference and for the self-test |
| `portable` | other hosts (the assembly is ARMv7 only) | OpenBSD's `chachapoly.c`, unchanged |

Nothing else changes: `wg_noise.c` and `wg_cookie.c` call the same four
functions, the vendored files are still unmodified, and the kernel links the
same `libtncore.a`. BLAKE2s and Curve25519 stay in OpenBSD C (the
benchmark times them so the handshake cost is known).

- **NEON and the integer paths** are both linked. One word picks between
  them per message (`tn_crypto_set_neon`, default on; `TN_CRYPTO_NEON=0`
  starts off). ChaCha20 under 193 bytes takes the integer path even with
  NEON on (the author's advice), so the one-time Poly1305 key and short
  packets never pay the NEON set-up.
- **Kernel safety.** The assembly is integer-only (fpcheck: 0 floating-point
  instructions), saves `d8`-`d15` as AAPCS requires, calls nothing outside
  itself, and is built in the generator's `__KERNEL__` configuration: no CPU
  probe word read PC-relative across segments, only ordinary relocations.
  It must run in thread context, which is where the core always runs.
- **Alignment.** Key and counter are copied into word-aligned locals (the
  assembly reads them with `ldm`). Data may sit at any address; the ARMv7
  integer paths use unaligned `ldr`/`str`, which the Vita's compiled code
  already relies on (GCC's default `-munaligned-access` for ARMv7). The
  self-test runs every path at offsets 0 to 3.
- **Self-test and benchmark** (`tn_crypto.h`): `tn_crypto_selftest()` runs
  RFC 8439 A.5 (the protocol's nonce layout) through every path at four
  offsets, in place and not, with a tag flip, then cross-checks every path
  against `*_ref` over lengths 0-1440. `tn_crypto_bench()` seals and opens
  1420-byte packets in place for each path, then times BLAKE2s and X25519.
  Both are integer-only and kernel-safe, use static buffers (not
  reentrant), and are pulled into a module only if called.

## How the vendored files compile unmodified

- `tn_shim.h` is forced in first (`-include`). It includes the system
  headers it needs, then defines everything else.
- `shim/include` comes first on the include path, so the vendored files'
  `<sys/systm.h>`, `<sys/param.h>`, `<sys/mutex.h>`, `<sys/rwlock.h>`,
  `<sys/atomic.h>`, `<sys/malloc.h>`, `<sys/pool.h>`, `<sys/socket.h>`,
  `<sys/time.h>`, `<netinet/in.h>` and `<lib/libkern/libkern.h>` land on
  stubs that do nothing but include `tn_shim.h`.
- `third_party/openbsd/sys` resolves `<crypto/...>`, `<net/...>` and OpenBSD's
  own `<sys/queue.h>` (vendored with `<sys/_null.h>`; `wg_noise.h` uses
  `SLIST` without including it, so `tn_shim.h` does).
- Name clashes with the C library are avoided by renaming macros
  (`arc4random_buf` to `tnshim_random_buf`, `timingsafe_bcmp` to
  `tnshim_timingsafe_bcmp`, `in_port_t` to `tn_in_port_t`, ...), defined
  after the system headers are in.
- The OpenBSD self-tests build from the same files with `-DWGTEST`; the shim
  routes their `printf` to the test harness and renames their static `index`
  away from `strings.h`'s `index()`.

**No source edits and no patches were needed**, so there is no `patches/`
directory. The only compiler concessions are four warning switches for the
vendored files (see below).

## The shim as implemented

| OpenBSD | Host back end | Vita kernel back end |
|---|---|---|
| `mtx_enter` / `mtx_leave` (every `struct mutex`) | one recursive pthread mutex | one recursive kernel mutex, `ksceKernelCreateMutex(..., SCE_KERNEL_MUTEX_ATTR_RECURSIVE)` |
| `rw_enter_*` / `rw_exit_*` (every `struct rwlock`) | a second recursive pthread mutex | a second recursive kernel mutex |
| `rw_assert_wrlock`, `MUTEX_ASSERT_LOCKED` | checks the calling thread holds that lock (`TN_DIAGNOSTIC`) | same check when built with `TN_DIAGNOSTIC`, otherwise nothing |
| `arc4random_buf` | `getentropy` | `ksceSblRngGenuineRandomNumber` (`0xAC57F4F0`), 64 bytes a call |
| `arc4random`, `arc4random_uniform` | `getentropy` | `ksceSblRngPseudoRandomNumber` (`0x4F9BFBE5`) |
| `getnanouptime`, `nanouptime` | `clock_gettime(CLOCK_MONOTONIC)` | `ksceKernelGetSystemTimeWide` (microseconds since boot) |
| `getnanotime` (TAI64N timestamps) | `clock_gettime(CLOCK_REALTIME)` | `ksceRtcGetCurrentTick` minus the tick of 1970-01-01 (falls back to uptime if the RTC cannot be read) |
| `explicit_bzero` | local: `memset` plus a compiler barrier | same |
| `timingsafe_bcmp` | local constant-time loop | same |
| `bzero`, `bcmp`, `bcopy` | `memset`, `memcmp`, `memmove` | same (SceSysclibForDriver) |
| `pool_init` / `pool_get` / `pool_put` / `pool_destroy` | static arena, `TN_POOL_SLOTS` pools of `TN_POOL_ITEMS` items | same; defaults 1 pool of 256 items of up to 64 bytes |
| `hashinit` / `hashfree` | static tables, `TN_HASH_SLOTS` of `TN_HASH_SIZE` (8192) buckets | same; default 1 table |
| `atomic_inc_long_nv` | `__atomic_add_fetch` | same (the 32-bit Vita takes OpenBSD's mutex path instead) |
| `htole32`, `le64toh`, `htobe64`, `lemtoh64`, `htolem64`, ... | builtins (both targets are little-endian) | same |
| `KASSERT` | aborts with file and line (`TN_DIAGNOSTIC`, on in the host build) | compiled out unless `TN_DIAGNOSTIC` |
| `panic` | prints and aborts | prints with `ksceKernelPrintf` and parks the calling thread |
| `tsleep_nsec` (self-tests only) | `nanosleep` | `ksceKernelDelayThread` |
| `struct sockaddr`, `sockaddr_in`, `in_addr` | own definitions, BSD 4.4 layout | same layout as `SceNetSockaddrIn` |

### Locks: why two module-wide locks

Kernel mutexes per object, with read-write locks as plain
mutexes, would be the obvious design. Done literally, that breaks against unmodified OpenBSD code:

- `noise_remote_begin_session` builds a keypair on the stack, initialises its
  counter mutex, and then copies the whole struct into place
  (`*r->r_current = kp`). A kernel object or an in-place fast mutex cannot be
  copied like that.
- `noise_local_init`, `noise_remote_init`, `cookie_maker_init` and
  `cookie_checker_init` `bzero` their struct and `rw_init` again, with no
  destroy call anywhere. Per-object kernel objects would leak on every
  reconfiguration.

So `struct mutex` and `struct rwlock` carry only a name, and all of them map
to two recursive locks created once by `tnshim_init`. With one peer this
loses no parallelism: OpenBSD already serialises a peer's encrypt and
decrypt on its single `r_keypair_mtx`, and handshakes on
`r_handshake_lock`. Keeping the two classes apart means a handshake's
Curve25519 work (under the rwlocks) never blocks the data path (under the
mutexes). The order is always rwlock before mutex.

### Random numbers

Every `arc4random_buf` caller in the core fills a secret: ephemeral private
keys (`curve25519_generate_secret`), the cookie secret, the rate limiter's
SipHash key, and XChaCha20 nonces. So `arc4random_buf` always takes the
genuine source, and only the 32-bit `arc4random` (indices, timer jitter)
takes the pseudo-random one. If the genuine source fails three times the
pseudo one stands in and `tnshim_rng_fallbacks()` counts it; if both fail,
the shim panics rather than return an unfilled key buffer.

## Contract for the interface layer

- Call `tnshim_init()` once before anything else (module start) and
  `tnshim_fini()` at the end.
- Thread context only: kernel mutexes and the RNG cannot be used from an
  interrupt handler, so neither can the core.
- The noise upcalls `u_index_drop` (and `u_index_set`) run with the core's
  locks held. They must not call back into noise or cookie functions that
  take a rwlock.
- Stack: the deepest paths need about 2 KiB (`noise_remote_begin_session`
  1184 bytes, `curve25519` 744, per GCC's `-fstack-usage` on the Vita
  build). Give any thread that calls the core at least 8 KiB.
- One `cookie_checker` per `TN_HASH_SLOTS` and `TN_POOL_SLOTS`. When the pool
  is full a new source address under load is refused (`ECONNREFUSED`),
  which is OpenBSD's own out-of-memory behaviour. A client only consults the
  rate limiter when it declares itself busy.
- IPv4 only: building with `INET6` is an error.

## Tests

| Program | What | Checks |
|---|---|---|
| `test_vectors` | RFC 8439 ChaCha20 (2.4.2), Poly1305 (2.5.2 and A.3 #1-#11, whole and byte at a time), one-time key (2.6.2), AEAD (2.8.2 from the primitives, A.5 through `chacha20poly1305_*`, every bit flip of ciphertext, tag and AAD rejected); XChaCha20 draft 03 HChaCha20 (2.2.1) and AEAD (A.3.1); RFC 7748 X25519 (5.2, 1 and 1,000 iterations, 6.1, small-order points refused); RFC 7693 BLAKE2s (`abc`, appendix E grand hash), the reference keyed KAT (all 256 lengths, whole and incremental), keyed BLAKE2s with 16-byte output, HMAC-BLAKE2s; SipHash-2-4 reference vectors 0-63 | 77 |
| `test_selftest` | OpenBSD's own `noise_test()` and `cookie_test()` (`WGTEST`), as `tnattach` runs them: counter window, handshake, rate-limit timings and capacity (65536 entries), cookie MACs, plus the speed test | 5 verdicts |
| `test_crypto` | `tn_crypto_selftest()`, then 20,000 random cases per NEON setting (lengths 0-2048, AD 0-64, buffer offsets 0-7, in place and not) and XChaCha20-Poly1305, each compared with `*_ref`. In the portable build it compares the reference with itself | 3 |
| `test_e2e` | two peers through the wire formats: handshake with and without PSK, PSK mismatch, unknown static key, bad mac1, data both ways, keepalive, tampering, initiation replay and flood guard, the replay window (out of order, duplicates, edge of the window), sequential nonces, `REKEY_AFTER_TIME` / `REKEY_AFTER_TIME_RECV` / `REJECT_AFTER_TIME`, key rotation with in-flight packets, cookie reply under load (tamper, replay, address and port binding, expiry, secret rotation), rate limiting, two threads sealing and opening both ways at once, the protocol constants | 141 |

The clock is moved with `tnshim_host_advance_ns` (host back end only)
rather than slept. The tests also pass under AddressSanitizer and
UndefinedBehaviorSanitizer
(`make test CC="cc -fsanitize=address,undefined"`).

`tests/vectors.h` is generated by `tests/gen_vectors.py` from the RFC and
draft texts, the BLAKE2 reference KAT (CC0) and the SipHash reference
vectors (CC0); the script's docstring lists the URLs. The HMAC-BLAKE2s set
has no published source and comes from Python's `hashlib`/`hmac`.

The host is 64-bit, so `unsigned long` is 8 bytes there and 4 on the Vita:
the replay window (`COUNTER_BITS`) and Poly1305's limb type differ, and the
host takes the `__LP64__` atomic path in `noise_counter_send` where the Vita
takes the mutex path. Both are exercised by the OpenBSD code generically,
and `./arm32.sh` runs the whole suite in the 32-bit configuration too.

## Vita build results

- `-O2 -Wall -Wextra`, GCC 15.2.0 (VitaSDK image pinned above): no warnings
  from the shim. The vendored files give nine, all benign, silenced by
  `core.mk` (`./vita.sh WARN_ALL=1` after a clean, to see them): `chacha_private.h`
  lines 52-53 (16-byte `sigma`/`tau` arrays without the NUL), `chachapoly.c`
  two unused parameters, two `int` against `sizeof` comparisons, one
  `char *` to `uint8_t *`; `siphash.c` one unused parameter; `wg_cookie.c`
  one `char *` to `uint8_t *`.
- `libtncore.a`: 46,412 bytes; text 24,664, data 8, bss 49,248 (the 32 KiB
  hash table and the 16 KiB pool).
- Floating point: none. The objdump listing has 298 VFP/NEON instructions, all
  integer or data movement: `vldr`/`vstr` (spills and 8-byte copies),
  `vld1`/`vst1`, `vmov`, and integer NEON (`vadd.i32`, `vsub.i32`, `veor`,
  `vorr`, `vand`, `vbsl`, `vbif`, `vceq.i32`, `vdup.32`, `vmvn`). No `.f32`
  or `.f64` operation, conversion, divide, square root or FPSCR access.
  Compiler-generated NEON uses `d` registers, so the core relies on kernel
  threads having VFP context.
- The link check resolves every symbol against the kernel stubs. External
  symbols: eleven ksce* calls (the table plus `ksceKernelPrintf`), `memcpy`, `memset`, `memcmp`,
  `__aeabi_uidivmod`, `__aeabi_uldivmod`.

## Deviations from upstream

From upstream OpenBSD: none in source. Behaviour that differs from an
OpenBSD kernel:

- Locks are two module-wide recursive locks (above), not per object.
- Pools and hash tables are fixed static storage; a full pool refuses.
- `KASSERT` is compiled out on the Vita by default; `panic` parks the
  thread.
- `arc4random_buf` is always the genuine hardware source.
- IPv6 is compiled out.

The protocol constants:

- `REKEY_TIMEOUT`, its jitter, `KEEPALIVE_TIMEOUT`, `MAX_TIMER_HANDSHAKES`
  and `NEW_HANDSHAKE_TIMEOUT` are not in `wg_noise`/`wg_cookie`; they are in
  `if_wg.c`, which is not vendored. `tn_core.h` restates them from `if_wg.c`
  v1.50 with attribution.
- The lock mapping above replaces a fast mutex per lock.
- Not yet verified on the console: the RTC tick's epoch and time zone (the
  shim assumes UTC microseconds since 0001-01-01, as the user-side RTC
  functions use), and the core running in a kernel thread at all.
