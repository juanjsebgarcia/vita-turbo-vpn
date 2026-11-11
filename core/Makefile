# Host build and tests of the protocol core.
#
#   make            libtncore.a for the host, and the test programs
#   make test       build and run every test
#   make vita       cross-compile the Vita kernel back end (Docker, vita.sh)
#   make bench      build and run the crypto benchmark (build/host/bench_crypto)
#   make clean
#
# WARN_ALL=1 shows the upstream warnings that core.mk silences.
# TN_CRYPTO=cryptogams|portable picks the ChaCha20-Poly1305 (core.mk); the
# default is cryptogams on a 32-bit ARM host (arm32.sh runs one in Docker),
# portable everywhere else, since the assembly is ARMv7 only.

ifeq ($(filter armv7%,$(shell uname -m)),)
TN_CRYPTO ?= portable
else
TN_CRYPTO ?= cryptogams
endif

CORE_DIR := .
include core.mk

CC      ?= cc
BUILD   ?= build/host

# Desk-only sizes: the OpenBSD rate-limit self-test fills RATELIMIT_SIZE_MAX
# (65536) entries, and the end-to-end test runs two cookie checkers.
HOST_DEFS := -DTN_SHIM_HOST -DTN_DIAGNOSTIC \
	-DTN_POOL_SLOTS=2 -DTN_POOL_ITEMS=65536 -DTN_HASH_SLOTS=2

CFLAGS_ALL  := $(CORE_CFLAGS) $(HOST_DEFS) $(CORE_CPPFLAGS)
LDLIBS      := -lpthread

LIB_OBJS := \
	$(patsubst $(OBSD_DIR)/%.c,$(BUILD)/obsd/%.o,$(OBSD_SRCS)) \
	$(BUILD)/shim/tn_shim.o $(BUILD)/shim/tn_shim_host.o \
	$(patsubst $(CORE_DIR)/crypto/%.c,$(BUILD)/crypto/%.o,$(CRYPTO_C_SRCS)) \
	$(patsubst $(CORE_DIR)/crypto/%.S,$(BUILD)/crypto/%.o,$(CRYPTO_S_SRCS))

# The self-test build: the two protocol files again, with WGTEST.
TNTEST_OBJS := $(BUILD)/tntest/wg_noise.o $(BUILD)/tntest/wg_cookie.o
NOTNTEST_OBJS := $(filter-out $(BUILD)/obsd/net/%,$(LIB_OBJS))

TESTS := $(BUILD)/test_vectors $(BUILD)/test_selftest $(BUILD)/test_e2e \
	$(BUILD)/test_crypto

# Every object depends on this stamp, so switching TN_CRYPTO or
# TN_CRYPTO_NEON rebuilds them all.
STAMP := $(BUILD)/.crypto-$(TN_CRYPTO)-$(TN_CRYPTO_NEON)

.PHONY: all test bench vita clean
all: $(BUILD)/libtncore.a $(TESTS) $(BUILD)/bench_crypto

$(STAMP):
	@mkdir -p $(BUILD)
	rm -f $(BUILD)/.crypto-*
	@touch $@

$(BUILD)/obsd/crypto/chachapoly.o: OBJ_DEFS := $(CRYPTO_REF_RENAME)

$(BUILD)/obsd/%.o: $(OBSD_DIR)/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) $(OBSD_WFLAGS) $(OBJ_DEFS) -c $< -o $@

# chachapoly_cg.c includes the vendored chacha_private.h: same warning set.
$(BUILD)/crypto/%.o: crypto/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) $(OBSD_WFLAGS) -c $< -o $@

$(BUILD)/crypto/%.o: crypto/%.S $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CRYPTO_ASFLAGS) $(ASFLAGS_EXTRA) -c $< -o $@

$(BUILD)/tntest/%.o: $(OBSD_DIR)/net/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) $(OBSD_WFLAGS) -DWGTEST -c $< -o $@

$(BUILD)/shim/%.o: shim/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD)/libtncore.a: $(LIB_OBJS)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/test_vectors: tests/test_vectors.c tests/vectors.h tests/harness.h $(BUILD)/libtncore.a
	$(CC) $(CFLAGS_ALL) $(OBSD_WFLAGS) $< $(BUILD)/libtncore.a $(LDLIBS) -o $@

$(BUILD)/test_e2e: tests/test_e2e.c tests/harness.h $(BUILD)/libtncore.a
	$(CC) $(CFLAGS_ALL) $< $(BUILD)/libtncore.a $(LDLIBS) -o $@

$(BUILD)/test_crypto: tests/test_crypto.c tests/harness.h $(BUILD)/libtncore.a
	$(CC) $(CFLAGS_ALL) $< $(BUILD)/libtncore.a $(LDLIBS) -o $@

$(BUILD)/bench_crypto: tests/bench_crypto.c $(BUILD)/libtncore.a
	$(CC) $(CFLAGS_ALL) $< $(BUILD)/libtncore.a $(LDLIBS) -o $@

$(BUILD)/test_selftest: tests/test_selftest.c $(TNTEST_OBJS) $(NOTNTEST_OBJS)
	$(CC) $(CFLAGS_ALL) -DWGTEST $< $(TNTEST_OBJS) $(NOTNTEST_OBJS) $(LDLIBS) -o $@

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done
	@echo "all host tests passed (TN_CRYPTO=$(TN_CRYPTO))"

bench: $(BUILD)/bench_crypto
	$(BUILD)/bench_crypto

vita:
	./vita.sh

clean:
	rm -rf build
