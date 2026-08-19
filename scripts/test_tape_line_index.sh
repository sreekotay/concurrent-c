#!/bin/sh
# FileTape line index + emit cache.
#
# Cold `--no-cache` must stamp `#line 1000` from the index into emit.c.
# A warm cached rebuild of the same TU must still run. Two cached builds
# of the remapped undeclared-call fail must both print virt_tape.cch:100
# (erroring emits are not cached — each run rebuilds the index).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_tape_line_index] FAIL: $1" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/out" "$work/bin"

emit_from_verbose() {
  # shadow_lower: cc … -c "out/.cc-build/native/<hash>/emit.c" -o …
  sed -n 's/.*-c "\([^"]*\/emit\.c\)".*/\1/p' "$1" | head -1
}

# --- success path: `#line 1000` on a cold lower; warm cache still runs ---
"$CCC" --verbose build --no-cache --out-dir "$work/out" --bin-dir "$work/bin" \
    --link tests/tape_line_index_smoke.ccs -o "$work/bin/t" \
    >"$work/stderr.cold" 2>&1 || fail "cold smoke build"
"$work/bin/t" >"$work/run.cold" || fail "cold smoke run"
grep -q "tape_line_index_smoke ok" "$work/run.cold" \
  || fail "cold smoke stdout"

emit="$(emit_from_verbose "$work/stderr.cold")"
[ -n "$emit" ] && [ -f "$emit" ] || fail "cold: emit.c path missing in verbose log"
grep -q '#line 1000' "$emit" || fail "cold emit missing #line 1000"
grep -q 'virt_ok' "$emit" || fail "cold emit missing virt_ok path"

"$CCC" build --out-dir "$work/out" --bin-dir "$work/bin" --link \
    tests/tape_line_index_smoke.ccs -o "$work/bin/t" \
    >"$work/stderr.warm" 2>&1 || fail "warm smoke build"
"$work/bin/t" >"$work/run.warm" || fail "warm smoke run"
grep -q "tape_line_index_smoke ok" "$work/run.warm" \
  || fail "warm smoke stdout"
# Cached rebuild must not rewrite the product without the remapped #line.
[ -f "$emit" ] || fail "warm: cold emit.c disappeared"
grep -q '#line 1000' "$emit" || fail "warm emit missing #line 1000"

# --- fail path: two cached builds both report the remapped locus ---
rm -rf "$work/out" "$work/bin"
mkdir -p "$work/out" "$work/bin"

run_fail() {
  "$CCC" build --out-dir "$work/out" --bin-dir "$work/bin" --link \
      tests/tape_line_index_fail.ccs -o "$work/bin/f" \
      >"$work/stderr.$1" 2>&1 && return 0 || return 1
}

if run_fail 1; then
  fail "fail run 1: build unexpectedly succeeded"
fi
grep -q 'virt_tape.cch:100' "$work/stderr.1" \
  || fail "fail run 1 (cold): remapped locus missing"

if run_fail 2; then
  fail "fail run 2 (warm): build unexpectedly succeeded"
fi
grep -q 'virt_tape.cch:100' "$work/stderr.2" \
  || fail "fail run 2 (warm): remapped locus missing — index skipped?"

echo "[test_tape_line_index] OK"
