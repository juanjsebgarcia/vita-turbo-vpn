#!/usr/bin/env bash
# The interface TX thread's re-arm decision (kernel/iface_rearm.h) on the
# host: header-only, so any C compiler builds it; no Docker needed.
#
#   ./rearm.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
cc -O2 -Wall -Wextra -Werror -std=gnu99 -I"$HERE/.." "$HERE/rearm_test.c" -o "$OUT/rearm_test"
"$OUT/rearm_test"
