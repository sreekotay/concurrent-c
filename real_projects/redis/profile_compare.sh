#!/bin/bash
# Profile redis_idiomatic / upstream under the same bench load.
# Runs redis-benchmark in the background, samples each server for N seconds,
# then writes call-graph summaries to /tmp/profile_<label>_<cmd>.txt.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_BIN="$SCRIPT_DIR/redis_c/src/redis-benchmark"
UPSTREAM_BIN="$SCRIPT_DIR/redis_c/src/redis-server"
IDIOMATIC_BIN="$SCRIPT_DIR/out/redis_idiomatic_profile"

UPSTREAM_PORT=6391
IDIOMATIC_PORT=6393

# How long to sample for (seconds). 6s @ 1ms = ~6000 samples; good signal.
SAMPLE_SEC="${SAMPLE_SEC:-6}"
# redis-benchmark load parameters (match bench_simple defaults at P=16, C=50).
REQUESTS="${REQUESTS:-2000000}"   # big enough the bench outlives sampling
CLIENTS="${CLIENTS:-50}"
PIPELINE="${PIPELINE:-16}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
CMDS="${CMDS:-set get incr}"
OUT_DIR="${OUT_DIR:-/tmp/redis_profile_$(date +%s)}"
mkdir -p "$OUT_DIR"

ulimit -n 65536 2>/dev/null || ulimit -n 8192 2>/dev/null || true

wait_for_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
deadline = time.time() + 10.0
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
print(f"port {port} did not open in time", file=sys.stderr)
sys.exit(1)
PY
}

profile_server() {
    local label="$1"
    local bin="$2"
    local port="$3"
    shift 3
    local args=("$@")

    echo "== profiling $label (pid-to-be on port $port) =="

    # Start server
    "$bin" "${args[@]}" >"$OUT_DIR/${label}_server.log" 2>&1 &
    local pid=$!
    sleep 0.2
    wait_for_port "$port"

    for cmd in $CMDS; do
        # Start the load in the background; let it soak before we sample.
        "$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$CLIENTS" \
            -P "$PIPELINE" -r "$RANDOM_KEYS" -q -t "$cmd" \
            >"$OUT_DIR/${label}_${cmd}_bench.log" 2>&1 &
        local bench_pid=$!
        # Give the pipeline a moment to reach steady state.
        sleep 0.5
        echo "  sampling $cmd for ${SAMPLE_SEC}s..."
        sample "$pid" "$SAMPLE_SEC" -mayDie -f "$OUT_DIR/${label}_${cmd}.sample" \
            >/dev/null 2>&1 || true
        # Stop the bench (it's sized to outlive the sample window).
        kill "$bench_pid" 2>/dev/null || true
        wait "$bench_pid" 2>/dev/null || true
    done

    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# --- upstream ---
profile_server "upstream" "$UPSTREAM_BIN" "$UPSTREAM_PORT" \
    --save "" --appendonly no --port "$UPSTREAM_PORT"

# --- idiomatic ---
profile_server "idiomatic" "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" "$IDIOMATIC_PORT"

echo
echo "profiles in $OUT_DIR"
ls -la "$OUT_DIR"
