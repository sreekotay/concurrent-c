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
PIPELINE="${PIPELINE:-1}"
REPEATS="${REPEATS:-3}"
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
    local bench_args=(
        -h 127.0.0.1
        -p "$port"
        -n "$REQUESTS"
        -c "$CLIENTS"
        -P "$PIPELINE"
        -q
        -r "$RANDOM_KEYS"
    )

    echo >&2
    echo "== $label ==" >&2

    for repeat in $(seq 1 "$REPEATS"); do
        echo "repeat $repeat/$REPEATS" >&2

        line_set="$("$BENCH_BIN" "${bench_args[@]}" SET bench:key:__rand_int__ value | tail -n 1)"
        line_get="$("$BENCH_BIN" "${bench_args[@]}" GET bench:key:__rand_int__ | tail -n 1)"
        line_incr="$("$BENCH_BIN" "${bench_args[@]}" INCR bench:ctr:__rand_int__ | tail -n 1)"

        echo "$line_set" >&2
        echo "$line_get" >&2
        echo "$line_incr" >&2

        python3 - "$label" "$repeat" "$line_set" "$line_get" "$line_incr" <<'PY'
import re
import sys

label = sys.argv[1]
repeat = sys.argv[2]
lines = sys.argv[3:]
for line in lines:
    m = re.match(r"(.+?):\s+([0-9.]+) requests per second, p50=([0-9.]+) msec", line.strip())
    if not m:
        print(f"{label}\t{repeat}\tparse_error\t{line}")
        continue
    cmd, rps, p50 = m.groups()
    cmd = cmd.split()[0]
    print(f"{label}\t{repeat}\t{cmd}\t{rps}\t{p50}")
PY
    done
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

echo "Redis benchmark comparison"
echo "requests=$REQUESTS clients=$CLIENTS pipeline=$PIPELINE repeats=$REPEATS random_keys=$RANDOM_KEYS"

"$UPSTREAM_BIN" --save "" --appendonly no --port "$UPSTREAM_PORT" >"$upstream_log" 2>&1 &
upstream_pid="$!"
wait_for_port "$UPSTREAM_PORT"

env CC_DEADLOCK_ABORT=0 "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$idiomatic_log" 2>&1 &
idiomatic_pid="$!"
wait_for_port "$IDIOMATIC_PORT"

upstream_tsv="$(run_suite "upstream" "$UPSTREAM_PORT")"
idiomatic_tsv="$(run_suite "redis_idiomatic" "$IDIOMATIC_PORT")"

echo
echo "== Median Summary (clients=$CLIENTS pipeline=$PIPELINE repeats=$REPEATS) =="
python3 - "$upstream_tsv" "$idiomatic_tsv" <<'PY'
import statistics
import sys

def parse_block(block):
    rows = {}
    for line in block.splitlines():
        parts = line.split("\t")
        if len(parts) != 5:
            continue
        _, _, cmd, rps, p50 = parts
        rows.setdefault(cmd, []).append((float(rps), float(p50)))
    return rows

def summarize(samples):
    rps = [sample[0] for sample in samples]
    p50 = [sample[1] for sample in samples]
    return (
        statistics.median(rps),
        statistics.median(p50),
        min(rps),
        max(rps),
    )

upstream = parse_block(sys.argv[1])
idiomatic = parse_block(sys.argv[2])
cmds = ["SET", "GET", "INCR"]

print("command\tupstream_median_rps\tidiomatic_median_rps\tidiomatic/upstream\tupstream_median_p50_ms\tidiomatic_median_p50_ms\tupstream_rps_range\tidiomatic_rps_range")
for cmd in cmds:
    if cmd not in upstream or cmd not in idiomatic:
        print(f"{cmd}\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing")
        continue
    urps, up50, umin, umax = summarize(upstream[cmd])
    irps, ip50, imin, imax = summarize(idiomatic[cmd])
    ratio = irps / urps if urps else 0.0
    print(f"{cmd}\t{urps:.2f}\t{irps:.2f}\t{ratio:.3f}x\t{up50:.3f}\t{ip50:.3f}\t{umin:.0f}-{umax:.0f}\t{imin:.0f}-{imax:.0f}")
PY
