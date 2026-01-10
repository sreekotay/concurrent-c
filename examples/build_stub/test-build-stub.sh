#!/usr/bin/env sh
set -euo pipefail

# Simple sanity check: ensure ccc build merges build.cc consts and CLI -D overrides.
# Assumes the ccc binary is at ./cc/bin/ccc relative to repo root.

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CC_BIN="$ROOT/cc/bin/ccc"
SRC="$ROOT/examples/build_stub/main.ccs"
OUT="/tmp/ccc_build_stub_out.c"

if [ ! -x "$CC_BIN" ]; then
  echo "cc binary not found at $CC_BIN" >&2
  exit 1
fi

run_and_check() {
  label="$1"
  shift
  expected="$1"
  shift
  echo "== $label"
  rm -f "$OUT"
  out="$("$CC_BIN" build --emit-c-only "$SRC" -o "$OUT" --dump-consts "$@" 2>&1)"
  printf "%s\n" "$out" | grep -F "$expected" >/dev/null
}

run_and_check "stub defaults" "CONST NUM_WORKERS=4"
run_and_check "cli overrides" "CONST NUM_WORKERS=8" "-DNUM_WORKERS=8"
run_and_check "target const" "CONST TARGET_PTR_WIDTH="  # just presence check
run_and_check "override build file path" "CONST USE_TLS=0" "--build-file" "$ROOT/examples/build_stub/build.cc" "-DUSE_TLS=0"

echo "OK"

