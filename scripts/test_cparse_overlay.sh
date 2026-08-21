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

emit8="$("$SL" tests/cparse_comma_fields_smoke.ccs --no-cache)" \
    || fail "lower cparse_comma_fields_smoke"
printf '%s\n' "$emit8" | grep -q 'size_t a' || fail "overlay dropped comma field a"
printf '%s\n' "$emit8" | grep -q 'size_t b' || fail "overlay dropped comma field b"
printf '%s\n' "$emit8" | grep -q 'size_t c' || fail "overlay dropped comma field c"
printf '%s\n' "$emit8" | grep -q 'int \* p' || fail "overlay dropped int *p"
printf '%s\n' "$emit8" | grep -q 'int q' || fail "overlay dropped int q"
printf '%s\n' "$emit8" | grep -q 'int \* r' || fail "overlay dropped int *r"
printf '%s\n' "$emit8" | grep -q 'int \* s' || fail "overlay dropped int *s (star on later comma name)"

emit9="$("$SL" tests/cparse_nested_fields_smoke.ccs --no-cache)" \
    || fail "lower cparse_nested_fields_smoke"
printf '%s\n' "$emit9" | grep -q 'union' || fail "overlay dropped nested union"
printf '%s\n' "$emit9" | grep -q 'inner_x' || fail "overlay dropped nested struct field"
if printf '%s\n' "$emit9" | grep -q 'a1 fat'; then
    fail "overlay chopped fat union into type+name"
fi
body9="$(printf '%s\n' "$emit9" | sed -n '/typedef struct CparseNest/,/^} CparseNest;/p')"
printf '%s\n' "$body9" | grep -q 'a00' || fail "overlay chopped fat union (a00)"
printf '%s\n' "$body9" | grep -q 'a79' || fail "overlay chopped fat union (a79)"
printf '%s\n' "$body9" | grep -q 'extra' || fail "overlay dropped extra after nested"

emit10="$("$SL" tests/cparse_cc_keep_overlay_smoke.ccs --no-cache)" \
    || fail "lower cparse_cc_keep_overlay_smoke"
if printf '%s\n' "$emit10" | grep -q 'int x = 3'; then
    fail "overlay reprinted default arg as C"
fi
if printf '%s\n' "$emit10" | grep -q '@typeview(Mut)'; then
    fail "overlay reprinted @typeview param as C"
fi
printf '%s\n' "$emit10" | grep -q 'cparse_default_arg' \
    || fail "overlay dropped cparse_default_arg"
printf '%s\n' "$emit10" | grep -q 'cparse_tv_store' \
    || fail "overlay dropped cparse_tv_store"

emit11="$("$SL" tests/cparse_mixed_fields_smoke.ccs --no-cache)" \
    || fail "lower cparse_mixed_fields_smoke"
printf '%s\n' "$emit11" | grep -q '#ifdef _WIN32' \
    || fail "mixed: dropped #ifdef"
printf '%s\n' "$emit11" | grep -q 'posix_pid' \
    || fail "mixed: dropped posix_pid"
printf '%s\n' "$emit11" | grep -q 'size_t a' || fail "mixed: dropped comma a"
printf '%s\n' "$emit11" | grep -q 'size_t b' || fail "mixed: dropped comma b"
if printf '%s\n' "$emit11" | grep -q '!>('; then
    fail "mixed: reprinted !> as C"
fi
if printf '%s\n' "$emit11" | grep -q 'int\[:\]'; then
    fail "mixed: reprinted int[:] as C"
fi
printf '%s\n' "$emit11" | grep -q 'slice_field' || fail "mixed: dropped slice_field"
printf '%s\n' "$emit11" | grep -q 'res_field' || fail "mixed: dropped res_field"
printf '%s\n' "$emit11" | grep -q 'CCSlice' \
    || fail "mixed: slice_field not lowered to CCSlice"
printf '%s\n' "$emit11" | grep -q 'CCResult' \
    || fail "mixed: res_field not lowered to CCResult"

# >4096 member tokens: heap CpTok (no silent beachhead / token-cap skip).
fat="$(mktemp "${TMPDIR:-/tmp}/cparse_fat.XXXXXX.ccs")"
{
    echo 'typedef struct CparseFat {'
    i=0
    while [ "$i" -lt 2000 ]; do
        echo "    int f$i;"
        i=$((i + 1))
    done
    echo '} CparseFat;'
    echo 'int main(void) { return 0; }'
} >"$fat"
"$SL" "$fat" --no-cache >/dev/null || {
    rm -f "$fat"
    fail "fat field list (>=4096 toks) must lower"
}
rm -f "$fat"

echo "[test_cparse_overlay] ok"
