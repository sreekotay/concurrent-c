#!/usr/bin/env bash
# compare_arena_tiers.sh — where to draw the slab-budget line.
#
# Same bulk workload, small root, sweep block_max:
#   1  = 2-tier fixed root + malloc overflow (cc_arena_malloc shape)
#   N  = 3-tier: up to N slabs, then overflow
#   0  = 2-tier unbounded extents (grow forever; ovf only if grow OOM)
#
# Root is intentionally small so policies actually diverge (large-root
# benches keep bulk_ovf=0 and hide the tier question).
#
# Env:
#   ARENA_TIER_POLICIES  default "1 2 4 8 0"
#   ARENA_MEM_TRIALS     default 3
#   ARENA_MEM_QUICK=1    lighter bulk
#   ARENA_MEM_BULK_ROOT  default 1MiB (small on purpose)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$SCRIPT_DIR/out"
CCC="${CCC:-}"
if [ -z "$CCC" ]; then
  if [ -x "$REPO_ROOT/cc/bin/ccc" ]; then CCC="$REPO_ROOT/cc/bin/ccc"
  elif [ -x "$REPO_ROOT/out/cc/bin/ccc" ]; then CCC="$REPO_ROOT/out/cc/bin/ccc"
  else echo "error: ccc not found" >&2; exit 1; fi
fi

TRIALS="${ARENA_MEM_TRIALS:-3}"
POLICIES="${ARENA_TIER_POLICIES:-1 2 4 8 0}"
SEED="${ARENA_MEM_SEED:-$(date +%s)-$$}"

mkdir -p "$OUT"

# Keep tip/ovf/churn cheap — this sweep is about bulk ownership under policy.
export ARENA_MEM_TIP_ROUNDS="${ARENA_MEM_TIP_ROUNDS:-20}"
export ARENA_MEM_TIP_STEPS="${ARENA_MEM_TIP_STEPS:-500}"
export ARENA_MEM_TIP_ROOT="${ARENA_MEM_TIP_ROOT:-$((2 * 1024 * 1024))}"
export ARENA_MEM_OVF="${ARENA_MEM_OVF:-0}"
export ARENA_MEM_CHURN="${ARENA_MEM_CHURN:-50}"
export ARENA_MEM_CHURN_EACH="${ARENA_MEM_CHURN_EACH:-64}"
export ARENA_MEM_BULK_ROOT="${ARENA_MEM_BULK_ROOT:-$((1024 * 1024))}"

if [ "${ARENA_MEM_QUICK:-0}" = "1" ]; then
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-300000}"
else
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-1500000}"
fi

echo "================================================================="
echo "ARENA TIERS — block_max sweep (CC + C bump + malloc baseline)"
echo "================================================================="
echo "trials=$TRIALS seed=$SEED policies: $POLICIES"
echo "bulk_n=$ARENA_MEM_BULK bulk_root=$ARENA_MEM_BULK_ROOT (small root → tiers diverge)"
echo ""
echo "Legend:"
echo "  block_max=1  2-tier fixed+ovf     (root only, then malloc)"
echo "  block_max=N  3-tier budget        (N slabs, then malloc)"
echo "  block_max=0  2-tier unbounded     (extents only)"
echo "  Watch bulk_ms (alloc+reclaim), bulk_ovf, reset_ms, bulk_gross."
echo "================================================================="
echo ""

echo "Building..."
"$CCC" build --release "$SCRIPT_DIR/arena_memory_bench.ccs" -o "$OUT/arena_memory_bench_cc" >/dev/null
cc -O2 -std=c11 "$SCRIPT_DIR/c/arena_memory_bench.c" -o "$OUT/arena_memory_bench_c"
cc -O2 -std=c11 "$SCRIPT_DIR/c/malloc_memory_bench.c" -o "$OUT/arena_memory_bench_malloc"
echo "Done."
echo ""

RAW="$OUT/arena_tier_results.raw"
: > "$RAW"

run_one() {
  local lang=$1 bin=$2 bm=$3 trial=$4
  local out line
  echo "  [$trial] $lang block_max=$bm"
  if ! out=$(ARENA_MEM_BLOCK_MAX="$bm" "$bin" 2>&1); then
    echo "$out"
    echo "error: $lang bm=$bm failed" >&2
    exit 1
  fi
  line=$(echo "$out" | grep '^RESULT ' || true)
  if [ -z "$line" ]; then
    echo "$out"
    echo "error: no RESULT" >&2
    exit 1
  fi
  echo "    $line"
  echo "trial=$trial $line" >> "$RAW"
}

# malloc baseline once per trial (no block_max)
for ((t = 1; t <= TRIALS; t++)); do
  echo "--- trial $t/$TRIALS ---"
  run_one malloc "$OUT/arena_memory_bench_malloc" - "$t"
  for bm in $POLICIES; do
    run_one cc "$OUT/arena_memory_bench_cc" "$bm" "$t"
    run_one c  "$OUT/arena_memory_bench_c"  "$bm" "$t"
    sleep 0.03
  done
  echo ""
done

echo "================================================================="
echo "AVERAGES — bulk ownership under tier policy"
echo "================================================================="
printf "%-8s %10s %10s %12s %12s %10s\n" \
  "lang" "block_max" "bulk_ms" "bulk_ovf" "bulk_gross" "reset_ms"
printf "%-8s %10s %10s %12s %12s %10s\n" \
  "--------" "----------" "--------" "----------" "----------" "--------"

awk '
/^trial=/ {
  lang=""; bm="-"; bulk=0; bovf=0; gross=0; reset=0
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^lang=/) { split($i, a, "="); lang = a[2] }
    if ($i ~ /^block_max=/) { split($i, a, "="); bm = a[2] }
    if ($i ~ /^bulk_ms=/) { split($i, a, "="); bulk = a[2]+0 }
    if ($i ~ /^bulk_ovf=/) { split($i, a, "="); bovf = a[2]+0 }
    if ($i ~ /^bulk_gross=/) { split($i, a, "="); gross = a[2]+0 }
    if ($i ~ /^reset_ms=/) { split($i, a, "="); reset = a[2]+0 }
  }
  if (lang == "malloc") bm = "-"
  key = lang "|" bm
  n[key]++; bulk_s[key]+=bulk; bovf_s[key]+=bovf; gross_s[key]+=gross; reset_s[key]+=reset
  if (!(key in order)) { order[key]=++nord; keys[nord]=key }
}
END {
  for (i = 1; i <= nord; i++) {
    key = keys[i]
    nn = n[key]
    split(key, p, "|")
    printf "%-8s %10s %10.2f %12.0f %12.0f %10.2f\n", \
      p[1], p[2], bulk_s[key]/nn, bovf_s[key]/nn, gross_s[key]/nn, reset_s[key]/nn
  }
}
' "$RAW"

echo ""
echo "raw: $RAW"
echo "Read: if block_max=1 (fixed+ovf) is much slower than 0/4 on bulk_ms or reset_ms,"
echo "      3-tier (or unbounded) is earning its keep. Pick the smallest N where"
echo "      bulk_ovf collapses / bulk_ms plateaus — that is the draw line."
echo "================================================================="
