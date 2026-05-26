#!/usr/bin/env bash
# Capture M0 baseline metrics into perf/baseline_M0.txt
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/perf/baseline_M0.txt"
CCC="${CCC:-$ROOT/out/cc/bin/ccc}"

if [[ ! -x "$CCC" ]]; then
  echo "Build compiler first: make -C $ROOT/cc" >&2
  exit 1
fi

REPARSES=$(grep -c 'cc__reparse_source_to_ast' "$ROOT/cc/src/visitor/visit_codegen.c" || true)

{
  echo "# M0 baseline — $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "reparse_sites_visit_codegen=$REPARSES"
  echo "initial_parse=1"
  echo ""
  for f in \
    "real_projects/pigz/pigz_idiomatic.ccs" \
    "real_projects/redis/redis_idiomatic.ccs" \
    "tests/ufcs_in_lifted_closure_smoke.ccs" \
    "tests/comptime_channel_ufcs_smoke.ccs"; do
    if [[ -f "$ROOT/$f" ]]; then
      t=$(/usr/bin/time -p "$CCC" build "$ROOT/$f" -o /tmp/cc_baseline_out 2>&1 | awk '/real/ {print $2}')
      echo "${f} compile_seconds=$t"
    fi
  done
} > "$OUT"
echo "Wrote $OUT"
