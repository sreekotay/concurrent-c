#!/usr/bin/env bash
# compare_parallel_unbound.sh — CC @parallel vs Go, fully UNBOUNDED matrix.
#
# The question this answers: what happens when the programmer writes the
# naive thing — a spawn per tree node, no grain cut, no tuning — in each
# runtime? CC's answer is the adaptive spawn gate (scheduler.c): churn
# sites are learned and inlined, so the flood never materializes. Go's
# answer is 16.7M live goroutines.
#
# Fairness contract:
#   * Same tree: depth-d binary reduction, u24(left+right), root (0,0)
#   * Both sides fully ungated: CC runs bare `@parallel { }`; Go runs
#     its grain sweep pinned to grain=depth (spawn at every level).
#     If anything this favors Go: its fork variant spawns ONE goroutine
#     per node where CC's @parallel spawns both arms.
#   * seq rows are the same computation with no spawn at all.
#   * CC defaults only — no CC_PAR_SPAWN_BACKLOG, no knobs.
#   * RSS is whole-process peak (BSD time -l), so it includes the seq
#     and cut phases too; the parallel flood dominates it on both sides.
#
# Env: CC_PAR_DEPTH (default 24), CC_PAR_SAMPLES (default 3).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

: "${CC_PAR_DEPTH:=24}"
: "${CC_PAR_SAMPLES:=3}"
export CC_PAR_DEPTH CC_PAR_SAMPLES

echo "================================================================="
echo "UNBOUNDED PARALLEL MATRIX: CC @parallel (defaults) vs Go"
echo "depth=$CC_PAR_DEPTH samples=$CC_PAR_SAMPLES"
echo "================================================================="

echo "Building CC surface @parallel..."
"$CCC" build --release "$SCRIPT_DIR/parallel_hello.ccs" -o "$OUT/parallel_hello_cc"
echo "Building Go..."
go build -o "$OUT/parallel_hello_go" "$SCRIPT_DIR/go/parallel_hello.go"
echo ""

echo "--- CC: seq + ungated par + cut (defaults, CC_PAR_BARE=1) ---"
CC_PAR_BARE=1 /usr/bin/time -l "$OUT/parallel_hello_cc" \
    >"$OUT/unbound_cc.txt" 2>"$OUT/unbound_cc.time"
grep -E '^ (seq|par|cut) ' "$OUT/unbound_cc.txt"
grep -E 'maximum resident' "$OUT/unbound_cc.time"
echo ""

echo "--- Go: seq + spawn-at-every-level (grain=depth) ---"
CC_PAR_GRAINS="$CC_PAR_DEPTH" /usr/bin/time -l "$OUT/parallel_hello_go" \
    >"$OUT/unbound_go.txt" 2>"$OUT/unbound_go.time"
grep -E '^ *(seq|'"$CC_PAR_DEPTH"') ' "$OUT/unbound_go.txt" || cat "$OUT/unbound_go.txt"
grep -E 'maximum resident' "$OUT/unbound_go.time"
echo ""

echo "================================================================="
echo "SUMMARY (medians; RSS = whole-process peak)"
echo "================================================================="
cc_seq=$(awk '/^ seq /{printf "%.1f", $3}' "$OUT/unbound_cc.txt")
cc_par=$(awk '/^ par /{printf "%.1f", $3}' "$OUT/unbound_cc.txt")
cc_rss=$(awk '/maximum resident/{printf "%.0f", $1/1048576}' "$OUT/unbound_cc.time")
go_seq=$(awk '/^  seq /{printf "%.1f", $3}' "$OUT/unbound_go.txt")
go_par=$(awk -v g="$CC_PAR_DEPTH" '$1==g{printf "%.1f", $3}' "$OUT/unbound_go.txt")
go_rss=$(awk '/maximum resident/{printf "%.0f", $1/1048576}' "$OUT/unbound_go.time")
printf "  %-14s %10s %14s %10s\n" "" "seq_ms" "par_unbound_ms" "peak_MB"
printf "  %-14s %10s %14s %10s\n" "CC (defaults)" "$cc_seq" "$cc_par" "$cc_rss"
printf "  %-14s %10s %14s %10s\n" "Go" "$go_seq" "$go_par" "$go_rss"
echo ""
echo "Interpret with the fairness contract at the top of this script."
