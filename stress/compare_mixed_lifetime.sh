#!/usr/bin/env bash
# compare_mixed_lifetime.sh — idiomatic mixed-lifetime strategies.
#
# Same workload for all peers:
#   alloc N → scramble-free N/2 → grow survivors → reclaim rest
#
# Strategies (not the same allocator protocol):
#   cc   arena_release  — cc_arena_heap + mid-life release + reset
#   c    malloc         — malloc/free/realloc/free
#   zig  malloc         — c_allocator same shape
#   go   gc             — drop refs + runtime.GC
#
# Env: MIX_LIFE_N MIX_LIFE_ROOT MIX_LIFE_SEED MIX_LIFE_TRIALS MIX_LIFE_QUICK=1
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

TRIALS="${MIX_LIFE_TRIALS:-3}"
SEED="${MIX_LIFE_SEED:-$(date +%s)-$$}"
# Numeric seed for peers (stable across langs within a trial via harness).
PEER_SEED="${MIX_LIFE_PEER_SEED:-12788910}"

mkdir -p "$OUT"
if [ "${MIX_LIFE_QUICK:-0}" = "1" ]; then
  export MIX_LIFE_N="${MIX_LIFE_N:-50000}"
else
  export MIX_LIFE_N="${MIX_LIFE_N:-200000}"
fi
export MIX_LIFE_ROOT="${MIX_LIFE_ROOT:-$((4 * 1024 * 1024))}"
export MIX_LIFE_SEED="$PEER_SEED"

echo "================================================================="
echo "MIXED LIFETIME — idiomatic strategies (CC / C / Go / Zig)"
echo "================================================================="
echo "trials=$TRIALS harness_seed=$SEED peer_seed=$PEER_SEED n=$MIX_LIFE_N root=$MIX_LIFE_ROOT"
echo ""
echo "Workload: alloc N → free N/2 (scrambled) → grow survivors → reclaim"
echo "  cc  = arena + release + reset     c/zig = malloc/free      go = GC"
echo ""
echo "Memory signals: reclaim correctness → arena gross/ovf (CC stress)."
echo "  Peak color → rss_peak_delta; on Linux also VmHWM + cgroup memory.peak."
echo "  macOS end RSS often lags free — do not treat residual as leak proof."
echo "  Linux scoreboard: docker/podman run (cgroup peak) or bare metal VmHWM."
echo "================================================================="
echo ""

echo "Building..."
cc -O2 -std=c11 -c "$SCRIPT_DIR/mem_sample.c" -o "$OUT/mem_sample.o"
"$CCC" build --release "$SCRIPT_DIR/arena_mixed_lifetime_bench.ccs" \
  -o "$OUT/mixed_lifetime_cc" --ld-flags "$OUT/mem_sample.o" >/dev/null
cc -O2 -std=c11 "$SCRIPT_DIR/c/mixed_lifetime_bench.c" "$SCRIPT_DIR/mem_sample.c" \
  -o "$OUT/mixed_lifetime_c"
if command -v go >/dev/null 2>&1; then
  go build -o "$OUT/mixed_lifetime_go" "$SCRIPT_DIR/go/mixed_lifetime_bench.go"
fi
if command -v zig >/dev/null 2>&1; then
  zig build-exe "$SCRIPT_DIR/zig/mixed_lifetime_bench.zig" -O ReleaseFast -lc \
    -femit-bin="$OUT/mixed_lifetime_zig" >/dev/null
fi
echo "Done."
echo ""

RUNNERS=()
RUNNERS+=("cc|$OUT/mixed_lifetime_cc")
RUNNERS+=("c|$OUT/mixed_lifetime_c")
[ -x "$OUT/mixed_lifetime_go" ] && RUNNERS+=("go|$OUT/mixed_lifetime_go")
[ -x "$OUT/mixed_lifetime_zig" ] && RUNNERS+=("zig|$OUT/mixed_lifetime_zig")

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

RAW="$OUT/mixed_lifetime_results.raw"
: > "$RAW"

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
      echo "error: $name failed" >&2
      exit 1
    fi
    echo "$out" | sed 's/^/    /'
    line=$(echo "$out" | grep '^RESULT ' || true)
    [ -n "$line" ] || { echo "error: no RESULT" >&2; exit 1; }
    echo "trial=$t order=${order_names[*]} $line" >> "$RAW"
  done < <(shuffle_runners "$t")
  echo "  order: ${order_names[*]}"
  echo ""
done

echo "================================================================="
echo "AVERAGES (n=$TRIALS)"
echo "================================================================="
printf "%-6s %-14s %9s %10s %9s %10s %9s %12s\n" \
  "lang" "strategy" "alloc_ms" "midfree_ms" "grow_ms" "reclaim_ms" "total_ms" "rss_peak_d"
printf "%-6s %-14s %9s %10s %9s %10s %9s %12s\n" \
  "----" "--------" "--------" "----------" "-------" "----------" "--------" "----------"

awk '
/^trial=/ {
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^lang=/) { split($i, a, "="); lang = a[2] }
    if ($i ~ /^strategy=/) { split($i, a, "="); strat = a[2] }
    if ($i ~ /^alloc_ms=/) { split($i, a, "="); al = a[2]+0 }
    if ($i ~ /^midfree_ms=/) { split($i, a, "="); mf = a[2]+0 }
    if ($i ~ /^grow_ms=/) { split($i, a, "="); gr = a[2]+0 }
    if ($i ~ /^reclaim_ms=/) { split($i, a, "="); rc = a[2]+0 }
    if ($i ~ /^total_ms=/) { split($i, a, "="); tot = a[2]+0 }
    if ($i ~ /^rss_peak_delta=/) { split($i, a, "="); rp = a[2]+0 }
  }
  key = lang "|" strat
  n[key]++; al_s[key]+=al; mf_s[key]+=mf; gr_s[key]+=gr; rc_s[key]+=rc; tot_s[key]+=tot; rp_s[key]+=rp
  if (!(key in ord)) { ord[key]=++nord; keys[nord]=key }
}
END {
  for (i = 1; i <= nord; i++) {
    key = keys[i]
    nn = n[key]
    split(key, p, "|")
    printf "%-6s %-14s %9.2f %10.2f %9.2f %10.2f %9.2f %12.0f\n", \
      p[1], p[2], al_s[key]/nn, mf_s[key]/nn, gr_s[key]/nn, rc_s[key]/nn, tot_s[key]/nn, rp_s[key]/nn
  }
}
' "$RAW"

echo ""
echo "raw: $RAW"
echo "Read: midfree+reclaim is where strategies diverge; alloc/grow are setup."
echo "      rss_peak_delta uses sampled peak; Linux RESULT also has hwm= / cgroup_peak=."
echo "QUICK: MIX_LIFE_QUICK=1 ./stress/compare_mixed_lifetime.sh"
echo "Linux container example:"
echo "  docker run --rm -v \"\$PWD\":/src -w /src gcc:14 bash -lc './stress/compare_mixed_lifetime.sh'"
echo "================================================================="
