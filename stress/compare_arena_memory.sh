#!/usr/bin/env bash
# compare_arena_memory.sh — heavy bump-arena protocol across CC / C / Go / Zig
# plus a raw malloc baseline.
#
# Builds peers, shuffles run order each trial (startup / cache coldness),
# averages RESULT lines.
#
# Env knobs (forwarded to all peers):
#   ARENA_MEM_TIP_ROUNDS ARENA_MEM_TIP_STEPS ARENA_MEM_TIP_ROOT
#   ARENA_MEM_BULK ARENA_MEM_BULK_ROOT
#   ARENA_MEM_OVF ARENA_MEM_OVF_ROOT
#   ARENA_MEM_CHURN ARENA_MEM_CHURN_EACH ARENA_MEM_BLOCK_MAX
#   ARENA_MEM_TRIALS (default 3)  ARENA_MEM_SEED
#   ARENA_MEM_QUICK=1 → lighter knobs for smoke
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$SCRIPT_DIR/out"
CCC="${CCC:-}"
if [ -z "$CCC" ]; then
  if [ -x "$REPO_ROOT/cc/bin/ccc" ]; then
    CCC="$REPO_ROOT/cc/bin/ccc"
  elif [ -x "$REPO_ROOT/out/cc/bin/ccc" ]; then
    CCC="$REPO_ROOT/out/cc/bin/ccc"
  else
    echo "error: ccc not found (set CCC=...)" >&2
    exit 1
  fi
fi

TRIALS="${ARENA_MEM_TRIALS:-3}"
SEED="${ARENA_MEM_SEED:-$(date +%s)-$$}"

mkdir -p "$OUT"

if [ "${ARENA_MEM_QUICK:-0}" = "1" ]; then
  export ARENA_MEM_TIP_ROUNDS="${ARENA_MEM_TIP_ROUNDS:-40}"
  export ARENA_MEM_TIP_STEPS="${ARENA_MEM_TIP_STEPS:-500}"
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-200000}"
  export ARENA_MEM_OVF="${ARENA_MEM_OVF:-20000}"
  export ARENA_MEM_CHURN="${ARENA_MEM_CHURN:-100}"
else
  export ARENA_MEM_TIP_ROUNDS="${ARENA_MEM_TIP_ROUNDS:-400}"
  export ARENA_MEM_TIP_STEPS="${ARENA_MEM_TIP_STEPS:-5000}"
  export ARENA_MEM_BULK="${ARENA_MEM_BULK:-2000000}"
  export ARENA_MEM_OVF="${ARENA_MEM_OVF:-200000}"
  export ARENA_MEM_CHURN="${ARENA_MEM_CHURN:-1000}"
fi
export ARENA_MEM_TIP_ROOT="${ARENA_MEM_TIP_ROOT:-$((8 * 1024 * 1024))}"
export ARENA_MEM_BULK_ROOT="${ARENA_MEM_BULK_ROOT:-$((64 * 1024 * 1024))}"
export ARENA_MEM_OVF_ROOT="${ARENA_MEM_OVF_ROOT:-4096}"
export ARENA_MEM_CHURN_EACH="${ARENA_MEM_CHURN_EACH:-128}"
export ARENA_MEM_BLOCK_MAX="${ARENA_MEM_BLOCK_MAX:-4}"

echo "================================================================="
echo "ARENA MEMORY — heavy protocol (CC / C / Go / Zig / malloc)"
echo "================================================================="
echo "trials=$TRIALS seed=$SEED"
echo "tip=${ARENA_MEM_TIP_ROUNDS}x${ARENA_MEM_TIP_STEPS} bulk=$ARENA_MEM_BULK ovf=$ARENA_MEM_OVF"
echo "bulk_root=$ARENA_MEM_BULK_ROOT block_max=$ARENA_MEM_BLOCK_MAX"
echo "ccc=$CCC"
echo "================================================================="
echo ""
echo "How to read (times are full ownership = alloc + reclaim):"
echo "  tip       — tip-grow rounds including reset/free between rounds."
echo "  bulk      — large-root keep-alive allocs + reset (or malloc free-all)."
echo "  reset_ms  — reclaim portion of bulk only (breakdown)."
echo "  ovf       — tiny-root spill path + free/drain of overflow."
echo "  churn     — create/fill/destroy arenas (or malloc/free pairs)."
echo ""

echo "Building..."
"$CCC" build --release "$SCRIPT_DIR/arena_memory_bench.ccs" -o "$OUT/arena_memory_bench_cc" >/dev/null
cc -O2 -std=c11 "$SCRIPT_DIR/c/arena_memory_bench.c" -o "$OUT/arena_memory_bench_c"
cc -O2 -std=c11 "$SCRIPT_DIR/c/malloc_memory_bench.c" -o "$OUT/arena_memory_bench_malloc"
if command -v go >/dev/null 2>&1; then
  go build -o "$OUT/arena_memory_bench_go" "$SCRIPT_DIR/go/arena_memory_bench.go"
else
  echo "  (skip go — not installed)"
fi
if command -v zig >/dev/null 2>&1; then
  zig build-exe "$SCRIPT_DIR/zig/arena_memory_bench.zig" -O ReleaseFast -lc \
    -femit-bin="$OUT/arena_memory_bench_zig" >/dev/null
else
  echo "  (skip zig — not installed)"
fi
echo "Done."
echo ""

RUNNERS=()
RUNNERS+=("cc|$OUT/arena_memory_bench_cc")
RUNNERS+=("c|$OUT/arena_memory_bench_c")
RUNNERS+=("malloc|$OUT/arena_memory_bench_malloc")
[ -x "$OUT/arena_memory_bench_go" ] && RUNNERS+=("go|$OUT/arena_memory_bench_go")
[ -x "$OUT/arena_memory_bench_zig" ] && RUNNERS+=("zig|$OUT/arena_memory_bench_zig")

shuffle_runners() {
  local trial=$1
  local n=${#RUNNERS[@]}
  local i j tmp
  local -a arr=("${RUNNERS[@]}")
  local stream
  stream=$(printf '%s' "${SEED}-t${trial}" | cksum | awk '{print $1}')
  for ((i = n - 1; i > 0; i--)); do
    stream=$(( (stream * 1103515245 + 12345) & 0x7fffffff ))
    j=$(( stream % (i + 1) ))
    tmp=${arr[i]}
    arr[i]=${arr[j]}
    arr[j]=$tmp
  done
  printf '%s\n' "${arr[@]}"
}

RESULTS_RAW="$OUT/arena_memory_results.raw"
: > "$RESULTS_RAW"

for ((t = 1; t <= TRIALS; t++)); do
  echo "--- trial $t/$TRIALS (shuffled) ---"
  order_names=()
  while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    name="${entry%%|*}"
    bin="${entry#*|}"
    order_names+=("$name")
    echo "  run $name"
    sleep 0.05
    if ! out=$("$bin" 2>&1); then
      echo "$out"
      echo "error: $name failed trial $t" >&2
      exit 1
    fi
    echo "$out" | sed 's/^/    /'
    line=$(echo "$out" | grep '^RESULT ' || true)
    if [ -z "$line" ]; then
      echo "error: no RESULT from $name" >&2
      exit 1
    fi
    echo "trial=$t order=${order_names[*]} $line" >> "$RESULTS_RAW"
  done < <(shuffle_runners "$t")
  echo "  order: ${order_names[*]}"
  echo ""
done

echo "================================================================="
echo "AVERAGES (n=$TRIALS, order randomized per trial)"
echo "================================================================="
printf "%-8s %9s %9s %10s %10s %9s %9s %9s\n" \
  "lang" "tip_ms" "tip_mv" "bulk_ms" "bulk_ovf" "ovf_ms" "reset_ms" "churn_ms"
printf "%-8s %9s %9s %10s %10s %9s %9s %9s\n" \
  "--------" "-------" "-------" "--------" "--------" "-------" "--------" "--------"

awk '
/^trial=/ {
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^lang=/) { split($i, a, "="); lang = a[2] }
    if ($i ~ /^tip_ms=/) { split($i, a, "="); tip = a[2]+0 }
    if ($i ~ /^tip_moves=/) { split($i, a, "="); moves = a[2]+0 }
    if ($i ~ /^bulk_ms=/) { split($i, a, "="); bulk = a[2]+0 }
    if ($i ~ /^bulk_ovf=/) { split($i, a, "="); bovf = a[2]+0 }
    if ($i ~ /^ovf_ms=/) { split($i, a, "="); ovf = a[2]+0 }
    if ($i ~ /^reset_ms=/) { split($i, a, "="); reset = a[2]+0 }
    if ($i ~ /^churn_ms=/) { split($i, a, "="); churn = a[2]+0 }
  }
  n[lang]++; tip_s[lang]+=tip; moves_s[lang]+=moves; bulk_s[lang]+=bulk
  bovf_s[lang]+=bovf; ovf_s[lang]+=ovf; reset_s[lang]+=reset; churn_s[lang]+=churn
}
END {
  order[1]="cc"; order[2]="c"; order[3]="zig"; order[4]="go"; order[5]="malloc"
  for (k = 1; k <= 5; k++) {
    lang = order[k]
    if (!(lang in n)) next
    nn = n[lang]
    printf "%-8s %9.2f %9.0f %10.2f %10.0f %9.2f %9.2f %9.2f\n", \
      lang, tip_s[lang]/nn, moves_s[lang]/nn, bulk_s[lang]/nn, \
      bovf_s[lang]/nn, ovf_s[lang]/nn, reset_s[lang]/nn, churn_s[lang]/nn
  }
}
' "$RESULTS_RAW"

echo ""
echo "raw: $RESULTS_RAW"
echo "tip_moves~0 means in-place tip growth; malloc tip_moves counts realloc moves."
echo "bulk_ovf~0 means the happy path stayed in slabs (compare bulk_ms to malloc)."
echo "Speed scoreboard only: bulk root is sized so N fits slabs — not a memory-thesis proof."
echo "CC tip/bulk/churn use alloc_local*_grow / realloc_local*_grow (single-owner tier)."
echo "Memory / reclaim thesis → compare_mixed_lifetime + tiers/heatmap + malloc_cost."
echo "QUICK smoke: ARENA_MEM_QUICK=1 ./stress/compare_arena_memory.sh"
echo "================================================================="
