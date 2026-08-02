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

if ! ./ccc tests/c_pp_shadow_emit_smoke.ccs -o bin/c_pp_shadow_emit_smoke; then
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
trap 'rm -f "$SMOKE_BIN"' EXIT
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
