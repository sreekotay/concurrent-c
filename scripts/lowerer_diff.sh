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
  # one line per test: OK or FAIL (XFAIL counts as ok for the diff)
  grep -oE '^\[(OK|FAIL|XFAIL|XPASS)\] [A-Za-z0-9_./-]+' "$OUT/$name.log" \
    | sed -E 's/^\[(OK|XFAIL)\] /ok /; s/^\[(FAIL|XPASS)\] /fail /' \
    | awk '{ print $2, $1 }' | sort -u > "$OUT/$name.tsv"
}
ARGS=("$@")
run shadow CC_LOWERER=shadow
run clean CC_LOWERER=clean
join -a1 -a2 -e missing -o 0,1.2,2.2 "$OUT/shadow.tsv" "$OUT/clean.tsv" > "$OUT/table.txt"
pp=$(awk '$2=="ok" && $3=="ok"' "$OUT/table.txt" | wc -l)
pf=$(awk '$2=="ok" && $3!="ok"' "$OUT/table.txt" | wc -l)
fp=$(awk '$2!="ok" && $3=="ok"' "$OUT/table.txt" | wc -l)
ff=$(awk '$2!="ok" && $3!="ok"' "$OUT/table.txt" | wc -l)
echo "shadow/clean  pass/pass=$pp  pass/fail=$pf  fail/pass=$fp  fail/fail=$ff  (table: $OUT/table.txt)"
awk '$2=="ok" && $3!="ok" { print "  clean fails:", $1 }' "$OUT/table.txt" | head -40
