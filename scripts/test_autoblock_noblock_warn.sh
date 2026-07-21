#!/bin/sh
# "Definition wins TU-locally" (@noblock) — lying-decl warning oracle.
#
# The cc_test harness only checks RUN stdout/stderr for passing tests;
# compile-time warnings land on BUILD stderr, which it never asserts on.
# This script pins the warning contract directly:
#
#   1. decl promises @noblock, def breaks it  -> exactly ONE warning,
#      carrying file:line of BOTH the decl and the def (lines computed
#      from the fixture, so the test survives fixture edits).
#   2. @noblock on the def only (unannotated fwd decl) -> NO warning.
#   3. @noblock on decl AND def                        -> NO warning.
#
# The behavioral side (wrapped vs inline) is asserted at runtime by the
# corresponding tests/autoblock_noblock_*_smoke.ccs tid oracles.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_autoblock_noblock_warn] FAIL: $1" >&2; exit 1; }

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

WARN_RE='warning: async: declaration of .* promises @noblock but its definition at .* does not carry it'

# --- 1. decl promises, def breaks: warning with both coordinates ---
fixture=tests/autoblock_noblock_decl_promises_def_breaks_smoke.ccs
decl_line=$(grep -n '@noblock static void record_tid' "$fixture" | head -1 | cut -d: -f1)
def_line=$(grep -n '^static void record_tid' "$fixture" | head -1 | cut -d: -f1)
[ -n "$decl_line" ] && [ -n "$def_line" ] || fail "fixture drifted: cannot locate decl/def lines in $fixture"

"$CCC" --no-cache --emit-c-only "$fixture" >/dev/null 2>"$tmp_dir/dp.err" \
  || fail "decl-promises fixture failed to compile: $(cat "$tmp_dir/dp.err")"

grep -Eq "$WARN_RE" "$tmp_dir/dp.err" \
  || fail "lying-decl warning missing from build stderr: $(cat "$tmp_dir/dp.err")"
warn_count=$(grep -Ec "$WARN_RE" "$tmp_dir/dp.err")
[ "$warn_count" -eq 1 ] \
  || fail "lying-decl warning fired $warn_count times (dedupe broken; want exactly 1)"
grep -Eq "declaration of 'record_tid' at [^ ]*${fixture##*/}:${decl_line} promises @noblock" "$tmp_dir/dp.err" \
  || fail "warning lacks the DECL coordinate (${fixture##*/}:${decl_line}): $(grep -E "$WARN_RE" "$tmp_dir/dp.err")"
grep -Eq "definition at [^ ]*${fixture##*/}:${def_line} does not carry it" "$tmp_dir/dp.err" \
  || fail "warning lacks the DEF coordinate (${fixture##*/}:${def_line}): $(grep -E "$WARN_RE" "$tmp_dir/dp.err")"

# --- 2/3. honest configurations must be warning-free ---
for f in tests/autoblock_noblock_def_wins_smoke.ccs tests/autoblock_noblock_decl_and_def_smoke.ccs; do
  "$CCC" --no-cache --emit-c-only "$f" >/dev/null 2>"$tmp_dir/ok.err" \
    || fail "$f failed to compile: $(cat "$tmp_dir/ok.err")"
  if grep -Eq 'promises @noblock' "$tmp_dir/ok.err"; then
    fail "spurious lying-decl warning for $f: $(grep -E 'promises @noblock' "$tmp_dir/ok.err")"
  fi
done

echo "[test_autoblock_noblock_warn] OK"
