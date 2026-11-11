#!/bin/sh
# Host tests of the kernel module's pure parts: the stranded-connection reset
# (strand_core.c, strand_fw.c) and the reset builder (tcp_rst.c).
#   sh tests/run.sh          from kernel/ (or anywhere)
set -eu
cd "$(dirname "$0")"
T="${TMPDIR:-/tmp}/vpnturbo-kernel-tests.$$"
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
CC="${CC:-cc}"
FLAGS="-std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I.."
$CC $FLAGS test_tcp_rst.c ../tcp_rst.c -o "$T/tcp_rst"
"$T/tcp_rst"
$CC $FLAGS -DSTRAND_HOST test_strand.c ../strand_core.c ../strand_fw.c ../tcp_rst.c -o "$T/strand"
"$T/strand"
