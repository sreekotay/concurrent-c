#!/usr/bin/env bash
# Bench flat / epoch / wheel CC backends vs Rust at a given HOSTILE_SCALE.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
RUN="$ROOT/studies/quickjs_lifetime/scripts/run_track_a.sh"
SCALE="${HOSTILE_SCALE:-x100}"
export HOSTILE_SCALE="$SCALE"
export CC_QUICKJS_SRC="${CC_QUICKJS_SRC:-/tmp/quickjs-ng}"

SUMMARY="$ROOT/studies/quickjs_lifetime/track_a/results/$(date +%Y%m%d)_${SCALE}_backends.txt"
: >"$SUMMARY"
{
  echo "Track A backend compare  scale=$SCALE  src=$CC_QUICKJS_SRC"
  echo
} | tee -a "$SUMMARY"

for impl in flat epoch wheel; do
  echo "==== HOSTILE_IMPL=$impl ====" | tee -a "$SUMMARY"
  HOSTILE_IMPL="$impl" "$RUN" | tee -a /tmp/track_a_${SCALE}_${impl}.log | tail -8
  cc_receipt="$ROOT/studies/quickjs_lifetime/track_a/results/$(date +%Y%m%d)_${SCALE}_${impl}_cc.txt"
  {
    echo "--- $impl CC ---"
    grep -E '^(PERF|hostile_impl|oracle|--- RSS)' "$cc_receipt" || cat "$cc_receipt"
    echo
  } | tee -a "$SUMMARY"
done

echo "Wrote $SUMMARY"
