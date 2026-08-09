#!/usr/bin/env bash
# compare_arena_memory.sh — fair bump-arena protocol across CC / C / Go / Zig.
#
# Builds peers, shuffles run order each trial (startup / cache coldness),
# averages RESULT lines.
#
# Env knobs (forwarded to all peers):
#   ARENA_MEM_TIP_ITERS ARENA_MEM_TIP_ROOT ARENA_MEM_STORM ARENA_MEM_ROOT_STORM
#   ARENA_MEM_CHURN ARENA_MEM_CHURN_EACH ARENA_MEM_BLOCK_MAX
#   ARENA_MEM_TRIALS (default 5)  ARENA_MEM_SEED (default from date+$$)
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

TRIALS="${ARENA_MEM_TRIALS:-5}"
SEED="${ARENA_MEM_SEED:-$(date +%s)-$$}"

mkdir -p "$OUT"
export ARENA_MEM_TIP_ITERS="${ARENA_MEM_TIP_ITERS:-20000}"
export ARENA_MEM_TIP_ROOT="${ARENA_MEM_TIP_ROOT:-$((4 * 1024 * 1024))}"
export ARENA_MEM_STORM="${ARENA_MEM_STORM:-50000}"
export ARENA_MEM_ROOT_STORM="${ARENA_MEM_ROOT_STORM:-4096}"
export ARENA_MEM_CHURN="${ARENA_MEM_CHURN:-200}"
export ARENA_MEM_CHURN_EACH="${ARENA_MEM_CHURN_EACH:-64}"
export ARENA_MEM_BLOCK_MAX="${ARENA_MEM_BLOCK_MAX:-4}"

echo "================================================================="
echo "ARENA MEMORY — fair bump protocol (CC / C / Go / Zig)"
echo "================================================================="
echo "trials=$TRIALS seed=$SEED"
echo "tip_iters=$ARENA_MEM_TIP_ITERS storm=$ARENA_MEM_STORM block_max=$ARENA_MEM_BLOCK_MAX"
echo "ccc=$CCC"
echo "================================================================="
echo ""

echo "Building..."
"$CCC" build --release "$SCRIPT_DIR/arena_memory_bench.ccs" -o "$OUT/arena_memory_bench_cc" >/dev/null
cc -O2 -std=c11 "$SCRIPT_DIR/c/arena_memory_bench.c" -o "$OUT/arena_memory_bench_c"
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

# Collect available runners as "name|path"
RUNNERS=()
RUNNERS+=("cc|$OUT/arena_memory_bench_cc")
RUNNERS+=("c|$OUT/arena_memory_bench_c")
[ -x "$OUT/arena_memory_bench_go" ] && RUNNERS+=("go|$OUT/arena_memory_bench_go")
[ -x "$OUT/arena_memory_bench_zig" ] && RUNNERS+=("zig|$OUT/arena_memory_bench_zig")

# Fisher–Yates shuffle using $SEED + trial index (portable awk/od).
shuffle_runners() {
  local trial=$1
  local n=${#RUNNERS[@]}
  local i j tmp
  local -a arr=("${RUNNERS[@]}")
  # Derive a deterministic stream of ints from seed+trial
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
    # Small pause so successive bins don't share warm caches identically.
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
printf "%-6s %10s %10s %10s %12s %12s %10s\n" \
  "lang" "tip_ms" "tip_moves" "storm_ms" "storm_gross" "storm_ovf" "churn_ms"
printf "%-6s %10s %10s %10s %12s %12s %10s\n" \
  "----" "------" "---------" "--------" "-----------" "---------" "--------"

awk '
/^trial=/ {
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^lang=/) { split($i, a, "="); lang = a[2] }
    if ($i ~ /^tip_ms=/) { split($i, a, "="); tip = a[2]+0 }
    if ($i ~ /^tip_moves=/) { split($i, a, "="); moves = a[2]+0 }
    if ($i ~ /^storm_ms=/) { split($i, a, "="); storm = a[2]+0 }
    if ($i ~ /^storm_gross=/) { split($i, a, "="); gross = a[2]+0 }
    if ($i ~ /^storm_ovf=/) { split($i, a, "="); ovf = a[2]+0 }
    if ($i ~ /^churn_ms=/) { split($i, a, "="); churn = a[2]+0 }
  }
  n[lang]++; tip_s[lang]+=tip; moves_s[lang]+=moves; storm_s[lang]+=storm
  gross_s[lang]+=gross; ovf_s[lang]+=ovf; churn_s[lang]+=churn
}
END {
  order[1]="cc"; order[2]="c"; order[3]="go"; order[4]="zig"
  for (k = 1; k <= 4; k++) {
    lang = order[k]
    if (!(lang in n)) next
    nn = n[lang]
    printf "%-6s %10.3f %10.1f %10.3f %12.0f %12.0f %10.3f\n", \
      lang, tip_s[lang]/nn, moves_s[lang]/nn, storm_s[lang]/nn, \
      gross_s[lang]/nn, ovf_s[lang]/nn, churn_s[lang]/nn
  }
}
' "$RESULTS_RAW"

echo ""
echo "raw: $RESULTS_RAW"
echo "Note: tip_moves should be ~0 for in-place tip growth on a large root."
echo "storm_gross/ovf are protocol accounting (not OS RSS)."
echo "================================================================="
