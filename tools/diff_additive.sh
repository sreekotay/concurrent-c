#!/usr/bin/env bash
# Differential additive check for the UFCS extension tiers (free-call
# symmetry, universal bare-name tier): every test that compiles with the
# tiers disabled (CC_UFCS_BASELINE=1) must lower byte-identically with
# them enabled. The tiers may only animate ill-formed code — a diff on a
# baseline-compiling test is an additivity violation. Tests that only
# compile WITH the tiers are the animated set and are skipped (counted).
#
# Every bucket is counted and named. A build that fails outright says nothing
# about additivity, so folding it into the violation count makes a broken
# toolchain — a full disk, a half-built binary — read as a regression in the
# tiers, which is an expensive thing to believe. Equally, a test that builds in
# neither configuration is not an animated test, and counting it as one hides
# it from the coverage number.
set -u
cd "$(dirname "$0")/.."
S="${TMPDIR:-/tmp}/cc_additive_$$"
mkdir -p "$S/base" "$S/full" "$S/res"
run_one() {
  t="$1"; S="$2"
  stem=$(basename "$t"); stem="${stem%.*}"

  # A `.env` sidecar carries knobs the test needs to build at all; without
  # them it fails here for reasons that have nothing to do with the tiers.
  e=()
  if [ -f "tests/$stem.env" ]; then
    while IFS= read -r line; do
      case "$line" in ''|\#*) continue;; esac
      e+=("$line")
    done < "tests/$stem.env"
  fi
  set -- ${e[@]+"${e[@]}"}

  if env "$@" CC_UFCS_BASELINE=1 CC_NO_CACHE=1 ./cc/bin/ccc build --emit-c-only "$t" -o "$S/base/$stem.c" >/dev/null 2>&1; then
    base_ok=1
  else
    base_ok=0
  fi
  if env "$@" CC_NO_CACHE=1 ./cc/bin/ccc build --emit-c-only "$t" -o "$S/full/$stem.c" >/dev/null 2>&1; then
    full_ok=1
  else
    full_ok=0
  fi
  # Baseline failing is what marks a test as animated — but only when the
  # tiers then compile it. Both failing means neither configuration builds,
  # which is a broken build, not an animated test.
  if [ "$base_ok" = 0 ]; then
    if [ "$full_ok" = 1 ]; then echo skip > "$S/res/$stem"
    # A `.compile_err` sidecar declares a test the compiler must reject, so
    # both configurations failing is the expected result rather than an alarm.
    # It is checked here and not up front: `--emit-c-only` stops before the
    # host C compile, so a test whose rejection comes later still emits C and
    # is worth diffing.
    elif [ -f "tests/$stem.compile_err" ]; then echo negative > "$S/res/$stem"
    else echo "error neither-config-builds" > "$S/res/$stem"; fi
    return
  fi
  if [ "$full_ok" = 0 ]; then
    echo "error build-full" > "$S/res/$stem"; return
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
pass=0; skip=0; neg=0; fail=0; err=0
for r in "$S"/res/*; do
  v=$(cat "$r")
  case "$v" in
    pass) pass=$((pass+1));;
    skip) skip=$((skip+1));;
    negative) neg=$((neg+1));;
    error*) err=$((err+1)); echo "ERROR($v): $(basename "$r")";;
    *) fail=$((fail+1)); echo "FAIL($v): $(basename "$r")";;
  esac
done
echo "additive-diff: $pass identical, $skip animated, $neg negative, $fail violations, $err build errors"
if [ "$err" -gt 0 ]; then
  echo "additive-diff: build errors say nothing about additivity — check the build first"
fi
rm -rf "$S"
[ "$fail" -eq 0 ] && [ "$err" -eq 0 ]
