#!/usr/bin/env bash
# Differential additive check for the UFCS extension tiers (free-call
# symmetry, universal bare-name tier): every test that compiles with the
# tiers disabled (CC_UFCS_BASELINE=1) must lower byte-identically with
# them enabled. The tiers may only animate ill-formed code — a diff on a
# baseline-compiling test is an additivity violation. Tests that only
# compile WITH the tiers are the animated set and are skipped (counted).
set -u
cd "$(dirname "$0")/.."
S="${TMPDIR:-/tmp}/cc_additive_$$"
mkdir -p "$S/base" "$S/full" "$S/res"
run_one() {
  t="$1"; S="$2"
  stem=$(basename "$t"); stem="${stem%.*}"
  if ! CC_UFCS_BASELINE=1 CC_NO_CACHE=1 ./cc/bin/ccc build --emit-c-only "$t" -o "$S/base/$stem.c" >/dev/null 2>&1; then
    echo skip > "$S/res/$stem"; return
  fi
  if ! CC_NO_CACHE=1 ./cc/bin/ccc build --emit-c-only "$t" -o "$S/full/$stem.c" >/dev/null 2>&1; then
    echo "fail build-full" > "$S/res/$stem"; return
  fi
  if cmp -s "$S/base/$stem.c" "$S/full/$stem.c"; then
    echo pass > "$S/res/$stem"
  else
    echo "fail diff" > "$S/res/$stem"
  fi
}
export -f run_one
jobs="${CC_ADDITIVE_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
ls tests/*.ccs tests/*.shcc 2>/dev/null | grep -v "_fail\." | \
  xargs -P "$jobs" -I{} bash -c 'run_one "$@"' _ {} "$S"
pass=0; skip=0; fail=0
for r in "$S"/res/*; do
  v=$(cat "$r")
  case "$v" in
    pass) pass=$((pass+1));;
    skip) skip=$((skip+1));;
    *) fail=$((fail+1)); echo "FAIL($v): $(basename "$r")";;
  esac
done
echo "additive-diff: $pass identical, $skip animated/skipped, $fail violations"
rm -rf "$S"
[ "$fail" -eq 0 ]
