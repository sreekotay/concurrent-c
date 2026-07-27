#!/usr/bin/env bash
# Capture compiler perf baseline into perf/compiler_baseline.txt.
#
# Metrics (all from a clean rebuild + a full smoke run):
#   - Static count of cc__reparse_source_to_ast call sites in visit_codegen.c
#   - LOC of key visitor files (visit_codegen.c, closure-literal pass, total visitor dir)
#   - Dynamic per-stage reparse counts across the 461-test smoke suite
#   - Wall-clock for the warm smoke suite (jobs=8 by default)
#
# Use:
#   ./scripts/capture_baseline.sh                   # write perf/compiler_baseline.txt
#   ./scripts/capture_baseline.sh /tmp/snapshot.txt # write somewhere else
#
# See tools/cc_perf_check.shcc for the regression check that consumes this
# file (bash oracle: tools/cc_perf_check.sh).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/perf/compiler_baseline.txt}"
CCC="$ROOT/cc/bin/ccc"
JOBS="${PERF_JOBS:-8}"

echo "[capture] ensuring compiler + cc_test are built..."
make -C "$ROOT" -j8 cc >/dev/null
make -C "$ROOT" tools >/dev/null

if [[ ! -x "$CCC" ]]; then
  echo "[capture] cc/bin/ccc not built, abort" >&2
  exit 1
fi

echo "[capture] measuring static metrics..."
# Count visit_codegen.c reparse call sites. The reparse machinery is accessed
# through two wrappers (`cc__reparse_source_to_ast_ctx` / `_ex`) — count both,
# then subtract 1 for the internal `_ctx -> _ex` hop in the wrapper itself.
ctx_calls=$(grep -c 'cc__reparse_source_to_ast_ctx(' "$ROOT/cc/src/visitor/visit_codegen.c" || true)
ex_calls=$(grep -c 'cc__reparse_source_to_ast_ex(' "$ROOT/cc/src/visitor/visit_codegen.c" || true)
# Forward-decl + definition each appear once with `_ex(` followed by `\n`; the
# internal `_ctx -> _ex` hop appears once. So subtract 1 (the internal hop).
# (Forward-decl + def are caught by the `(` match; we count only true call
#  sites, of which the internal hop is one we want to exclude.)
reparse_sites=$(( ctx_calls + ex_calls - 1 ))
loc_visit_codegen=$(wc -l < "$ROOT/cc/src/visitor/visit_codegen.c" | tr -d ' ')
loc_closure_pass=$(wc -l < "$ROOT/cc/src/visitor/pass_closure_literal_ast.c" | tr -d ' ')
loc_visitor_dir=$(find "$ROOT/cc/src/visitor" -name '*.c' -type f -exec cat {} + | wc -l | tr -d ' ')

echo "[capture] clearing scratch and running smoke suite with CC_DEBUG_REPARSE=1 (jobs=$JOBS)..."
rm -rf "$ROOT/out/.cc_test" "$ROOT/bin/.cc_test"
# Run twice: discard first (cold) to get a warm wall-time number that matches
# day-to-day developer workflow (incremental builds, cached preprocessor state).
# Tolerate noisier first run for stage counts (still deterministic per-TU).
CC_DEBUG_REPARSE=1 "$ROOT/tools/cc_test" --jobs "$JOBS" >/dev/null 2>&1 || {
  echo "[capture] FATAL: smoke suite failed; refusing to capture a broken baseline" >&2
  exit 1
}

# Now do a warm timed run. /usr/bin/time -p writes "real X.XX..." to its own
# stderr; cc_test ALSO writes its progress + "cc_test: N passed" to stderr.
# Combine the two streams into one file and parse out both lines.
echo "[capture] timing warm suite..."
( /usr/bin/time -p "$ROOT/tools/cc_test" --jobs "$JOBS" ) >/tmp/cc_baseline.combined 2>&1 || true
wall_real=$(awk '/^real/ {print $2}' /tmp/cc_baseline.combined)
suite_pass=$(awk '/^cc_test:/ {print $2}' /tmp/cc_baseline.combined)

echo "[capture] aggregating per-stage reparse counts from $ROOT/out/.cc_test/**/*.build.stderr..."
# Group by the first space-delimited token after `stage=` to get a stable
# categorical name (`phase3`, `statement-lowering`, `async-lowering`,
# `final-UFCS`). Comments inside source explain why splits stay categorical.
reparse_data=$(find "$ROOT/out/.cc_test" -name '*.build.stderr' -exec grep -hE '^\[cc:reparse\]' {} + 2>/dev/null \
  | awk -F'stage=' '{split($2,a," "); print a[1]}' \
  | sort | uniq -c | awk '{print $2"="$1}')

# Extract individual stage counts (default to 0 if a stage is absent post-fix).
get_stage() { echo "$reparse_data" | awk -F'=' -v k="$1" '$1==k {print $2; found=1} END {if (!found) print 0}'; }
r_phase3=$(get_stage phase3)
r_stmtlower=$(get_stage statement-lowering)
r_asynclower=$(get_stage async-lowering)
r_finalufcs=$(get_stage final-UFCS)
r_total=$(echo "$reparse_data" | awk -F'=' 'BEGIN{s=0} {s+=$2} END{print s}')

echo "[capture] writing $OUT"
{
  echo "# Compiler perf baseline"
  echo "# Captured: $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "# Host: $(uname -srm)"
  echo "# Suite: tools/cc_test (jobs=$JOBS)"
  echo "# Read perf/README.md \"Compiler perf baseline\" section for what each metric guards against."
  echo ""
  echo "# Static metrics (visitor source code shape)"
  echo "reparse_sites_visit_codegen=$reparse_sites"
  echo "loc_visit_codegen=$loc_visit_codegen"
  echo "loc_closure_pass=$loc_closure_pass"
  echo "loc_visitor_dir=$loc_visitor_dir"
  echo ""
  echo "# Dynamic metrics (per-stage reparse counts across $suite_pass tests)"
  echo "suite_tests=$suite_pass"
  echo "reparses_phase3=$r_phase3"
  echo "reparses_statement_lowering=$r_stmtlower"
  echo "reparses_async_lowering=$r_asynclower"
  echo "reparses_final_ufcs=$r_finalufcs"
  echo "reparses_total=$r_total"
  echo ""
  echo "# Wall-clock (warm run, with build cache + scratch dir reused)"
  echo "wall_real_seconds=$wall_real"
} > "$OUT"

echo "[capture] done."
echo
cat "$OUT"
