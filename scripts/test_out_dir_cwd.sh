#!/bin/sh
# Checkout ccc writes default / relative --out-dir against cwd, not the
# compiler repo root.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT_DIR/cc/bin/ccc"

fail() { echo "[test_out_dir_cwd] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

printf 'int main(void) { return 0; }\n' > cwd_out_dir_probe.ccs

"$CCC" --no-cache --out-dir out --emit-c-only cwd_out_dir_probe.ccs \
    >/dev/null 2>&1 \
    || fail "emit-c-only with relative --out-dir failed"
[ -f "$work/out/cwd_out_dir_probe.c" ] \
    || fail "expected $work/out/cwd_out_dir_probe.c"
if [ -f "$ROOT_DIR/out/cwd_out_dir_probe.c" ]; then
    fail "checkout ccc wrote into the compiler repo out/"
fi

rm -rf "$work/out"
"$CCC" --no-cache --emit-c-only cwd_out_dir_probe.ccs >/dev/null 2>&1 \
    || fail "emit-c-only with default out/ failed"
[ -f "$work/out/cwd_out_dir_probe.c" ] \
    || fail "default out/ was not cwd-relative"
if [ -f "$ROOT_DIR/out/cwd_out_dir_probe.c" ]; then
    fail "default out/ landed in the compiler repo"
fi

echo "[test_out_dir_cwd] ok"
