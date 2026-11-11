#!/usr/bin/env bash
# Cross-compile the parser for a Vita user app in the pinned VitaSDK Docker
# image (the same pin as the other builds) and list the symbols it needs
# from outside. Output: build/vita/libvtconf.a, owned by the host user.

set -euo pipefail

DEFAULT_IMAGE="vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb"
IMAGE="${VITASDK_IMAGE:-$DEFAULT_IMAGE}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

docker run --rm \
  -v "$HERE/..:/src" -w /src/conf \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" sh -ec '
    trap "chown -R \"\$HOST_UID:\$HOST_GID\" build 2>/dev/null || true" EXIT
    mkdir -p build/vita
    CC="$VITASDK/bin/arm-vita-eabi-gcc"
    FLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -O2 -ffunction-sections -fdata-sections"
    $CC --version | head -1
    for f in vtconf vtconf_util; do
      $CC $FLAGS -c $f.c -o build/vita/$f.o
    done
    "$VITASDK/bin/arm-vita-eabi-ar" rcs build/vita/libvtconf.a build/vita/vtconf.o build/vita/vtconf_util.o
    echo "external symbols:"
    "$VITASDK/bin/arm-vita-eabi-nm" -u build/vita/libvtconf.a | grep " U " | sort -u | grep -v " vtc_" | sed "s/^ */  /"
    "$VITASDK/bin/arm-vita-eabi-size" build/vita/libvtconf.a
  '
