#!/usr/bin/env bash
# Milestone 2: shadow-lower real examples/hello.ccs → host cc → link runtime → run.
# Parallel path: SERDES owns the transform; host cc + concurrent_c.o consume the .c
# (TCC is not required for this product path).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SRC="$ROOT/examples/hello.ccs"
SHADOW_SH="$ROOT/cc/shadow/shadow_lower.sh"
RT_O="$ROOT/out/cc/obj/runtime/concurrent_c.o"
TMP="${TMPDIR:-/tmp}/cc_serdes_hello_$$"
mkdir -p "$TMP"

if [[ ! -f "$RT_O" ]]; then
  echo "FAIL: missing $RT_O (build cc runtime first)"
  exit 1
fi

"$SHADOW_SH" "$SRC" -o "$TMP/hello.shadow.c"
cc -std=c11 -I"$ROOT/out/include" -c "$TMP/hello.shadow.c" -o "$TMP/hello.o"
cc "$TMP/hello.o" "$RT_O" -o "$TMP/hello.bin" -lpthread

out="$("$TMP/hello.bin")"
printf '%s\n' "$out"
for need in \
  "Hello from task A! (last)" \
  "Hello from task B!" \
  "Hello from task C!" \
  "All tasks completed."; do
  if ! grep -Fq "$need" <<<"$out"; then
    echo "FAIL: missing expected line: $need"
    exit 1
  fi
done
echo "run_hello_shadow: behavioral OK"
rm -rf "$TMP"
