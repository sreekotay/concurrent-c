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
run_adv() {
  local bin="$1"
  shift
  if [[ -n "${MODE:-}" ]]; then
    python3 "$ROOT/stress/staticd/adversary.py" \
      --spawn "$bin" \
      --scale "$SCALE" \
      --seed "${FUZZ_SEED:-1}" \
      --mode "$MODE" \
      "$@"
  else
    python3 "$ROOT/stress/staticd/adversary.py" \
      --spawn "$bin" \
      --scale "$SCALE" \
      --seed "${FUZZ_SEED:-1}" \
      "$@"
  fi
}

run_adv "$STATICD_BIN"

# Compact-live / halfclose against forced poll (control). Native already ran above.
if [[ -z "${MODE:-}" || "${MODE}" == "waiter_compact_live" || "${MODE}" == "halfclose_after_request" ]]; then
  echo "=== poll control (waiter_compact_live, halfclose_after_request) ==="
  POLL_BIN="$ROOT/real_projects/staticd/out/staticd.poll"
  rm -f "$ROOT/real_projects/staticd/out/staticd"
  make -C "$ROOT/real_projects/staticd" staticd EXTRA_CFLAGS=-DCC_SERVER_WAIT_POLL=1
  cp "$ROOT/real_projects/staticd/out/staticd" "$POLL_BIN"
  # Restore native default binary for later local use.
  rm -f "$ROOT/real_projects/staticd/out/staticd"
  make -C "$ROOT/real_projects/staticd" staticd
  if [[ -z "${MODE:-}" || "${MODE}" == "waiter_compact_live" ]]; then
    MODE=waiter_compact_live run_adv "$POLL_BIN"
  fi
  if [[ -z "${MODE:-}" || "${MODE}" == "halfclose_after_request" ]]; then
    MODE=halfclose_after_request run_adv "$POLL_BIN"
  fi
fi
