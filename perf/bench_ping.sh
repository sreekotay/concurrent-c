#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/cc/bin/ccc"
OUT_DIR="$SCRIPT_DIR/out"
SERVER_BIN="$OUT_DIR/ping_server"
CLIENT_BIN="$OUT_DIR/ping_bench_client"

: "${CC_PING_PORT:=6545}"
: "${CC_PING_CLIENTS:=100}"
: "${CC_PING_REQUESTS:=100000}"

mkdir -p "$OUT_DIR"

echo "Building ping benchmark..."
"$CCC" build --release "$SCRIPT_DIR/ping_server.ccs" -o "$SERVER_BIN"
cc -O3 "$SCRIPT_DIR/ping_bench_client.c" -o "$CLIENT_BIN" -lpthread

echo "Starting ping server on port $CC_PING_PORT..."
LOG_FILE="$OUT_DIR/ping_server.log"
"$SERVER_BIN" "$CC_PING_PORT" >"$LOG_FILE" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

python3 - "$CC_PING_PORT" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
deadline = time.time() + 10.0
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY

echo "Running ping benchmark..."
"$CLIENT_BIN" "$CC_PING_PORT"
