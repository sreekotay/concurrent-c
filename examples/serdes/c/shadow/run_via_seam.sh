#!/usr/bin/env bash
# Optional hosting seam: shadow_lower → host cc + concurrent_c.o (not default ccc).
# Usage: bash examples/serdes/c/shadow/run_via_seam.sh examples/hello.ccs
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SRC_ARG="${1:?usage: run_via_seam.sh <file.ccs>}"
# Relative paths are repo-rooted; absolute paths pass through (no double-prefix).
if [[ "$SRC_ARG" = /* ]]; then
  SRC="$SRC_ARG"
else
  SRC="$ROOT/$SRC_ARG"
fi
SHADOW_BIN="$ROOT/out/cc/bin/shadow_lower"
if [[ ! -x "$SHADOW_BIN" ]]; then
  SHADOW_BIN="$ROOT/cc/bin/shadow_lower"
fi
RT_O="$ROOT/out/cc/obj/runtime/concurrent_c.o"
TMP="${TMPDIR:-/tmp}/cc_serdes_seam_$$"
mkdir -p "$TMP"
name="$(basename "$SRC" .ccs)"
"$SHADOW_BIN" "$SRC" -o "$TMP/$name.c"
cc -std=c11 -I"$ROOT/out/include" -c "$TMP/$name.c" -o "$TMP/$name.o"
cc "$TMP/$name.o" "$RT_O" -o "$TMP/$name.bin" -lpthread -lm
"$TMP/$name.bin"
rm -rf "$TMP"
