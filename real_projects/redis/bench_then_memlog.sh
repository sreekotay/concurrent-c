#!/bin/bash
# bench_then_memlog.sh — run redis-benchmark against redis_idiomatic, then CC.MEMLOG
# to print map/key/value arena slab sizes + committed bytes to stderr (server log).
#
# Env: PORT (default 6422), REQUESTS (default 300000), CLIENTS (50), PIPELINE (16),
#      RANDOM_KEYS (50000), BENCH_TESTS (set,get,incr)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIOMATIC="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"
BENCH="${BENCH_BIN:-$SCRIPT_DIR/redis_c/src/redis-benchmark}"
REDIS_CLI_BIN="$SCRIPT_DIR/redis_c/src/redis-cli"
CLI="${REDIS_CLI:-$REDIS_CLI_BIN}"

PORT="${PORT:-6422}"
REQUESTS="${REQUESTS:-300000}"
CLIENTS="${CLIENTS:-50}"
PIPELINE="${PIPELINE:-16}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
BENCH_TESTS="${BENCH_TESTS:-set,get,incr}"

need() {
    if [[ ! -x "$1" ]]; then
        echo "missing $2 at $1" >&2
        exit 1
    fi
}
need "$IDIOMATIC" "redis_idiomatic"
need "$BENCH" "redis-benchmark"
need "$CLI" "redis-cli"

wait_port() {
    python3 - "$1" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
for _ in range(200):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
print("timeout", file=sys.stderr)
sys.exit(1)
PY
}

LOG="$(mktemp -t bench_memlog.XXXXXX)"
echo "server log: $LOG" >&2
"$IDIOMATIC" "$PORT" >"$LOG" 2>&1 &
SP=$!
trap 'kill "$SP" 2>/dev/null || true; wait "$SP" 2>/dev/null || true' EXIT
wait_port "$PORT"

"$BENCH" -h 127.0.0.1 -p "$PORT" -n "$REQUESTS" -c "$CLIENTS" -P "$PIPELINE" \
    -q -r "$RANDOM_KEYS" -t "$BENCH_TESTS" >/dev/null

"$CLI" -h 127.0.0.1 -p "$PORT" CC.MEMLOG >/dev/null

echo "--- arena lines (from server stderr) ---" >&2
grep -E 'arena_initial|arena_map_slab|arena_key_slab|arena_value_slab|arena_committed|mem\[cc-memlog\]' "$LOG" || true
