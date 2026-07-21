#!/bin/sh
# Reparse-input sanitizer regressions.
#
# `__attribute__((constructor(N)))`: the pinned TCC has no priority-arg
# support, and whether the reparse ever SEES the attribute depends on the
# prelude's libc headers — glibc #define-erases __attribute__ for non-GCC
# compilers (so Linux CI passes with no help), the Apple SDK does not (so
# macOS reparses died with "')' expected (got '(')").  The fix blanks the
# priority arg in the reparse input, length-preserving.  This test asserts
# on the REPARSE INPUT DUMP itself, so it pins the sanitizer on every
# platform — including the ones where libc would mask the regression.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_reparse_sanitize] FAIL: $1" >&2; exit 1; }

dump_dir="$(mktemp -d)"
trap 'rm -rf "$dump_dir"' EXIT

CC_DEBUG_REPARSE_DUMP_DIR="$dump_dir" "$CCC" --no-cache --emit-c-only \
    tests/l2_rewriter_ctor_priority_smoke.ccs >/dev/null 2>&1 \
  || fail "l2_rewriter_ctor_priority_smoke failed to compile"

prepared="$dump_dir/reparse_prepared_tests_l2_rewriter_ctor_priority_smoke_ccs.c"
[ -f "$prepared" ] || prepared="$(ls "$dump_dir"/reparse_prepared_* 2>/dev/null | head -1)"
[ -n "$prepared" ] && [ -f "$prepared" ] || fail "no reparse_prepared dump produced (reparse never ran?)"

if grep -Eq '(constructor|destructor)\([0-9]' "$prepared"; then
  fail "ctor/dtor priority arg survived into the reparse input (TCC cannot parse it on macOS): $prepared"
fi
grep -q 'constructor    ' "$prepared" \
  || fail "expected blanked 'constructor    ' in reparse input, found none: $prepared"

echo "[test_reparse_sanitize] OK"
