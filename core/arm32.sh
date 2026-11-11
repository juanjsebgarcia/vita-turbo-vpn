#!/usr/bin/env bash
# Build and run the host tests and the crypto benchmark as 32-bit ARMv7
# Linux (Alpine armhf) in Docker, so the CRYPTOGAMS assembly runs on the
# desk. On an x86-64 or Apple Silicon machine Docker runs it under QEMU
# user-mode emulation: results check correctness, and the timings are the
# emulator's, not a Cortex-A9's.
#
#   ./arm32.sh                  make test bench (TN_CRYPTO=cryptogams)
#   ./arm32.sh test             any make targets/variables, e.g.
#   ./arm32.sh test ASFLAGS_EXTRA=-marm      the assembly in ARM mode
#   ./arm32.sh test TN_CRYPTO=portable
#
# Output in build/arm32/.
set -euo pipefail

IMAGE="${ARM32_IMAGE:-alpine:3.20}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
[ $# -gt 0 ] || set -- test bench

docker run --rm --platform linux/arm/v7 \
  -v "$ROOT:/src" -w /src/core \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" sh -ec '
    trap "chown -R \"\$HOST_UID:\$HOST_GID\" build 2>/dev/null || true" EXIT
    apk add --no-cache -q build-base >/dev/null
    echo "---- $(uname -m), $(gcc -dumpmachine)"
    make -j"$(nproc)" BUILD=build/arm32 "$@"
  ' sh "$@"
