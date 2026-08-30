#!/bin/sh
# Wrap files are content-keyed (hash.ccs / hash.shcc), not hash.PID.
# A second ccc of the same unit must reuse emit/obj — no host cc -c.
set -eu
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT_DIR/cc/bin/ccc"

fail() { echo "[test_wrap_cache_stable] FAIL: $1" >&2; exit 1; }

[ -x "$CCC" ] || fail "missing $CCC"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat >"$work/t.shcc" <<'EOF'
#!/usr/bin/env -S ccc
io.println("ok") !>;
EOF

cat >"$work/t.ccs" <<'EOF'
#!ccc ccs
int main(void) { return 0; }
EOF

out="$work/out"
err1="$work/err1"
err2="$work/err2"

"$CCC" --out-dir "$out" "$work/t.shcc" >/dev/null 2>"$err1" ||
    fail "first shcc run:
$(cat "$err1")"
"$CCC" --verbose --out-dir "$out" "$work/t.shcc" >/dev/null 2>"$err2" ||
    fail "second shcc run:
$(cat "$err2")"

wraps=$(find "$out/.cc-build/shcc_native" -name '*.ccs' -o -name '*.tmp' 2>/dev/null | wc -l | tr -d ' ')
# exactly one wrap, name is 16 hex + .ccs
stable=$(find "$out/.cc-build/shcc_native" -name '????????????????.ccs' | wc -l | tr -d ' ')
[ "$stable" = "1" ] || fail "expected one hash.ccs wrap, got stable=$stable wraps=$wraps:
$(ls -la "$out/.cc-build/shcc_native" 2>/dev/null || true)"
if find "$out/.cc-build/shcc_native" -name '*.*.ccs' | grep -q .; then
    fail "pid-suffixed shcc wrap still present:
$(ls -la "$out/.cc-build/shcc_native")"
fi
if grep -E 'shadow_lower: .*-c ' "$err2" >/dev/null; then
    fail "second shcc run recompiled (cache miss):
$(cat "$err2")"
fi

"$CCC" --out-dir "$out" build "$work/t.ccs" -o "$out/t" >/dev/null 2>"$err1" ||
    fail "first ccs build:
$(cat "$err1")"
"$CCC" --verbose --out-dir "$out" build "$work/t.ccs" -o "$out/t" >/dev/null 2>"$err2" ||
    fail "second ccs build:
$(cat "$err2")"

stable=$(find "$out/.cc-build/unit_native" -name '????????????????.ccs' | wc -l | tr -d ' ')
[ "$stable" = "1" ] || fail "expected one unit_native hash.ccs, got $stable:
$(ls -la "$out/.cc-build/unit_native" 2>/dev/null || true)"
if find "$out/.cc-build/unit_native" -name '*.*.ccs' | grep -q .; then
    fail "pid-suffixed unit wrap still present:
$(ls -la "$out/.cc-build/unit_native")"
fi
if grep -E 'shadow_lower: .*-c ' "$err2" >/dev/null; then
    fail "second ccs build recompiled (cache miss):
$(cat "$err2")"
fi

echo "[test_wrap_cache_stable] ok"
