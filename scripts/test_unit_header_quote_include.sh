#!/bin/sh
# #!ccc units copy into unit_native; quoted #include of a sibling .cch must
# still resolve from the original source directory.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT_DIR/cc/bin/ccc"

fail() { echo "[test_unit_header_quote_include] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat >"$work/sib.cch" <<'EOF'
static int sib(void) { return 0; }
EOF

cat >"$work/main.ccs" <<'EOF'
#!ccc ccs
#include "sib.cch"
int main(void) { return sib(); }
EOF

emitted="$work/main.c"
err="$work/err"
if ! "$CCC" --no-cache --out-dir "$work/out" --emit-c-only "$work/main.ccs" \
        -o "$emitted" >/dev/null 2>"$err"; then
    fail "emit-c-only of #!ccc unit with quoted include failed:
$(cat "$err")"
fi
[ -s "$emitted" ] || fail "no emitted C"
if grep -q 'cannot open #include' "$err"; then
    fail "quoted include missed the project tree:
$(cat "$err")"
fi

echo "[test_unit_header_quote_include] ok"
