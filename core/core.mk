# core.mk - sources and flags of the protocol core, shared by the host
# Makefile and the Vita build (vita.mk). Set CORE_DIR before including.

OBSD_DIR    := $(CORE_DIR)/../third_party/openbsd/sys

# Vendored OpenBSD sources, compiled unmodified.
OBSD_SRCS   := \
	$(OBSD_DIR)/net/wg_noise.c \
	$(OBSD_DIR)/net/wg_cookie.c \
	$(OBSD_DIR)/crypto/blake2s.c \
	$(OBSD_DIR)/crypto/curve25519.c \
	$(OBSD_DIR)/crypto/chachapoly.c \
	$(OBSD_DIR)/crypto/poly1305.c \
	$(OBSD_DIR)/crypto/siphash.c

SHIM_SRCS   := $(CORE_DIR)/shim/tn_shim.c

# ChaCha20-Poly1305 (include/tn_crypto.h). Set TN_CRYPTO
# before including this file, or on the make command line:
#   cryptogams  CRYPTOGAMS ARMv4/NEON assembly behind OpenBSD's four AEAD
#               entry points (crypto/chachapoly_cg.c); OpenBSD's chachapoly.c
#               is still built, its four AEAD functions renamed *_ref.
#               ARMv7 targets only (the Vita; an arm32 host via arm32.sh).
#   portable    OpenBSD's C only, as vendored.
# TN_CRYPTO_NEON=0 makes the cryptogams build start on the integer-only
# paths (tn_crypto_set_neon switches at run time either way).
TN_CRYPTO      ?= portable
TN_CRYPTO_NEON ?= 1
CG_DIR         := $(CORE_DIR)/../third_party/cryptogams

CRYPTO_C_SRCS  := $(CORE_DIR)/crypto/tn_crypto_bench.c
ifeq ($(TN_CRYPTO),cryptogams)
CRYPTO_C_SRCS  += $(CORE_DIR)/crypto/chachapoly_cg.c
CRYPTO_S_SRCS  := $(CORE_DIR)/crypto/cg_chacha.S \
	$(CORE_DIR)/crypto/cg_poly1305.S
CRYPTO_DEFS    := -DTN_CRYPTO_CRYPTOGAMS -DTN_CRYPTO_NEON=$(TN_CRYPTO_NEON)
# Applied to chachapoly.c only.
CRYPTO_REF_RENAME := \
	-Dchacha20poly1305_encrypt=chacha20poly1305_encrypt_ref \
	-Dchacha20poly1305_decrypt=chacha20poly1305_decrypt_ref \
	-Dxchacha20poly1305_encrypt=xchacha20poly1305_encrypt_ref \
	-Dxchacha20poly1305_decrypt=xchacha20poly1305_decrypt_ref
else ifeq ($(TN_CRYPTO),portable)
CRYPTO_C_SRCS  += $(CORE_DIR)/crypto/tn_crypto_portable.c
CRYPTO_S_SRCS  :=
CRYPTO_DEFS    :=
CRYPTO_REF_RENAME :=
else
$(error TN_CRYPTO must be cryptogams or portable, not '$(TN_CRYPTO)')
endif
CRYPTO_ASFLAGS := -I$(CG_DIR)/generated

# The shim header is forced in first; shim/include then catches the
# vendored files' <sys/systm.h> and friends before the system headers do,
# and third_party/openbsd/sys resolves <crypto/...>, <net/...> and
# <sys/queue.h>.
CORE_CPPFLAGS := \
	-include tn_shim.h \
	-I$(CORE_DIR)/include \
	-I$(CORE_DIR)/shim/include \
	-I$(OBSD_DIR) \
	$(CRYPTO_DEFS)

CORE_CFLAGS := -std=gnu99 -O2 -Wall -Wextra

# Upstream code trips these under -Wall -Wextra (unused parameters in
# OpenBSD's fixed signatures, int loop counters against sizeof, char vs
# uint8_t pointers, 16-byte "expand 32-byte k" arrays without the NUL).
# All are benign; they are silenced for the vendored files only.
# Build with WARN_ALL=1 to see them.
ifeq ($(WARN_ALL),)
OBSD_WFLAGS := -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign \
	-Wno-unknown-warning-option -Wno-unterminated-string-initialization
endif
