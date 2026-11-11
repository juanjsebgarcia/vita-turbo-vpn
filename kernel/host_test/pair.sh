#!/usr/bin/env bash
# Two copies of kernel/proto.c against each other on the host (tn_pair_test.c):
# handshake counter, rehandshake, stop, same-key check, and three threads
# through the send path at once. No network and no reference peer needed (unlike
# run.sh). Runs in an Alpine container.
#
#   ./pair.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"   # the repository root

docker run --rm -v "$ROOT:/src" -w /src alpine:latest sh -ec '
  apk add -q build-base binutils >/dev/null
  mkdir -p /tmp/o /tmp/b
  # Two peers, two rate-limiter pools and tables (the module has one).
  CORE="-DTN_SHIM_HOST -DTN_POOL_SLOTS=2 -DTN_HASH_SLOTS=2 -include tn_shim.h \
        -Icore/include -Icore/shim/include -Ithird_party/openbsd/sys -Ikernel \
        -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign -Wno-unterminated-string-initialization"
  for f in kernel/proto.c core/shim/tn_shim.c core/shim/tn_shim_host.c \
           third_party/openbsd/sys/net/wg_noise.c \
           third_party/openbsd/sys/net/wg_cookie.c \
           third_party/openbsd/sys/crypto/blake2s.c \
           third_party/openbsd/sys/crypto/curve25519.c \
           third_party/openbsd/sys/crypto/chachapoly.c \
           third_party/openbsd/sys/crypto/poly1305.c \
           third_party/openbsd/sys/crypto/siphash.c; do
    cc -O2 -std=gnu99 $CORE -c "$f" -o "/tmp/o/$(basename "$f" .c).o"
  done
  # Copy B: the same proto object, its public tnp_* renamed to tnpb_*
  # (tnp_log stays: the harness supplies one for both).
  nm /tmp/o/proto.o | awk "\$2 == \"T\" && \$3 ~ /^tnp_/ { sub(/^tnp_/, \"\", \$3); print \"tnp_\" \$3 \" tnpb_\" \$3 }" > /tmp/b/syms
  objcopy --redefine-syms=/tmp/b/syms /tmp/o/proto.o /tmp/b/proto_b.o
  cc -O2 -Wall -Wextra -std=gnu99 -Ikernel -c kernel/host_test/tn_pair_test.c -o /tmp/b/pair.o
  cc -o /tmp/tn_pair_test /tmp/b/pair.o /tmp/b/proto_b.o /tmp/o/*.o -lpthread
  /tmp/tn_pair_test
'
