#!/bin/sh
# Quote-include policy: ordinary .h passthrough; CC faces still splice.
#
# 1) emit of the runtime smoke keeps `#include "….h"` and does not inline
#    the helper body (QUOTE_H_PASSTHROUGH_MARK).
# 2) missing ordinary .h must not produce lowerer `cannot open #include`
#    (`--emit-c-only` succeeds; host-cc would still fail).
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
CCC=./cc/bin/ccc

fail() { echo "[test_quote_h_passthrough] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"
[ -f tests/quote_h_passthrough_smoke.ccs ] || fail "missing smoke source"

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

emitted="$out_dir/quote_h_passthrough_smoke.c"
"$CCC" build --no-cache --emit-c-only tests/quote_h_passthrough_smoke.ccs \
    -o "$emitted" >/dev/null 2>&1 \
    || fail "emit-c-only of quote_h_passthrough_smoke.ccs failed"
[ -s "$emitted" ] || fail "no emitted C produced"

grep -q '#include "quote_h_passthrough_helper.h"' "$emitted" \
    || fail "emitted C dropped the quote include (spliced?)"
if grep -q 'QUOTE_H_PASSTHROUGH_MARK' "$emitted"; then
    fail "emitted C spliced the ordinary .h body"
fi

missing="$out_dir/missing_h.ccs"
printf '#include "no_such_foreign_only.h"\nint main(void) { return 0; }\n' \
    > "$missing"
missing_c="$out_dir/missing_h.c"
err="$out_dir/missing_h.err"
if ! "$CCC" build --no-cache --emit-c-only "$missing" -o "$missing_c" \
        >/dev/null 2>"$err"; then
    if grep -q 'cannot open #include' "$err"; then
        fail "missing ordinary .h still fails at the lowerer"
    fi
    fail "emit-c-only of missing .h failed: $(cat "$err")"
fi
[ -s "$missing_c" ] || fail "no emitted C for missing .h"
grep -q '#include "no_such_foreign_only.h"' "$missing_c" \
    || fail "missing .h was not passed through"
if grep -q 'cannot open #include' "$err"; then
    fail "lowerer diagnosed a missing ordinary .h"
fi

# File exists only via host -I (the curl_setup.h case): lowerer must not
# require it; host-cc finds it.
mkdir -p "$out_dir/inc"
printf 'static inline int foreign_add(int a, int b) { return a + b; }\n' \
    > "$out_dir/inc/curl_setup_standin.h"
printf '%s\n' '#include "curl_setup_standin.h"' \
    'int main(void) { return foreign_add(1, 2) == 3 ? 0 : 1; }' \
    > "$out_dir/foreign.ccs"
"$CCC" build --no-cache --link --cc-flags "-I$out_dir/inc" \
    "$out_dir/foreign.ccs" -o "$out_dir/foreign" >/dev/null 2>&1 \
    || fail "quote .h found only via --cc-flags -I failed to build"
"$out_dir/foreign" || fail "foreign -I binary returned nonzero"

# Bare CamelCase type: lowerer must not fail (host-cc typechecks).
printf 'int main(void) { CURLcode x = 0; (void)x; return 0; }\n' \
    > "$out_dir/camel.ccs"
if ! "$CCC" build --no-cache --emit-c-only "$out_dir/camel.ccs" \
        -o "$out_dir/camel.c" >/dev/null 2>"$out_dir/camel.err"; then
    fail "bare CamelCase type failed at the lowerer: $(cat "$out_dir/camel.err")"
fi
grep -q CURLcode "$out_dir/camel.c" || fail "CURLcode was not passed through"

echo "[test_quote_h_passthrough] ok"
