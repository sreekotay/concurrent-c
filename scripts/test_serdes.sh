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

# Mangling debt locks: beachhead gone; raw-body rewrite off by default.
EMIT="$ROOT/examples/serdes/c/pp_emit.cch"
if rg -q 'shadow_lower_expr_beachhead' "$EMIT"; then
  echo "[test_serdes] FAIL: shadow_lower_expr_beachhead still present"
  exit 1
fi
if ! rg -q '#define SHADOW_RAW_BODY_REWRITE 0' "$EMIT"; then
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

echo "test_serdes: OK"
