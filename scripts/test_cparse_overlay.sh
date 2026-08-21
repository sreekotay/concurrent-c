#!/bin/sh
# Shadow overlay: FileTape → cparse for C-only struct fields and
# mid-declarator __attribute__ static fns. Requires a shadow_lower
# linked with libcparse (iterate, not last-good-only).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
SL=out/cc/bin/shadow_lower

fail() { echo "[test_cparse_overlay] FAIL: $1" >&2; exit 1; }

[ -x "$SL" ] || fail "missing $SL"

emit="$("$SL" tests/cparse_attr_fn_smoke.ccs --no-cache)" \
    || fail "lower cparse_attr_fn_smoke"
printf '%s\n' "$emit" | grep -q '__attribute__((unused))' \
    || fail "overlay dropped mid-declarator __attribute__((unused))"
printf '%s\n' "$emit" | grep -q 'cparse_unused_fn' \
    || fail "overlay dropped cparse_unused_fn"

emit2="$("$SL" tests/cparse_ifdef_fields_smoke.ccs --no-cache)" \
    || fail "lower cparse_ifdef_fields_smoke"
printf '%s\n' "$emit2" | grep -q '#ifdef _WIN32' \
    || fail "overlay dropped #ifdef _WIN32 in struct"
printf '%s\n' "$emit2" | grep -q 'handle' \
    || fail "overlay dropped handle"
printf '%s\n' "$emit2" | grep -q '#else' \
    || fail "overlay dropped #else"
printf '%s\n' "$emit2" | grep -q 'posix_pid' \
    || fail "overlay dropped posix_pid"
printf '%s\n' "$emit2" | grep -q '#endif' \
    || fail "overlay dropped #endif"

emit3="$("$SL" tests/cparse_elif_fields_smoke.ccs --no-cache)" \
    || fail "lower cparse_elif_fields_smoke"
printf '%s\n' "$emit3" | grep -q '#elif defined(__linux__)' \
    || fail "overlay dropped #elif defined(__linux__)"
printf '%s\n' "$emit3" | grep -q 'linux_fd' \
    || fail "overlay dropped linux_fd"
printf '%s\n' "$emit3" | grep -q 'posix_pid' \
    || fail "overlay dropped posix_pid after #elif"

emit4="$("$SL" tests/cparse_if_compound_smoke.ccs --no-cache)" \
    || fail "lower cparse_if_compound_smoke"
printf '%s\n' "$emit4" | grep -q 'defined(_WIN32) || defined(_WIN64)' \
    || fail "overlay dropped ||"
printf '%s\n' "$emit4" | grep -q 'defined(__linux__) && !defined(__ANDROID__)' \
    || fail "overlay dropped &&"

emit5="$("$SL" tests/cparse_if_full_smoke.ccs --no-cache)" \
    || fail "lower cparse_if_full_smoke"
printf '%s\n' "$emit5" | grep -q '1 ? 1 : 0' || fail "overlay dropped ?:"
printf '%s\n' "$emit5" | grep -q '__has_feature' || fail "overlay dropped __has_feature"
printf '%s\n' "$emit5" | grep -q '__has_builtin' || fail "overlay dropped __has_builtin"

emit6="$("$SL" tests/cparse_plain_fn_smoke.ccs --no-cache)" \
    || fail "lower cparse_plain_fn_smoke"
printf '%s\n' "$emit6" | grep -q 'cparse_plain_add' \
    || fail "overlay dropped cparse_plain_add"
printf '%s\n' "$emit6" | grep -q 'cparse_plain_id' \
    || fail "overlay dropped cparse_plain_id"
printf '%s\n' "$emit6" | grep -q 'return x + y' \
    || fail "overlay dropped add body"

emit7="$("$SL" tests/cparse_ufcs_meth_smoke.ccs --no-cache)" \
    || fail "lower cparse_ufcs_meth_smoke"
printf '%s\n' "$emit7" | grep -q 'CparseDoc_break_coalesce' \
    || fail "overlay did not emit CparseDoc_break_coalesce"
printf '%s\n' "$emit7" | grep -q 'CparseDoc_break_coalesce(&d)' \
    || fail "UFCS d.break_coalesce() did not lower (empty method table)"

echo "[test_cparse_overlay] ok"
