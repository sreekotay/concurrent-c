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
  CHAOS_WALL="${CHAOS_TIMEOUT:-120}"
  chaos_once() {
    OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-1}" \
      CHAOS_SCALE="$SCALE" \
      "$ROOT/scripts/run_monitored.sh" "$CHAOS_WALL" "${FUZZ_HEARTBEAT_SECS:-5}" -- \
        node --expose-gc "$ROOT/stress/bridge/js_python_chaos.js"
  }
  if ! chaos_once; then
    st=$?
    # In-process asyncio_lane_storm has aborted Node (SIGABRT) on GHA after
    # a prior in-process domain; one retry is the same as a fresh worker.
    if [ "$st" -eq 134 ]; then
      echo "[stress/bridge] chaos aborted (exit 134); retrying once"
      chaos_once || rc=1
    else
      rc=1
    fi
  fi

  echo "--- cc_node_stress_wire ---"
  CHAOS_SCALE="$SCALE" \
    PYTHONPATH="$ROOT/pypi/cc-node${PYTHONPATH:+:$PYTHONPATH}" \
    python3 "$ROOT/stress/bridge/cc_node_stress_wire.py" || rc=1
else
  echo "SKIP package-bridge sinks (need node + python3)"
fi

# CC embed parents (js.cch / py.cch Waves A–C). Needs a built ccc; skips
# individual modes when node / libpython / libnode / subinterp are absent.
if [[ -x "$ROOT/out/cc/bin/ccc" || -x "$ROOT/cc/bin/ccc" ]]; then
  CCC="$ROOT/out/cc/bin/ccc"
  [[ -x "$CCC" ]] || CCC="$ROOT/cc/bin/ccc"
  echo "--- cc_embed_stress ---"
  EMBED_WALL="${EMBED_TIMEOUT:-180}"
  CHAOS_SCALE="$SCALE" \
    "$ROOT/scripts/run_monitored.sh" "$EMBED_WALL" "${FUZZ_HEARTBEAT_SECS:-5}" -- \
      "$CCC" run "$ROOT/stress/bridge/cc_embed_stress.ccs" || rc=1
else
  echo "SKIP cc_embed_stress (need out/cc/bin/ccc or cc/bin/ccc)"
fi

exit "$rc"
