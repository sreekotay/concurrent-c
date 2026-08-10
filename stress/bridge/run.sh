#!/usr/bin/env bash
# Package-bridge adversarial storms (host-driven — not picked up by run_all).
#
#   ./stress/bridge/run.sh                      # quick
#   CHAOS_SCALE=full ./stress/bridge/run.sh     # bigger N
#   CHAOS_SCALE=soak ./stress/bridge/run.sh     # + multi-second RSS soaks
#   FUZZ_SEED=42 ./stress/bridge/run.sh         # replay seeded walk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

SCALE="${CHAOS_SCALE:-quick}"
echo "=== stress/bridge scale=$SCALE ==="

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "SKIP: need $1" >&2
    return 1
  fi
  return 0
}

rc=0
if need node && need python3; then
  echo "--- js_python_fuzz ---"
  # Host monitor: heartbeats even if node hangs before first print (dlopen).
  FUZZ_WALL="${FUZZ_TIMEOUT:-60}"
  OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}" \
    CHAOS_SCALE="$SCALE" \
    FUZZ_SEED="${FUZZ_SEED-}" \
    FUZZ_OPS="${FUZZ_OPS-}" \
    FUZZ_TIMEOUT="$FUZZ_WALL" \
    FUZZ_HEARTBEAT_SECS="${FUZZ_HEARTBEAT_SECS:-5}" \
    "$ROOT/scripts/run_monitored.sh" "$FUZZ_WALL" "${FUZZ_HEARTBEAT_SECS:-5}" -- \
      node --expose-gc "$ROOT/stress/bridge/js_python_fuzz.js" || rc=1
  echo "[stress/bridge] fuzz end $(date -u +%H:%M:%SZ) rc_so_far=$rc"

  echo "--- js_python_chaos ---"
  OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}" \
    CHAOS_SCALE="$SCALE" \
    node --expose-gc "$ROOT/stress/bridge/js_python_chaos.js" || rc=1

  echo "--- cc_node_stress_wire ---"
  CHAOS_SCALE="$SCALE" \
    PYTHONPATH="$ROOT/pypi/cc-node${PYTHONPATH:+:$PYTHONPATH}" \
    python3 "$ROOT/stress/bridge/cc_node_stress_wire.py" || rc=1
else
  echo "SKIP stress/bridge (need node + python3)"
  exit 0
fi

exit "$rc"
