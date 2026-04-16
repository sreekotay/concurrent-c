#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
REDIS_SRC_DIR="$SCRIPT_DIR/redis_c/src"
BENCH_BIN="$REDIS_SRC_DIR/redis-benchmark"
UPSTREAM_BIN="$REDIS_SRC_DIR/redis-server"
HYBRID_BIN="${HYBRID_BIN:-$SCRIPT_DIR/out/redis_hybrid}"
IDIOMATIC_BIN="$SCRIPT_DIR/out/redis_idiomatic"

REQUESTS="${REQUESTS:-50000}"
CLIENTS="${CLIENTS:-50}"
PIPELINE="${PIPELINE:-16}"
REPEATS="${REPEATS:-3}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
BENCH_TESTS="${BENCH_TESTS:-set,get,incr}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
HYBRID_PORT="${HYBRID_PORT:-6392}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6393}"
BENCH_IDIOMATIC="${BENCH_IDIOMATIC:-0}"
HYBRID_CC_V2_THREADS="${HYBRID_CC_V2_THREADS:-}"
# Pause after stopping each server before starting the next. On macOS, redis-benchmark with
# many clients + pipeline leaves ephemeral ports in TIME_WAIT; 1s is often too short and the
# next server's benchmark hits "connection reset" / kills the hybrid process (SIGTRAP).
BENCH_GAP_SEC="${BENCH_GAP_SEC:-5}"

need_bin() {
    local path="$1"
    local label="$2"
    if [[ ! -x "$path" ]]; then
        echo "missing $label at $path" >&2
        echo "hint: cd \"$SCRIPT_DIR\" && make upstream redis_hybrid" >&2
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
    local bench_output
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
        echo "command: $BENCH_BIN ${bench_args[*]} -t $BENCH_TESTS" >&2
        bench_output="$("$BENCH_BIN" "${bench_args[@]}" -t "$BENCH_TESTS")"

        printf '%s\n' "$bench_output" >&2

        python3 - "$label" "$repeat" "$bench_output" <<'PY'
import re
import sys

label = sys.argv[1]
repeat = sys.argv[2]
lines = sys.argv[3].splitlines()
for line in lines:
    m = re.match(r"(.+?):\s+([0-9.]+) requests per second, p50=([0-9.]+) msec", line.strip())
    if not m:
        continue
    cmd, rps, p50 = m.groups()
    cmd = cmd.split()[0]
    print(f"{label}\t{repeat}\t{cmd}\t{rps}\t{p50}")
PY
    done
}

need_bin "$BENCH_BIN" "redis-benchmark"
need_bin "$UPSTREAM_BIN" "upstream redis-server"
need_bin "$HYBRID_BIN" "redis_hybrid"

if [[ "$BENCH_IDIOMATIC" == "1" ]]; then
    need_bin "$IDIOMATIC_BIN" "redis_idiomatic"
fi

tmp_dir="$(mktemp -d "$SCRIPT_DIR/.bench_simple.XXXXXX")"
upstream_log="$tmp_dir/upstream.log"
hybrid_log="$tmp_dir/hybrid.log"
idiomatic_log="$tmp_dir/idiomatic.log"
upstream_pid=""
hybrid_pid=""
idiomatic_pid=""

cleanup() {
    stop_server "$upstream_pid"
    stop_server "$hybrid_pid"
    stop_server "$idiomatic_pid"
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

echo "Redis benchmark: upstream vs redis_hybrid (set BENCH_IDIOMATIC=1 to include redis_idiomatic)"
echo "Each server runs alone (stop after its suite) so redis-benchmark bursts do not exhaust ephemeral ports/FDs on macOS."
echo "requests=$REQUESTS clients=$CLIENTS pipeline=$PIPELINE repeats=$REPEATS random_keys=$RANDOM_KEYS tests=$BENCH_TESTS"
if [[ -n "$HYBRID_CC_V2_THREADS" ]]; then
    echo "hybrid override: CC_V2_THREADS=$HYBRID_CC_V2_THREADS"
else
    echo "hybrid override: using runtime default CC_V2_THREADS"
fi

ulimit -n 65536 2>/dev/null || ulimit -n 8192 2>/dev/null || true

"$UPSTREAM_BIN" --save "" --appendonly no --port "$UPSTREAM_PORT" >"$upstream_log" 2>&1 &
upstream_pid="$!"
wait_for_port "$UPSTREAM_PORT"
upstream_tsv="$(run_suite "upstream" "$UPSTREAM_PORT")"
stop_server "$upstream_pid"
upstream_pid=""
sleep "$BENCH_GAP_SEC"

hybrid_env=(CC_DEADLOCK_ABORT=0)
if [[ -n "$HYBRID_CC_V2_THREADS" ]]; then
    hybrid_env+=(CC_V2_THREADS="$HYBRID_CC_V2_THREADS")
fi
env "${hybrid_env[@]}" "$HYBRID_BIN" "$HYBRID_PORT" >"$hybrid_log" 2>&1 &
hybrid_pid="$!"
wait_for_port "$HYBRID_PORT"
hybrid_tsv="$(run_suite "redis_hybrid" "$HYBRID_PORT")"
if ! kill -0 "$hybrid_pid" 2>/dev/null; then
    echo "redis_hybrid exited during its suite; log tail:" >&2
    tail -80 "$hybrid_log" >&2
    exit 1
fi
stop_server "$hybrid_pid"
hybrid_pid=""
sleep "$BENCH_GAP_SEC"

idiomatic_tsv=""
if [[ "$BENCH_IDIOMATIC" == "1" ]]; then
    env CC_DEADLOCK_ABORT=0 "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$idiomatic_log" 2>&1 &
    idiomatic_pid="$!"
    wait_for_port "$IDIOMATIC_PORT"
    idiomatic_tsv="$(run_suite "redis_idiomatic" "$IDIOMATIC_PORT")"
    stop_server "$idiomatic_pid"
    idiomatic_pid=""
fi

echo
echo "== Median Summary (clients=$CLIENTS pipeline=$PIPELINE repeats=$REPEATS) =="
if [[ "$BENCH_IDIOMATIC" == "1" ]]; then
    python3 - "$upstream_tsv" "$hybrid_tsv" "$idiomatic_tsv" <<'PY'
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
hybrid = parse_block(sys.argv[2])
idiomatic = parse_block(sys.argv[3])
cmds = ["SET", "GET", "INCR"]

print("command\tupstream_rps\thybrid_rps\thybrid/upstream\tidiomatic_rps\tidiomatic/upstream\tup_p50_ms\thy_p50_ms\tid_p50_ms\tup_rps_range\thy_rps_range\tid_rps_range")
for cmd in cmds:
    if cmd not in upstream or cmd not in hybrid or cmd not in idiomatic:
        print(f"{cmd}\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing")
        continue
    urps, up50, umin, umax = summarize(upstream[cmd])
    hrps, hp50, hmin, hmax = summarize(hybrid[cmd])
    irps, ip50, imin, imax = summarize(idiomatic[cmd])
    hratio = hrps / urps if urps else 0.0
    iratio = irps / urps if urps else 0.0
    print(f"{cmd}\t{urps:.2f}\t{hrps:.2f}\t{hratio:.3f}x\t{irps:.2f}\t{iratio:.3f}x\t{up50:.3f}\t{hp50:.3f}\t{ip50:.3f}\t{umin:.0f}-{umax:.0f}\t{hmin:.0f}-{hmax:.0f}\t{imin:.0f}-{imax:.0f}")
PY
else
    python3 - "$upstream_tsv" "$hybrid_tsv" <<'PY'
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
hybrid = parse_block(sys.argv[2])
cmds = ["SET", "GET", "INCR"]

print("command\tupstream_rps\thybrid_rps\thybrid/upstream\tupstream_p50_ms\thybrid_p50_ms\tupstream_rps_range\thybrid_rps_range")
for cmd in cmds:
    if cmd not in upstream or cmd not in hybrid:
        print(f"{cmd}\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing\tmissing")
        continue
    urps, up50, umin, umax = summarize(upstream[cmd])
    hrps, hp50, hmin, hmax = summarize(hybrid[cmd])
    ratio = hrps / urps if urps else 0.0
    print(f"{cmd}\t{urps:.2f}\t{hrps:.2f}\t{ratio:.3f}x\t{up50:.3f}\t{hp50:.3f}\t{umin:.0f}-{umax:.0f}\t{hmin:.0f}-{hmax:.0f}")
PY
fi
