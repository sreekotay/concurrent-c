#!/usr/bin/env bash
# Loud fail if the in-tree TinyCC checkout is missing or unpatched.
# The test suite needs that libtcc (comptime / #line). Installed `ccc` does not.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TCC="$ROOT/third_party/tcc"

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
