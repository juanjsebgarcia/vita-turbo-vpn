#!/usr/bin/env bash
# Build the app in the pinned VitaSDK Docker image, as the other builds do.
# Output: build/vpnturbo.vpk, owned by the host user. Extra arguments go to
# cmake, e.g. ./build.sh -DVPNTURBO_BACKEND=vt
#
#   ./build.sh release   the release: real back end only, in build-release/
#                        (a release cannot be built with the mock back end)

set -euo pipefail

DEFAULT_IMAGE="vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb"
IMAGE="${VITASDK_IMAGE:-$DEFAULT_IMAGE}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=build
OUT=vpnturbo.vpk
if [ "${1:-}" = release ]; then
  shift
  BUILD=build-release
  set -- -DVPNTURBO_RELEASE=ON -DVPNTURBO_BACKEND=vt "$@"
fi

# Mount the repository root so the fonts in ../third_party are reachable.
docker run --rm \
  -v "$HERE/..:/src" -w /src/app \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -e BUILD="$BUILD" \
  "$IMAGE" sh -ec '
    trap "chown -R \"\$HOST_UID:\$HOST_GID\" \"\$BUILD\" 2>/dev/null || true" EXIT
    cmake -S . -B "$BUILD" "$@" >/dev/null
    make -C "$BUILD" -j"$(nproc)"
  ' sh "$@"
ls -l "$HERE/$BUILD/$OUT"
