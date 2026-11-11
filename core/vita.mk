# vita.mk - the core and shim (Vita kernel back end) as a static library,
# built inside the VitaSDK image by vita.sh. Toolchain defaults apply:
# armv7-a+simd, Thumb-2, -mfpu=neon, hard-float ABI, tuned for Cortex-A9.
#
#   build/vita/libtncore.a     link with the KERNEL_STUBS below
#   build/vita/libtncore.dis   its objdump listing, scanned for float opcodes
#   build/vita/linkcheck.elf   a dummy module linked against it all, to show
#                              that every symbol resolves

# ChaCha20-Poly1305: the CRYPTOGAMS assembly by default;
# make -f vita.mk TN_CRYPTO=portable for OpenBSD's C only.
TN_CRYPTO ?= cryptogams

CORE_DIR := .
include core.mk

PREFIX  ?= arm-vita-eabi-
CC      := $(PREFIX)gcc
AR      := $(PREFIX)ar
OBJDUMP := $(PREFIX)objdump
BUILD   := build/vita

CFLAGS_ALL := $(CORE_CFLAGS) -DTN_SHIM_VITA $(CORE_CPPFLAGS)

OBJS := \
	$(patsubst $(OBSD_DIR)/%.c,$(BUILD)/obsd/%.o,$(OBSD_SRCS)) \
	$(BUILD)/shim/tn_shim.o $(BUILD)/shim/tn_shim_vita.o \
	$(patsubst $(CORE_DIR)/crypto/%.c,$(BUILD)/crypto/%.o,$(CRYPTO_C_SRCS)) \
	$(patsubst $(CORE_DIR)/crypto/%.S,$(BUILD)/crypto/%.o,$(CRYPTO_S_SRCS))

KERNEL_STUBS := -lSceThreadmgrForDriver_stub -lSceSblSsMgrForDriver_stub \
	-lSceRtcForDriver_stub -lSceSysclibForDriver_stub -lSceDebugForDriver_stub

# Every object depends on this stamp, so switching TN_CRYPTO or
# TN_CRYPTO_NEON rebuilds them all.
STAMP := $(BUILD)/.crypto-$(TN_CRYPTO)-$(TN_CRYPTO_NEON)

.PHONY: all fpcheck
all: $(BUILD)/libtncore.a fpcheck $(BUILD)/linkcheck.elf

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

# Toolchain defaults (Thumb-2, NEON) apply; ASFLAGS_EXTRA=-marm for ARM mode.
$(BUILD)/crypto/%.o: crypto/%.S $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CRYPTO_ASFLAGS) $(ASFLAGS_EXTRA) -c $< -o $@

$(BUILD)/shim/%.o: shim/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD)/libtncore.a: $(OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/linkcheck.elf: tests/vita_linkcheck.c $(BUILD)/libtncore.a
	$(CC) $(CFLAGS_ALL) -I$(VITASDK)/arm-vita-eabi/include -nostartfiles \
	    -nostdlib -Wl,-q -Wl,-e,module_start $< $(BUILD)/libtncore.a $(KERNEL_STUBS) -lgcc -o $@

# Floating-point data processing: any VFP/NEON mnemonic with an .f16, .f32
# or .f64 type, conversions, square root, divide, and FP status register
# access. Integer NEON (vadd.i32, veor, vbsl, vld1) and VFP register loads,
# stores and moves (vldr, vstr, vmov) raise no FP exceptions and are fine.
FP_AWK := $$3 ~ /^v/ && ($$3 ~ /\.f(16|32|64)/ || $$3 ~ /^(vcvt|vsqrt|vdiv|vmrs|vmsr|vcmp)/)

fpcheck: $(BUILD)/libtncore.a
	$(OBJDUMP) -d $< > $(BUILD)/libtncore.dis
	@awk -F'\t' '$(FP_AWK) { print }' $(BUILD)/libtncore.dis > $(BUILD)/fp.txt
	@echo "VFP/NEON instructions (all kinds): $$(awk -F'\t' '$$3 ~ /^v/' $(BUILD)/libtncore.dis | wc -l)"
	@echo "floating-point instructions: $$(wc -l < $(BUILD)/fp.txt)"
	@test ! -s $(BUILD)/fp.txt
