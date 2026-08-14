#!/usr/bin/env bash
# Loud fail if the in-tree TinyCC checkout is missing, unpatched, or stale.
# The test suite needs that libtcc (comptime / #line). Installed `ccc` does not.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TCC="$ROOT/third_party/tcc"
ABI_H="$ROOT/cc/src/tcc_ext_abi.h"

recipe() {
  cat <<'EOF'
The in-tree test suite needs patched TinyCC. Install-and-use does not.
From the repo root:

  ./scripts/fetch_submodules.sh
  ./scripts/apply_tcc_patches.sh
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  (cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs" libtcc.a tcc libtcc1.a)
  make cc -j"$jobs"

See docs/build-when.md (First checkout build).
EOF
}

fail() {
  echo "[test] FAIL: $*" >&2
  recipe >&2
  exit 1
}

fail_rebuild() {
  echo "[test] FAIL: $*" >&2
  echo "[test] From the repo root: ./scripts/apply_tcc_patches.sh && make -C cc" >&2
  echo "[test] See docs/build-when.md (Driver / TCC glue → make cc)." >&2
  exit 1
}

if [ ! -f "$TCC/tcc.h" ]; then
  fail "third_party/tcc is missing or not checked out"
fi
if ! grep -q 'CC_TCC_EXT_AVAILABLE' "$TCC/tcc.h"; then
  echo "[test] Unpatched libtcc drops #line resumes after the comptime prelude" >&2
  echo "[test] (undefined symbol 'cc_emit_tpl_splice_at', compile_err mismatches)." >&2
  fail "TinyCC is unpatched (tcc.h has no CC_TCC_EXT_AVAILABLE)"
fi
if [ ! -f "$TCC/config.mak" ] || ! grep -q 'CONFIG_cc_ext=yes' "$TCC/config.mak"; then
  fail "TinyCC was not configured with --config-cc_ext"
fi
if [ ! -f "$TCC/libtcc.a" ]; then
  fail "third_party/tcc/libtcc.a is missing"
fi

expected="$(sed -n 's/^#define CC_TCC_EXT_ABI_EXPECTED //p' "$ABI_H" | head -1)"
got="$(sed -n 's/^#define CC_TCC_EXT_ABI //p' "$TCC/tcc.h" | head -1)"
if [ -z "$expected" ]; then
  fail_rebuild "cc/src/tcc_ext_abi.h has no CC_TCC_EXT_ABI_EXPECTED"
fi
if [ -z "$got" ]; then
  fail_rebuild "tcc.h has no CC_TCC_EXT_ABI (patch older than this checkout)"
fi
if [ "$got" != "$expected" ]; then
  fail_rebuild "CC_TCC_EXT_ABI is $got, cc/ expects $expected (stale patch)"
fi

for p in "$ROOT"/third_party/tcc-patches/000*.patch; do
  if [ -f "$p" ] && [ "$p" -nt "$TCC/libtcc.a" ]; then
    fail_rebuild "libtcc.a is older than $(basename "$p")"
  fi
done

# Comptime @emit + #line resume — the actual splice_at / prelude contract.
# Markers can pass after git pull while yesterday's libtcc still relocates
# the prelude call as an undefined extern.
if [ -x "$ROOT/cc/bin/ccc" ]; then
  contract="$ROOT/tests/tcc_ext_emit_contract.ccs"
  if [ -f "$contract" ]; then
    if ! out="$("$ROOT/cc/bin/ccc" --no-cache run "$contract" 2>&1)"; then
      echo "$out" | tail -20 >&2
      fail_rebuild "comptime @emit/#line contract failed (stale ccc or libtcc.a)"
    fi
  fi
fi
