#!/bin/bash
# bench_conn_sweep.sh — measure redis_idiomatic RSS vs redis-benchmark -c
# (isolates how much RSS scales with concurrent clients vs fixed baseline).
#
# Prints a table: idle RSS, RSS during load (snapshot), RSS after bench stops,
# deltas and naive delta/clients.
#
# Environment (defaults match bench_robust.sh where applicable):
#   IDIOMATIC_BIN    path to server (default ./out/redis_idiomatic)
#   IDIOMATIC_PORT   listen port (default 6402)
#   BENCH_BIN        path to redis-benchmark
#   CLIENTS_SWEEP    space-separated -c values (default "1 5 10 50")
#   REQUESTS         -n per run (default 300000)
#   PIPELINE         -P (default 16)
#   RANDOM_KEYS      -r keyspace (default 50000)
#   BENCH_TESTS      -t command list (default set,get,incr)
#   SNAPSHOT_SEC     seconds after starting bench before RSS sample (default 2.5)
#   POST_IDLE_SEC    seconds after bench ends before second RSS sample (default 0.8)
#   CC_WORKERS         passed through when starting idiomatic (optional)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_BIN="${BENCH_BIN:-$SCRIPT_DIR/redis_c/src/redis-benchmark}"
IDIOMATIC_BIN="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"

IDIOMATIC_PORT="${IDIOMATIC_PORT:-6402}"
CLIENTS_SWEEP="${CLIENTS_SWEEP:-1 5 10 50}"
REQUESTS="${REQUESTS:-300000}"
PIPELINE="${PIPELINE:-16}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
BENCH_TESTS="${BENCH_TESTS:-set,get,incr}"
SNAPSHOT_SEC="${SNAPSHOT_SEC:-2.5}"
POST_IDLE_SEC="${POST_IDLE_SEC:-0.8}"

need() {
    if [[ ! -x "$1" ]]; then
        echo "missing $2 at $1" >&2
        exit 1
    fi
}
need "$BENCH_BIN" "redis-benchmark"
need "$IDIOMATIC_BIN" "redis_idiomatic"

wait_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
deadline = time.time() + 12.0
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", p), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
print(f"port {p} did not open", file=sys.stderr)
sys.exit(1)
PY
}

sample_rss_kb() {
    # macOS ps(1): rss is resident set size in kilobytes
    ps -p "$1" -o rss= 2>/dev/null | tr -d ' ' || echo 0
}

TMP_DIR="$(mktemp -d -t bench_conn_sweep.XXXXXX)"
LOG="$TMP_DIR/idiomatic.log"
echo "tmp dir: $TMP_DIR" >&2

if [[ -n "${CC_WORKERS:-}" ]]; then
    CC_WORKERS="$CC_WORKERS" "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$LOG" 2>&1 &
else
    "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$LOG" 2>&1 &
fi
SRV_PID=$!
trap 'if [[ -n "${SRV_PID:-}" ]]; then kill "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; fi' EXIT
wait_port "$IDIOMATIC_PORT"

idle_kb="$(sample_rss_kb "$SRV_PID")"
echo "redis_idiomatic pid=$SRV_PID port=$IDIOMATIC_PORT idle_rss_kb=$idle_kb"
echo "bench: -n $REQUESTS -P $PIPELINE -r $RANDOM_KEYS -t $BENCH_TESTS snapshot=${SNAPSHOT_SEC}s post_idle=${POST_IDLE_SEC}s"
echo ""
print_row() {
    printf '%-9s %14s %16s %20s %13s %14s %19s\n' "$@"
}

print_row "clients" "rss_during_kb" "delta_during_kb" "per_client_during" "rss_after_kb" "delta_after_kb" "per_client_after"
print_row "-------" "-------------" "---------------" "-----------------" "------------" "--------------" "----------------"
print_row "0 idle" "$idle_kb" "0" "-" "$(sample_rss_kb "$SRV_PID")" "0" "-"

for c in $CLIENTS_SWEEP; do
    "$BENCH_BIN" -h 127.0.0.1 -p "$IDIOMATIC_PORT" -n "$REQUESTS" -c "$c" -P "$PIPELINE" \
        -q -r "$RANDOM_KEYS" -t "$BENCH_TESTS" >"$TMP_DIR/bench_c${c}.log" 2>&1 &
    bench_pid=$!
    sleep "$SNAPSHOT_SEC"
    during_kb="$(sample_rss_kb "$SRV_PID")"
    kill "$bench_pid" 2>/dev/null || true
    wait "$bench_pid" 2>/dev/null || true
    sleep "$POST_IDLE_SEC"
    after_kb="$(sample_rss_kb "$SRV_PID")"
    dd=$((during_kb - idle_kb))
    da=$((after_kb - idle_kb))
    # awk for float division without python dependency for each line
    pc_during="$(awk -v d="$dd" -v c="$c" 'BEGIN { printf "%.1f", d / c }')"
    pc_after="$(awk -v d="$da" -v c="$c" 'BEGIN { printf "%.1f", d / c }')"
    print_row "$c" "$during_kb" "$dd" "$pc_during" "$after_kb" "$da" "$pc_after"
done

echo ""
echo "per_client_* = delta / clients (naive linear split; large at low c means fixed/shared cost dominates)." >&2
