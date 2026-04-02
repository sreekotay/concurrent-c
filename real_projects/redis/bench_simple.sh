#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
REDIS_SRC_DIR="$SCRIPT_DIR/redis_c/src"
BENCH_BIN="$REDIS_SRC_DIR/redis-benchmark"
UPSTREAM_BIN="$REDIS_SRC_DIR/redis-server"
IDIOMATIC_BIN="$SCRIPT_DIR/out/redis_idiomatic"

REQUESTS="${REQUESTS:-50000}"
CLIENTS="${CLIENTS:-50}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6392}"

need_bin() {
    local path="$1"
    local label="$2"
    if [[ ! -x "$path" ]]; then
        echo "missing $label at $path" >&2
        echo "hint: cd \"$SCRIPT_DIR\" && make upstream redis_idiomatic" >&2
        exit 1
    fi
}

wait_for_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket
import sys
import time

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

stop_server() {
    local pid="${1:-}"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
}

run_suite() {
    local label="$1"
    local port="$2"
    local line_set line_get line_incr

    echo
    echo "== $label =="

    line_set="$("$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$CLIENTS" -q -r "$RANDOM_KEYS" \
        SET bench:key:__rand_int__ value | tail -n 1)"
    line_get="$("$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$CLIENTS" -q -r "$RANDOM_KEYS" \
        GET bench:key:__rand_int__ | tail -n 1)"
    line_incr="$("$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$CLIENTS" -q -r "$RANDOM_KEYS" \
        INCR bench:ctr:__rand_int__ | tail -n 1)"

    echo "$line_set"
    echo "$line_get"
    echo "$line_incr"

    python3 - "$label" "$line_set" "$line_get" "$line_incr" <<'PY'
import re
import sys

label = sys.argv[1]
lines = sys.argv[2:]
for line in lines:
    m = re.match(r"(.+?):\s+([0-9.]+) requests per second, p50=([0-9.]+) msec", line.strip())
    if not m:
        print(f"{label}\tparse_error\t{line}")
        continue
    cmd, rps, p50 = m.groups()
    cmd = cmd.split()[0]
    print(f"{label}\t{cmd}\t{rps}\t{p50}")
PY
}

need_bin "$BENCH_BIN" "redis-benchmark"
need_bin "$UPSTREAM_BIN" "upstream redis-server"
need_bin "$IDIOMATIC_BIN" "redis_idiomatic"

tmp_dir="$(mktemp -d "$SCRIPT_DIR/.bench_simple.XXXXXX")"
upstream_log="$tmp_dir/upstream.log"
idiomatic_log="$tmp_dir/idiomatic.log"
upstream_pid=""
idiomatic_pid=""

cleanup() {
    stop_server "$upstream_pid"
    stop_server "$idiomatic_pid"
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

echo "Redis simple benchmark comparison"
echo "requests=$REQUESTS clients=$CLIENTS random_keys=$RANDOM_KEYS"

"$UPSTREAM_BIN" --save "" --appendonly no --port "$UPSTREAM_PORT" >"$upstream_log" 2>&1 &
upstream_pid="$!"
wait_for_port "$UPSTREAM_PORT"

env CC_DEADLOCK_ABORT=0 "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$idiomatic_log" 2>&1 &
idiomatic_pid="$!"
wait_for_port "$IDIOMATIC_PORT"

upstream_tsv="$(run_suite "upstream" "$UPSTREAM_PORT" | tee /dev/stderr | tail -n 3)"
idiomatic_tsv="$(run_suite "redis_idiomatic" "$IDIOMATIC_PORT" | tee /dev/stderr | tail -n 3)"

echo
echo "== Summary =="
python3 - "$upstream_tsv" "$idiomatic_tsv" <<'PY'
import sys

def parse_block(block):
    rows = {}
    for line in block.splitlines():
        parts = line.split("\t")
        if len(parts) != 4:
            continue
        _, cmd, rps, p50 = parts
        rows[cmd] = (float(rps), float(p50))
    return rows

upstream = parse_block(sys.argv[1])
idiomatic = parse_block(sys.argv[2])
cmds = ["SET", "GET", "INCR"]

print("command\tupstream_rps\tidiomatic_rps\tidiomatic/upstream\tupstream_p50_ms\tidiomatic_p50_ms")
for cmd in cmds:
    if cmd not in upstream or cmd not in idiomatic:
        print(f"{cmd}\tmissing\tmissing\tmissing\tmissing\tmissing")
        continue
    urps, up50 = upstream[cmd]
    irps, ip50 = idiomatic[cmd]
    ratio = irps / urps if urps else 0.0
    print(f"{cmd}\t{urps:.2f}\t{irps:.2f}\t{ratio:.3f}x\t{up50:.3f}\t{ip50:.3f}")
PY
