#!/usr/bin/env bash
# compare_arena_malloc_cost.sh — cost of using cc_arena_malloc for tons of allocs.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$SCRIPT_DIR/out"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
[ -x "$CCC" ] || CCC="$REPO_ROOT/out/cc/bin/ccc"

mkdir -p "$OUT"
if [ "${ARENA_MEM_QUICK:-0}" = "1" ]; then
  export ARENA_MALLOC_COST_N="${ARENA_MALLOC_COST_N:-200000}"
else
  export ARENA_MALLOC_COST_N="${ARENA_MALLOC_COST_N:-800000}"
fi
export ARENA_MALLOC_COST_ROOT="${ARENA_MALLOC_COST_ROOT:-$((64 * 1024))}"
export ARENA_MALLOC_COST_FAT_ROOT="${ARENA_MALLOC_COST_FAT_ROOT:-$((16 * 1024 * 1024))}"

echo "================================================================="
echo "cc_arena_malloc cost vs heap / raw malloc"
echo "================================================================="
echo "n=$ARENA_MALLOC_COST_N tiny_root=$ARENA_MALLOC_COST_ROOT fat_root=$ARENA_MALLOC_COST_FAT_ROOT"
echo ""

"$CCC" build --release "$SCRIPT_DIR/arena_malloc_cost_bench.ccs" \
  -o "$OUT/arena_malloc_cost_bench" >/dev/null
"$OUT/arena_malloc_cost_bench"

echo ""
echo "Read: arena_malloc = per-object ovf (durable); reset frees N mallocs."
echo "      heap+tiny still overflows, but scratch ovf uses 64KiB bump chunks."
echo "      heap+fat root (fits live) is the request-scratch path — prefer that."
echo "================================================================="
