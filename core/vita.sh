#!/usr/bin/env bash
# Cross-compile the protocol core for the Vita kernel in the pinned VitaSDK
# Docker image (the same pin as the other builds). Output in build/vita/,
# owned by the host user.

set -euo pipefail

DEFAULT_IMAGE="vitasdk/vitasdk@sha256:8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb"
IMAGE="${VITASDK_IMAGE:-$DEFAULT_IMAGE}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

# Mount the repository root so that ../third_party resolves inside the container.
docker run --rm \
  -v "$ROOT:/src" -w /src/core \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" sh -ec '
    trap "chown -R \"\$HOST_UID:\$HOST_GID\" build 2>/dev/null || true" EXIT
    make -f vita.mk -j"$(nproc)" "$@"
  ' sh "$@"
ls -l "$HERE/build/vita/libtncore.a"
