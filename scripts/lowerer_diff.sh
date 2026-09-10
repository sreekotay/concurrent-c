#!/usr/bin/env bash
# Differential run of cc_test on the shadow lowerer and the clean lowerer.
#   ./scripts/lowerer_diff.sh [--filter SUBSTR] [cc_test args...]
# Writes out/lowerer_diff/{shadow,clean}.log and the per-test table
# out/lowerer_diff/table.txt (test  shadow  clean), then prints the 2x2
# summary. A test passing on shadow and failing on clean is the work list.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT=out/lowerer_diff
mkdir -p "$OUT"
[ -x out/tools/cc_test ] || { echo "error: out/tools/cc_test missing (cc -O2 tools/cc_test.c -o out/tools/cc_test)" >&2; exit 2; }
[ -x out/cc/bin/cclower_cc ] || { echo "error: out/cc/bin/cclower_cc missing (make -C cc lower-cc)" >&2; exit 2; }
run() {  # name env-assignment
  local name=$1; shift
  env "$@" out/tools/cc_test --quick "${ARGS[@]}" > "$OUT/$name.log" 2>&1 || true
  # one line per test: ok, fail, xfail or timeout. The harness prints a
  # row's verdict lines in order -- `[FAIL] s: ...` then `[XFAIL] s (...)`,
  # `[OK] s` then `[XPASS] s: ...` -- so the last line for a stem is its
  # verdict. The stem must be followed by end of line, `:` or ` (`: a
  # `[FAIL] expected stdout to contain: ...` line names no row.
  sed -nE 's/^\[(OK|FAIL|XFAIL|XPASS|TIMEOUT)\] ([A-Za-z0-9_./-]+)($|:.*| \(.*)/\2 \1/p' "$OUT/$name.log" \
    | awk '{ st = ($2 == "OK") ? "ok" : ($2 == "XFAIL") ? "xfail" : ($2 == "TIMEOUT") ? "timeout" : "fail"; r[$1] = st }
           END { for (t in r) print t, r[t] }' | sort > "$OUT/$name.tsv"
}
ARGS=("$@")
run shadow CC_LOWERER=shadow
run clean CC_LOWERER=clean
join -a1 -a2 -e missing -o 0,1.2,2.2 "$OUT/shadow.tsv" "$OUT/clean.tsv" > "$OUT/table.txt"
# The 2x2 is over rows neither lowerer marks expected-to-fail; a marked row
# is counted on its own line, and a hang is a failure that says so.
pp=$(awk '$2=="ok" && $3=="ok"' "$OUT/table.txt" | wc -l)
pf=$(awk '$2=="ok" && ($3=="fail" || $3=="timeout" || $3=="missing")' "$OUT/table.txt" | wc -l)
fp=$(awk '($2=="fail" || $2=="timeout" || $2=="missing") && $3=="ok"' "$OUT/table.txt" | wc -l)
ff=$(awk '($2=="fail" || $2=="timeout" || $2=="missing") && ($3=="fail" || $3=="timeout" || $3=="missing")' "$OUT/table.txt" | wc -l)
xs=$(awk '$2=="xfail"' "$OUT/table.txt" | wc -l)
xc=$(awk '$3=="xfail"' "$OUT/table.txt" | wc -l)
echo "shadow/clean  pass/pass=$pp  pass/fail=$pf  fail/pass=$fp  fail/fail=$ff  xfail: shadow=$xs clean=$xc  (table: $OUT/table.txt)"
awk '$2=="ok" && $3!="ok" && $3!="xfail" { print "  clean fails:", $1, "(" $3 ")" }' "$OUT/table.txt" | head -40
