#!/usr/bin/env bash
# Run a command with host-side heartbeats + hard wall timeout.
# Survives hangs before the child prints anything (e.g. dlopen of a .node).
#
#   ./scripts/run_monitored.sh [timeout_secs] [heartbeat_secs] -- cmd args...
#   RUN_MONITORED_TIMEOUT=60 RUN_MONITORED_HEARTBEAT=5 \
#     ./scripts/run_monitored.sh -- node --expose-gc stress/bridge/js_python_fuzz.js
#
# Exit: child's status, or 124 on wall timeout (SIGKILL after SIGTERM).
set -euo pipefail

TIMEOUT_SECS="${RUN_MONITORED_TIMEOUT:-60}"
HEARTBEAT_SECS="${RUN_MONITORED_HEARTBEAT:-5}"

if [[ "${1:-}" =~ ^[0-9]+$ ]]; then
  TIMEOUT_SECS="$1"; shift
fi
if [[ "${1:-}" =~ ^[0-9]+$ ]]; then
  HEARTBEAT_SECS="$1"; shift
fi
if [[ "${1:-}" == "--" ]]; then shift; fi
if [[ $# -lt 1 ]]; then
  echo "usage: run_monitored.sh [timeout] [heartbeat] -- cmd args..." >&2
  exit 2
fi

T0=$(date +%s)
ts() {
  local now
  now=$(date +%s)
  printf '[monitor +%ds %s] ' "$((now - T0))" "$(date -u '+%H:%M:%S')"
}

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

ts; echo "start timeout=${TIMEOUT_SECS}s heartbeat=${HEARTBEAT_SECS}s cmd: $*"

# Line-buffer child stdout/stderr into $LOG and the terminal.
"$@" > >(tee -a "$LOG") 2> >(tee -a "$LOG" >&2) &
CHILD=$!

(
  while kill -0 "$CHILD" 2>/dev/null; do
    sleep "$HEARTBEAT_SECS" || exit 0
    kill -0 "$CHILD" 2>/dev/null || exit 0
    age=$(( $(date +%s) - T0 ))
    last=$(tail -n 1 "$LOG" 2>/dev/null | tr -d '\r' | head -c 160)
    nlines=$(wc -l <"$LOG" 2>/dev/null | tr -d ' ')
    ts
    echo "alive pid=$CHILD elapsed=${age}s lines=${nlines:-0} last=${last:-"(no output yet)"}"
    if [ "$age" -ge "$TIMEOUT_SECS" ]; then
      ts; echo "TIMEOUT ${TIMEOUT_SECS}s — SIGTERM pid=$CHILD" >&2
      kill -TERM "$CHILD" 2>/dev/null || true
      sleep 2
      kill -KILL "$CHILD" 2>/dev/null || true
      exit 0
    fi
  done
) &
HB=$!

set +e
wait "$CHILD"
st=$?
set -e
kill "$HB" 2>/dev/null || true
wait "$HB" 2>/dev/null || true
# drain tees
sleep 0.15 2>/dev/null || true

age=$(( $(date +%s) - T0 ))
if [ "$age" -ge "$TIMEOUT_SECS" ] && [ "$st" -ne 0 ]; then
  # distinguish kill-from-timeout when child died from signal
  if [ "$st" -ge 128 ]; then st=124; fi
fi
# If we SIGKILL'd, wait often returns 137/143
if ! kill -0 "$CHILD" 2>/dev/null; then
  :
fi
if [ "$age" -ge "$TIMEOUT_SECS" ] && { [ "$st" -eq 137 ] || [ "$st" -eq 143 ] || [ "$st" -eq 129 ]; }; then
  st=124
fi

ts; echo "done exit=$st elapsed=${age}s"
exit "$st"
