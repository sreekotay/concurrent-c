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

# Generated-unwrap-handler sanitizer must be LINE-NEUTRAL.  Multi-line
# lowered `!>` handlers open with "{\n"; the ` (void)0;` placeholder stamp
# used to be a fixed memcpy at `{`+1, overwriting that newline — one line
# lost per handler in the reparse input, so recorder lines below ran N-low
# and line-keyed UFCS probes missed (the redis "no member named
# 'write_reply'" dead band).  Assert on the reparse dump of a fixture with
# multi-line handlers: the handler `{` must keep its newline (stamp lands
# on a later line), and the joined `{ (void)0;` signature must not appear.
dump_dir2="$(mktemp -d)"
trap 'rm -rf "$dump_dir" "$dump_dir2"' EXIT

CC_DEBUG_REPARSE_DUMP_DIR="$dump_dir2" "$CCC" --no-cache --emit-c-only \
    tests/ufcs_below_multiline_unwrap_smoke.ccs >/dev/null 2>&1 \
  || fail "ufcs_below_multiline_unwrap_smoke failed to compile"

prepared2="$(ls "$dump_dir2"/reparse_prepared_* 2>/dev/null | head -1)"
[ -n "$prepared2" ] && [ -f "$prepared2" ] || fail "no reparse_prepared dump for unwrap fixture"

grep -Eq '_is_err\(__cc_pu[A-Za-z0-9_]*\)\) \{ *$' "$prepared2" \
  || fail "fixture's multi-line unwrap handler not found newline-intact in reparse input (fixture drifted, or sanitizer ate the '{'-adjacent newline): $prepared2"
if grep -Eq '_is_err\(__cc_pu[A-Za-z0-9_]*\)\) \{ \(void\)0;' "$prepared2"; then
  fail "unwrap sanitizer stamped ' (void)0;' over the handler's '{'-adjacent newline (line-eating regression): $prepared2"
fi

echo "[test_reparse_sanitize] OK"
