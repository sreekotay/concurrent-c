#!/usr/bin/env bash
# SERDES parallel-path gate (examples/serdes/c shadow lowerer).
#
# Not the production compiler. ./scripts/test.sh exercises host `ccc` only.
# This script is the entrypoint for the experiment:
#   stage/tape smoke → emit goldens → stdlib hard-go → recipe runs → seam.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "[test_serdes] parallel path (shadow lowerer) — not production ccc"

# Mangling debt locks: deleted helpers gone; raw-body rewrite off by default.
EMIT_DIR="$ROOT/examples/serdes/c"
if rg -q 'shadow_lower_expr_beachhead|shadow_rewrite_slot|shadow_ufcs_apply' \
     "$EMIT_DIR"/pp_emit*.cch; then
  echo "[test_serdes] FAIL: deleted mangling helper still present"
  exit 1
fi
if ! rg -q '#define SHADOW_RAW_BODY_REWRITE 0' "$EMIT_DIR/pp_emit.cch"; then
  echo "[test_serdes] FAIL: SHADOW_RAW_BODY_REWRITE must default to 0"
  exit 1
fi

# Smokes live under tests/ but are skipped by default cc_test (c_pp_* / serdes).
if ! ./ccc tests/c_pp_stage_spike_smoke.ccs -o bin/c_pp_stage_spike_smoke; then
  echo "[test_serdes] FAIL: build c_pp_stage_spike_smoke"
  exit 1
fi
if ! ./bin/c_pp_stage_spike_smoke; then
  echo "[test_serdes] FAIL: c_pp_stage_spike_smoke"
  exit 1
fi

# Emit smoke includes pp_emit (generic-factory / comptime notes). Link the same
# comptime + libtcc stack as native shadow_lower (API boundary must stay aligned).
SHADOW_COMPTIME_LIB="$ROOT/out/cc/obj/libshadow_comptime.a"
TCC_LIB="$ROOT/third_party/tcc"
if [[ ! -f "$SHADOW_COMPTIME_LIB" || ! -f "$TCC_LIB/libtcc.a" ]]; then
  echo "[test_serdes] building libshadow_comptime.a (+ libtcc if needed)"
  make -C cc ../out/cc/obj/libshadow_comptime.a
fi
# --no-cache: smoke includes pp_*.cch; stale objects would miss capacity-diag edits.
if ! ./ccc tests/c_pp_shadow_emit_smoke.ccs -o bin/c_pp_shadow_emit_smoke --no-cache \
    --ld-flags "$SHADOW_COMPTIME_LIB -L$TCC_LIB -ltcc"; then
  echo "[test_serdes] FAIL: build c_pp_shadow_emit_smoke"
  exit 1
fi
if ! ./bin/c_pp_shadow_emit_smoke; then
  echo "[test_serdes] FAIL: c_pp_shadow_emit_smoke"
  exit 1
fi

bash scripts/test_serdes_shadow.sh

# Product build + cache smoke (succession path).
SHADOW_BIN="$ROOT/out/cc/bin/shadow_lower"
if [[ ! -x "$SHADOW_BIN" ]]; then
  echo "[test_serdes] FAIL: missing native shadow_lower at $SHADOW_BIN"
  exit 1
fi
SMOKE_BIN="${TMPDIR:-/tmp}/cc_serdes_build_smoke_$$"
CACHE_WORK="${TMPDIR:-/tmp}/cc_serdes_cache_deps_$$"
PROD_C="${TMPDIR:-/tmp}/cc_serdes_prod_generics_$$.c"
trap 'rm -rf "$SMOKE_BIN" "$CACHE_WORK" "$PROD_C"' EXIT
if ! "$SHADOW_BIN" examples/hello.ccs -o "$SMOKE_BIN"; then
  echo "[test_serdes] FAIL: shadow_lower host build"
  exit 1
fi
if ! "$SHADOW_BIN" examples/hello.ccs -o "$SMOKE_BIN"; then
  echo "[test_serdes] FAIL: shadow_lower cached rebuild"
  exit 1
fi
out="$("$SMOKE_BIN")"
if ! grep -Fq "All tasks completed." <<<"$out"; then
  echo "[test_serdes] FAIL: shadow-built hello missing expected output"
  exit 1
fi
echo "[test_serdes] shadow_lower host build + cache OK"

# Warm emit must miss when a quoted transitive include changes.
mkdir -p "$CACHE_WORK"
cp examples/serdes/c/shadow/cache_warm_main.ccs "$CACHE_WORK/main.ccs"
printf '%s\n' 'static int cache_warm_marker(void) { return 1; }' \
  >"$CACHE_WORK/cache_warm_dep.inc"
CACHE_BIN="$CACHE_WORK/bin"
if ! "$SHADOW_BIN" --no-cache "$CACHE_WORK/main.ccs" -o "$CACHE_BIN"; then
  echo "[test_serdes] FAIL: cache_warm cold build"
  exit 1
fi
out1="$("$CACHE_BIN")"
if [[ "$out1" != "marker=1" ]]; then
  echo "[test_serdes] FAIL: cache_warm unexpected out1: $out1"
  exit 1
fi
# Warm rebuild with unchanged deps (must stay green).
if ! "$SHADOW_BIN" "$CACHE_WORK/main.ccs" -o "$CACHE_BIN"; then
  echo "[test_serdes] FAIL: cache_warm warm rebuild (unchanged)"
  exit 1
fi
printf '%s\n' 'static int cache_warm_marker(void) { return 42; }' \
  >"$CACHE_WORK/cache_warm_dep.inc"
if ! "$SHADOW_BIN" "$CACHE_WORK/main.ccs" -o "$CACHE_BIN"; then
  echo "[test_serdes] FAIL: cache_warm rebuild after include edit"
  exit 1
fi
out2="$("$CACHE_BIN")"
if [[ "$out2" != "marker=42" ]]; then
  echo "[test_serdes] FAIL: warm cache stale after include edit (got: $out2)"
  exit 1
fi
echo "[test_serdes] shadow_lower transitive-dep invalidation OK"

# Product-path comptime golden: native shadow_lower on real CC_GENERIC_FACTORY
# (not the hand-seeded shadow/recipe_generics twin).
GOLDEN_PROD="$ROOT/examples/serdes/c/shadow/recipe_user_generics.product.c.golden"
if ! "$SHADOW_BIN" examples/recipe_user_generics.ccs -o "$PROD_C"; then
  echo "[test_serdes] FAIL: product lower recipe_user_generics"
  exit 1
fi
if ! python3 - "$PROD_C" "$GOLDEN_PROD" <<'PY'
import sys
from pathlib import Path
def norm(p):
    t = Path(p).read_bytes()
    while t.endswith(b"\n"):
        t = t[:-1]
    return t
if norm(sys.argv[1]) != norm(sys.argv[2]):
    sys.exit(1)
PY
then
  echo "[test_serdes] FAIL: recipe_user_generics product golden mismatch"
  echo "  regenerate: $SHADOW_BIN examples/recipe_user_generics.ccs -o $GOLDEN_PROD"
  exit 1
fi
if ! grep -Fq '/* generic Pair_int_double */' "$PROD_C"; then
  echo "[test_serdes] FAIL: product emit missing factory splice marker"
  exit 1
fi
echo "[test_serdes] product comptime golden OK"

# Opt-in ccc --frontend=serdes delegates to native shadow_lower.
FRONT_BIN="${TMPDIR:-/tmp}/cc_serdes_frontend_smoke_$$"
if ! "$ROOT/out/cc/bin/ccc" --frontend=serdes examples/hello.ccs -o "$FRONT_BIN"; then
  echo "[test_serdes] FAIL: ccc --frontend=serdes"
  rm -f "$FRONT_BIN"
  exit 1
fi
fout="$("$FRONT_BIN")"
rm -f "$FRONT_BIN"
if ! grep -Fq "All tasks completed." <<<"$fout"; then
  echo "[test_serdes] FAIL: --frontend=serdes hello missing expected output"
  exit 1
fi
echo "[test_serdes] ccc --frontend=serdes OK"

echo "test_serdes: OK"
