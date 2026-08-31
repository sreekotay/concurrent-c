#!/usr/bin/env sh
# Emit cache must miss when the lowerer at a stable path is overwritten
# (seed install replaces $PREFIX/bin/shadow_lower). Wiping out/ is not the
# product — the key has to carry that identity.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT/cc/bin/ccc"
if [ ! -x "$CCC" ]; then
  echo "ccc not found at $CCC (run: make cc)" >&2
  exit 1
fi

SL=""
if [ -x "$ROOT/out/cc/bin/shadow_lower" ]; then
  SL="$ROOT/out/cc/bin/shadow_lower"
elif [ -x "$ROOT/cc/bin/shadow_lower" ]; then
  SL="$ROOT/cc/bin/shadow_lower"
else
  echo "shadow_lower not found (run: make -C cc)" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/main.ccs" <<'EOF'
#include <ccc/std/prelude.cch>
int main(void) { return 0; }
EOF

cat >"$tmp/build.cc" <<'EOF'
CC_DEFAULT app
CC_TARGET app exe main.ccs
EOF

meta_val() {
  # One TU → one emit .meta (not host .obj / .link).
  cat "$tmp/out/.cc-build/"*__app__*.meta
}

# Same path both builds — the install case. First a real lowerer, then a
# wrapper at that path (different bytes, still lowers).
cp "$SL" "$tmp/sl"
chmod +x "$tmp/sl"

CC_SHADOW_LOWER="$tmp/sl" "$CCC" build --build-file "$tmp/build.cc" \
  --out-dir "$tmp/out" --bin-dir "$tmp/bin" >/dev/null
test -x "$tmp/bin/app" || { echo "missing $tmp/bin/app" >&2; exit 1; }
m1="$(meta_val)"
[ -n "$m1" ] || { echo "missing emit meta after first build" >&2; exit 1; }

printf '#!/bin/sh\nexec "%s" "$@"\n' "$SL" >"$tmp/sl"
chmod +x "$tmp/sl"

CC_SHADOW_LOWER="$tmp/sl" "$CCC" build --build-file "$tmp/build.cc" \
  --out-dir "$tmp/out" --bin-dir "$tmp/bin" >/dev/null
m2="$(meta_val)"
if [ "$m1" = "$m2" ]; then
  echo "stale emit cache: same-path shadow_lower overwrite did not change emit key" >&2
  echo "  meta=$m1" >&2
  exit 1
fi

# Warm: same wrapper — key must stay.
CC_SHADOW_LOWER="$tmp/sl" "$CCC" build --build-file "$tmp/build.cc" \
  --out-dir "$tmp/out" --bin-dir "$tmp/bin" >/dev/null
m3="$(meta_val)"
if [ "$m2" != "$m3" ]; then
  echo "emit key unstable across identical rebuilds" >&2
  echo "  after overwrite=$m2 warm=$m3" >&2
  exit 1
fi

echo "[test_emit_toolchain_cache] OK"
