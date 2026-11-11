#!/bin/sh
# The stranded-connection hook (strand_hook.c) and the frame test and ring it
# calls (strand_core.c) run inside the network stack's own calls, in whatever
# thread makes them: they must be leaf and non-blocking. The object files must
#   - define the hook and the core functions (so an empty or misnamed file
#     cannot pass),
#   - contain no floating-point or vector instruction anywhere, and no system
#     call instruction (svc). The address-translation check (mcr/mrc on the
#     translation registers, with interrupts masked across the pair) is
#     allowed: it is a processor operation, not a call,
#   - in every function of the two objects, call nothing but each other: every
#     relocation of a call or a jump names one of the allowed functions. The
#     call through the reference to the original routine is indirect, so it
#     carries no relocation; a library routine (memcpy, a division, an
#     out-of-line atomic), a system call, a lock or a wait would.
# Run by the build after the link, on both object files:
#   check_strand.sh strand_core.c.obj strand_hook.c.obj
set -eu
OBJDUMP="${OBJDUMP:-arm-vita-eabi-objdump}"
[ -n "$OBJDUMP" ] || OBJDUMP=arm-vita-eabi-objdump
ALLOWED="${STRAND_ALLOWED_CALLS:-strand_capture strand_frame_headers}"
REQUIRED="strand_hook strand_capture strand_frame_headers strand_pop"
VEC='[[:space:]]v(ld|st|mov|add|sub|mul|cvt|cmp|dup|and|orr|eor|ext|zip|swp|rev|shl|shr|push|pop|mrs|msr|abs|neg|sqrt|div|fma|nmul|nmla|nmls|mla|mls|rec|rsq|sel|max|min|bic|bsl|bif|bit|tbl|trn|uzp|qadd|qsub|qmov)[a-z0-9.]*[[:space:]]'
dump=""
for obj in "$@"; do
  dump="$dump
$("$OBJDUMP" -dr "$obj")"
done
for sym in $REQUIRED; do
  case "$dump" in *"<$sym>:"*) ;; *) echo "check_strand: no $sym in $*" >&2; exit 1 ;; esac
done
bad="$(printf '%s\n' "$dump" | grep -E "^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f ]+$VEC" || true)"
if [ -n "$bad" ]; then
  echo "check_strand: floating-point/vector instruction (the hook may use general registers only):" >&2
  printf '%s\n' "$bad" >&2
  exit 1
fi
svc="$(printf '%s\n' "$dump" | grep -E "^[[:space:]]+[0-9a-f]+:[[:space:]]+[0-9a-f ]+[[:space:]]svc[[:space:]]" || true)"
if [ -n "$svc" ]; then
  echo "check_strand: system call instruction in the hook objects:" >&2
  printf '%s\n' "$svc" >&2
  exit 1
fi
# Every call or jump relocation, as "function target".
calls="$(printf '%s\n' "$dump" | awk '
  /^[0-9a-f]+ <[^>]+>:/ { fn = $2; gsub(/[<>:]/, "", fn); sub(/\..*/, "", fn); next }
  /R_ARM_[A-Z0-9_]*(CALL|JUMP|PLT)[A-Z0-9_]*[ \t]/ { print fn, $3 }')"
echo "$calls" | while read -r fn target; do
  [ -n "$fn" ] || continue
  ok=0
  for a in $ALLOWED; do
    [ "$target" = "$a" ] && ok=1
  done
  if [ "$ok" = 0 ]; then
    echo "check_strand: $fn calls $target (allowed: $ALLOWED)" >&2
    exit 1
  fi
done || exit 1
echo "check_strand: no floating-point/vector/svc instructions; calls only: $(printf '%s\n' "$calls" | awk 'NF { print $1 "->" $2 }' | sort -u | tr '\n' ' ')"
