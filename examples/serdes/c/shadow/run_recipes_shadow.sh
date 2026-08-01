#!/usr/bin/env bash
# Behavioral gate: shadow-lower real examples → host cc → link runtime → run.
# Parallel path — TCC not required for this product consume step.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SHADOW_SH="$ROOT/examples/serdes/c/shadow_lower.sh"
RT_O="$ROOT/out/cc/obj/runtime/concurrent_c.o"
TMP="${TMPDIR:-/tmp}/cc_serdes_recipes_$$"
mkdir -p "$TMP"

if [[ ! -f "$RT_O" ]]; then
  echo "FAIL: missing $RT_O (build cc runtime first)"
  exit 1
fi

run_one() {
  local src="$1"
  shift
  local name
  name="$(basename "$src" .ccs)"
  "$SHADOW_SH" "$ROOT/$src" -o "$TMP/$name.c"
  cc -std=c11 -I"$ROOT/out/include" -c "$TMP/$name.c" -o "$TMP/$name.o"
  cc "$TMP/$name.o" "$RT_O" -o "$TMP/$name.bin" -lpthread
  local out
  out="$("$TMP/$name.bin")"
  printf '%s\n' "$out"
  local need
  for need in "$@"; do
    if ! grep -Fq "$need" <<<"$out"; then
      echo "FAIL: $name missing expected line: $need"
      exit 1
    fi
  done
  echo "run_recipes_shadow: $name OK"
}

run_one examples/hello.ccs \
  "Hello from task A! (last)" \
  "Hello from task B!" \
  "Hello from task C!" \
  "All tasks completed."

run_one examples/recipe_result_error_handling.ccs \
  "total wait time: 90 seconds" \
  "missing key fell back to: -1" \
  "invalid key mapped to: 0" \
  "compose ok: timeout=30"

run_one examples/recipe_arena_scope.ccs \
  "Processing request 1 bytes: 4096" \
  "Processing request 2 bytes: 4096" \
  "Processing request 3 bytes: 4096"

run_one examples/recipe_explicit_capture.ccs \
  "=== value capture ===" \
  "task A sees snapshot = 42" \
  "=== reference capture (read-only) ===" \
  "task C reads counter = 100"

# Timing-sensitive: exact iteration counts vary; recipe itself asserts 1..10.
run_one examples/recipe_timeout.ccs \
  "iterations before deadline" \
  "checkpoints against handle"

echo "run_recipes_shadow: all OK"
rm -rf "$TMP"
