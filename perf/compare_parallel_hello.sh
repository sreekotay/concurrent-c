#!/usr/bin/env bash
# compare_parallel_hello.sh — CC @parallel vs hand-lowered fork vs Go
#
# Fairness contract:
#   * Same tree: depth-d binary reduction, u24(left+right), root (0,0)
#   * Same env: CC_PAR_DEPTH (default 24 here) / CC_PAR_SAMPLES (5)
#   * Sequential row is the computation with no spawn
#   * Parallel rows run the join on a worker (CC fiber / Go goroutine)
#     so the timed path is park_fiber / goro switch, not a thread park
#   * Rows:
#       CC @parallel par — skipped here (grain-sweep focus; for the
#                          ungated head-to-head see
#                          compare_parallel_unbound.sh. CC_PAR_BARE=1
#                          to include it.)
#       CC @parallel cut — @parallel (d < CC_PAR_CUT); same arms, serial below cut.
#       CC lowered       — explicit spawn-while-d<grain sweep
#       Go               — same explicit grain sweep (go + WaitGroup)
#   * Compare `cut` to the lowered/Go row at grain=CC_PAR_CUT.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

: "${CC_PAR_DEPTH:=24}"
: "${CC_PAR_SAMPLES:=5}"
: "${CC_PAR_CUT:=8}"
: "${CC_PAR_BARE:=0}"
export CC_PAR_DEPTH CC_PAR_SAMPLES CC_PAR_CUT CC_PAR_BARE

echo "================================================================="
echo "PARALLEL TREE: @parallel vs lowered fork vs Go"
echo "================================================================="
echo "depth=$CC_PAR_DEPTH cut=$CC_PAR_CUT samples=$CC_PAR_SAMPLES"
echo ""

echo "Building Concurrent-C surface @parallel..."
"$CCC" build --release "$SCRIPT_DIR/parallel_hello.ccs" -o "$OUT/parallel_hello_cc"

echo "Building Concurrent-C hand-lowered grain sweep..."
"$CCC" build --release "$SCRIPT_DIR/parallel_hello_lowered.ccs" \
    -o "$OUT/parallel_hello_lowered_cc"

echo "Building Go..."
go build -o "$OUT/parallel_hello_go" "$SCRIPT_DIR/go/parallel_hello.go"
echo ""

echo "--- cc @parallel (spawn) ---"
"$OUT/parallel_hello_cc" | tee "$OUT/parallel_hello_cc.txt"
echo ""

echo "--- cc lowered (explicit grain) ---"
"$OUT/parallel_hello_lowered_cc" | tee "$OUT/parallel_hello_lowered.txt"
echo ""

echo "--- go (explicit grain) ---"
"$OUT/parallel_hello_go" | tee "$OUT/parallel_hello_go.txt"
echo ""

echo "================================================================="
echo "SUMMARY"
echo "================================================================="
echo "Surface @parallel:"
grep -E '^  (seq|par|cut) ' "$OUT/parallel_hello_cc.txt" || true
echo ""
echo "Grain sweep (spawn while d < grain):"
printf "  %-7s %8s %10s %8s %10s %8s\n" \
    "grain" "spawns" "cc_ms" "cc_vs" "go_ms" "go_vs"
# lowered / go data rows: "      0        0      31.30    0.89x"
paste <(awk '/^ *[0-9]/{print $1,$2,$3,$4}' "$OUT/parallel_hello_lowered.txt") \
      <(awk '/^ *[0-9]/{print $3,$4}' "$OUT/parallel_hello_go.txt") \
    | awk '{printf "  %-7s %8s %10s %8s %10s %8s\n", $1,$2,$3,$4,$5,$6}'
echo ""
echo "Interpret with the fairness contract at the top of this script."
