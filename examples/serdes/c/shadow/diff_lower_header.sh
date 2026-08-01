#!/usr/bin/env bash
# Compare CCResult_* names from shadow emit vs production lower_header
# on result_frag.cch. Exit 0 if the mangled result type sets match.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
FRAG="$ROOT/examples/serdes/c/shadow/result_frag.cch"
SHADOW_H="$ROOT/examples/serdes/c/shadow/result_frag.h.golden"
LH_BIN="$ROOT/out/cc/bin/lower_headers"
TMP="${TMPDIR:-/tmp}/cc_serdes_lh_$$"
mkdir -p "$TMP/in" "$TMP/out"
cp "$FRAG" "$TMP/in/result_frag.cch"

if [[ ! -x "$LH_BIN" ]]; then
  make -C "$ROOT/cc" "$LH_BIN" >/dev/null
fi
"$LH_BIN" "$TMP/in" "$TMP/out"

extract() {
  # Unique concrete result type names (exclude FOO_DEFINED guards)
  grep -oE 'CCResult_[A-Za-z0-9_]+' "$1" | grep -v '_DEFINED$' | sort -u
}

SHADOW_SET=$(extract "$SHADOW_H")
LH_SET=$(extract "$TMP/out/result_frag.h")
rm -rf "$TMP"

if [[ "$SHADOW_SET" != "$LH_SET" ]]; then
  echo "FAIL: CCResult_ name set mismatch"
  echo "---- shadow ----"
  echo "$SHADOW_SET"
  echo "---- lower_header ----"
  echo "$LH_SET"
  exit 1
fi
echo "diff_lower_header: CCResult_ names match ($SHADOW_SET)"
