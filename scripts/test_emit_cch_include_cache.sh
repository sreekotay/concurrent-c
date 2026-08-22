#!/usr/bin/env sh
# ccc build writes C under out/c/<hash>/ and reuses it when the emit key
# matches. That key used to hash only the .ccs (+ #pragma cc_depends), so
# editing an included .cch left stale C. Host .d files then rebuilt .o from
# that body against the new header — clang blamed the .cch for a call that
# was not on that line.
#
# Do not cmp the emitted .c: local .cch lowers to a path-stable .h, so the
# TU text can be identical while the header (and the program) changed.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CCC="$ROOT/cc/bin/ccc"
if [ ! -x "$CCC" ]; then
  echo "ccc not found at $CCC (run: make cc)" >&2
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cat >"$tmp/h.cch" <<'EOF'
static int helper(void) { return 1; }
EOF

cat >"$tmp/main.ccs" <<'EOF'
#include <ccc/std/prelude.cch>
#include "h.cch"
int main(void) { return helper(); }
EOF

cat >"$tmp/build.cc" <<'EOF'
CC_DEFAULT app
CC_TARGET app exe main.ccs
CC_TARGET_INCLUDE app .
EOF

"$CCC" build --build-file "$tmp/build.cc" \
  --out-dir "$tmp/out" --bin-dir "$tmp/bin" >/dev/null
test -x "$tmp/bin/app" || { echo "missing $tmp/bin/app" >&2; exit 1; }
rc=0
"$tmp/bin/app" || rc=$?
if [ "$rc" -ne 1 ]; then
  echo "first run: expected exit 1, got $rc" >&2
  exit 1
fi

cat >"$tmp/h.cch" <<'EOF'
static int helper(void) { return 2; }
EOF

"$CCC" --verbose build --build-file "$tmp/build.cc" \
  --out-dir "$tmp/out" --bin-dir "$tmp/bin" >"$tmp/rebuild.txt" 2>&1
if ! grep -q shadow_lower "$tmp/rebuild.txt"; then
  echo "stale emit cache: included .cch edit did not re-lower" >&2
  exit 1
fi
rc=0
"$tmp/bin/app" || rc=$?
if [ "$rc" -ne 2 ]; then
  echo "after .cch edit: expected exit 2, got $rc (stale lowered C?)" >&2
  exit 1
fi

echo "[test_emit_cch_include_cache] OK"
