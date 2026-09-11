#!/usr/bin/env bash
# compare_parallel_trace.sh — hello storm with a Whitted leaf.
#
# Same three rows as parallel_hello.ccs (seq / ungated par / cut) on a
# quadtree of pixels. No Go twin: the question is whether a real leaf
# flips the adaptive gate off CHURN.
#
#   CC_PAR_DEPTH    default 9 here (512²); the .ccs default is 8
#   CC_PAR_CUT      default 3
#   CC_PAR_SAMPLES  default 3
#   CC_PAR_BARE     default 1
#   CC_PAR_BOUNCES  default 4
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
mkdir -p "$OUT"

: "${CC_PAR_DEPTH:=9}"
: "${CC_PAR_CUT:=3}"
: "${CC_PAR_SAMPLES:=3}"
: "${CC_PAR_BARE:=1}"
: "${CC_PAR_BOUNCES:=4}"
export CC_PAR_DEPTH CC_PAR_CUT CC_PAR_SAMPLES CC_PAR_BARE CC_PAR_BOUNCES

echo "================================================================="
echo "PARALLEL TRACE: hello storm, Whitted leaf"
echo "depth=$CC_PAR_DEPTH cut=$CC_PAR_CUT samples=$CC_PAR_SAMPLES bounces=$CC_PAR_BOUNCES"
echo "================================================================="

echo "Building CC surface @parallel..."
"$CCC" build --release "$SCRIPT_DIR/parallel_trace.ccs" -o "$OUT/parallel_trace_cc"
echo ""

echo "--- cc @parallel (seq / par / cut) ---"
/usr/bin/time -l "$OUT/parallel_trace_cc" \
    >"$OUT/parallel_trace_cc.txt" 2>"$OUT/parallel_trace_cc.time"
grep -E '^ (seq|par|cut) ' "$OUT/parallel_trace_cc.txt"
grep -E 'maximum resident' "$OUT/parallel_trace_cc.time"
echo ""

echo "================================================================="
echo "SUMMARY (medians)"
echo "================================================================="
cat "$OUT/parallel_trace_cc.txt"
echo ""
echo "Interpret with the header in perf/parallel_trace.ccs."
