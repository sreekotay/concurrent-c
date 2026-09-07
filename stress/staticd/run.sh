#!/usr/bin/env bash
# staticd adversarial storms (host-driven).
#
#   ./stress/staticd/run.sh
#   CHAOS_SCALE=full ./stress/staticd/run.sh
#   MODE=slowloris_headers ./stress/staticd/run.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SCALE="${CHAOS_SCALE:-quick}"
STATICD_BIN="${STATICD_BIN:-$ROOT/real_projects/staticd/out/staticd}"

echo "=== stress/staticd scale=$SCALE ==="

if [[ ! -x "$STATICD_BIN" ]]; then
  echo "building staticd..."
  make -C "$ROOT/real_projects/staticd" staticd
fi

if [[ ! -f "$ROOT/real_projects/staticd/fixtures/1kb.bin" ]]; then
  (cd "$ROOT/real_projects/staticd" && ./gen_fixtures.sh)
fi

WALL="${STATICD_STRESS_TIMEOUT:-180}"
if [[ -n "${MODE:-}" ]]; then
  python3 "$ROOT/stress/staticd/adversary.py" \
    --spawn "$STATICD_BIN" \
    --scale "$SCALE" \
    --seed "${FUZZ_SEED:-1}" \
    --mode "$MODE"
else
  python3 "$ROOT/stress/staticd/adversary.py" \
    --spawn "$STATICD_BIN" \
    --scale "$SCALE" \
    --seed "${FUZZ_SEED:-1}"
fi
