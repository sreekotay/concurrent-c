#!/usr/bin/env bash
# compare_mixed_lifetime.sh — mixed-lifetime strategies (equal root budget).
#
# Same free-set for all peers:
#   alloc N → drop N/2 (scrambled set) → grow survivors → reclaim rest
#
# Strategies:
#   cc        arena_release — ONE arena + mid-life release (escape hatch)
#   cc_split  arena_split   — scratch + keep arenas (lifetime annotation)
#   c         malloc
#   zig       malloc
#   go        gc
#
# Capacity honesty: MIX_LIFE_TOTAL_ROOT is shared.
#   release root = TOTAL; split scratch=keep = TOTAL/2.
#   (Same total root bytes; slab budget still × block_max per arena.)
#
# Env: MIX_LIFE_N MIX_LIFE_TOTAL_ROOT MIX_LIFE_ROOT (alias for release)
#      MIX_LIFE_SCRATCH_ROOT MIX_LIFE_KEEP_ROOT MIX_LIFE_SEED MIX_LIFE_TRIALS
#      MIX_LIFE_QUICK=1
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
PEER_SEED="${MIX_LIFE_PEER_SEED:-12788910}"

mkdir -p "$OUT"
if [ "${MIX_LIFE_QUICK:-0}" = "1" ]; then
  export MIX_LIFE_N="${MIX_LIFE_N:-50000}"
  export MIX_LIFE_TOTAL_ROOT="${MIX_LIFE_TOTAL_ROOT:-$((8 * 1024 * 1024))}"
else
  export MIX_LIFE_N="${MIX_LIFE_N:-200000}"
  # ~45MiB live at N=200k; 32MiB root × block_max=4 ≈ 128MiB slab budget.
  export MIX_LIFE_TOTAL_ROOT="${MIX_LIFE_TOTAL_ROOT:-$((32 * 1024 * 1024))}"
fi

# Equal root-byte budget: one fat arena vs two half-size arenas.
export MIX_LIFE_ROOT="${MIX_LIFE_ROOT:-$MIX_LIFE_TOTAL_ROOT}"
export MIX_LIFE_SCRATCH_ROOT="${MIX_LIFE_SCRATCH_ROOT:-$((MIX_LIFE_TOTAL_ROOT / 2))}"
export MIX_LIFE_KEEP_ROOT="${MIX_LIFE_KEEP_ROOT:-$((MIX_LIFE_TOTAL_ROOT / 2))}"
export MIX_LIFE_SEED="$PEER_SEED"
export MIX_LIFE_TOTAL_ROOT

echo "================================================================="
echo "MIXED LIFETIME — equal root budget (CC / C / Go / Zig)"
echo "================================================================="
echo "trials=$TRIALS harness_seed=$SEED peer_seed=$PEER_SEED n=$MIX_LIFE_N"
echo "total_root=$MIX_LIFE_TOTAL_ROOT  release_root=$MIX_LIFE_ROOT"
echo "split scratch_root=$MIX_LIFE_SCRATCH_ROOT keep_root=$MIX_LIFE_KEEP_ROOT"
echo "  (equal total root bytes; each arena still grows up to block_max slabs)"
echo ""
echo "Workload: alloc N → drop N/2 (same free set) → grow survivors → reclaim"
echo "  cc       = one arena + per-object release (escape)"
echo "  cc_split = scratch + keep reset (intended annotation)"
echo "  c/zig    = malloc/free     go = GC"
echo ""
echo "Timing columns are comparable. rss_peak_d only for process RSS samplers"
echo "  (cc/c via mem_sample). Go Sys / Zig -1 are NOT averaged into that column."
echo "  Reclaim correctness → arena peak_gross/peak_ovf (CC RESULT fields)."
echo "================================================================="
echo ""

echo "Building..."
cc -O2 -std=c11 -c "$SCRIPT_DIR/mem_sample.c" -o "$OUT/mem_sample.o"
"$CCC" build --release "$SCRIPT_DIR/arena_mixed_lifetime_bench.ccs" \
  -o "$OUT/mixed_lifetime_cc" --ld-flags "$OUT/mem_sample.o" >/dev/null
"$CCC" build --release "$SCRIPT_DIR/arena_mixed_lifetime_split_bench.ccs" \
  -o "$OUT/mixed_lifetime_cc_split" --ld-flags "$OUT/mem_sample.o" >/dev/null
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
RUNNERS+=("cc_split|$OUT/mixed_lifetime_cc_split")
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
echo "AVERAGES (n=$TRIALS)  equal total_root=$MIX_LIFE_TOTAL_ROOT"
echo "================================================================="
printf "%-6s %-14s %9s %10s %9s %10s %9s %12s %12s\n" \
  "lang" "strategy" "alloc_ms" "midfree_ms" "grow_ms" "reclaim_ms" "total_ms" "rss_peak_d" "peak_gross"
printf "%-6s %-14s %9s %10s %9s %10s %9s %12s %12s\n" \
  "----" "--------" "--------" "----------" "-------" "----------" "--------" "----------" "----------"

awk '
/^trial=/ {
  lang=""; strat=""; al=0; mf=0; gr=0; rc=0; tot=0; rp=0
  pg=-1; sg=-1; kg=-1; rss_ok=0
  for (i = 1; i <= NF; i++) {
    if ($i ~ /^lang=/) { split($i, a, "="); lang = a[2] }
    if ($i ~ /^strategy=/) { split($i, a, "="); strat = a[2] }
    if ($i ~ /^alloc_ms=/) { split($i, a, "="); al = a[2]+0 }
    if ($i ~ /^midfree_ms=/) { split($i, a, "="); mf = a[2]+0 }
    if ($i ~ /^grow_ms=/) { split($i, a, "="); gr = a[2]+0 }
    if ($i ~ /^reclaim_ms=/) { split($i, a, "="); rc = a[2]+0 }
    if ($i ~ /^total_ms=/) { split($i, a, "="); tot = a[2]+0 }
    if ($i ~ /^rss_peak_delta=/) { split($i, a, "="); rp = a[2]+0 }
    if ($i ~ /^peak_gross=/) { split($i, a, "="); pg = a[2]+0 }
    if ($i ~ /^scratch_gross=/) { split($i, a, "="); sg = a[2]+0 }
    if ($i ~ /^keep_gross=/) { split($i, a, "="); kg = a[2]+0 }
  }
  if (strat == "arena_split" && sg >= 0 && kg >= 0) pg = sg + kg
  # mem_sample process RSS only (cc/c). Go Sys / Zig -1 are different currency.
  if ((lang == "cc" || lang == "c") && rp >= 0) rss_ok = 1
  key = lang "|" strat
  n[key]++; al_s[key]+=al; mf_s[key]+=mf; gr_s[key]+=gr; rc_s[key]+=rc; tot_s[key]+=tot
  if (rss_ok) { rp_s[key]+=rp; rp_n[key]++ }
  if (pg >= 0) { pg_s[key]+=pg; pg_n[key]++ }
  if (!(key in ord)) { ord[key]=++nord; keys[nord]=key }
}
END {
  for (i = 1; i <= nord; i++) {
    key = keys[i]
    nn = n[key]
    split(key, p, "|")
    rss_str = "-"
    if (rp_n[key] > 0) rss_str = sprintf("%.0f", rp_s[key]/rp_n[key])
    pg_str = "-"
    if (pg_n[key] > 0) pg_str = sprintf("%.0f", pg_s[key]/pg_n[key])
    printf "%-6s %-14s %9.2f %10.2f %9.2f %10.2f %9.2f %12s %12s\n", \
      p[1], p[2], al_s[key]/nn, mf_s[key]/nn, gr_s[key]/nn, rc_s[key]/nn, tot_s[key]/nn, rss_str, pg_str
  }
}
' "$RAW"

echo ""
echo "raw: $RAW"
echo "Read:"
echo "  • Timing: comparable across langs."
echo "  • arena_split midfree ≈ scratch.reset; arena_release = per-object (ovf frees now)."
echo "  • peak_gross: CC accounting only (release one arena; split = scratch+keep)."
echo "  • rss_peak_d: mem_sample only (cc/c). Go/Zig omitted (different currency)."
echo "  • Happy-path tip/bulk speed → compare_arena_memory.sh (sized to fit slabs)."
echo "QUICK: MIX_LIFE_QUICK=1 ./stress/compare_mixed_lifetime.sh"
echo "================================================================="
