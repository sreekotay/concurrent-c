#!/bin/bash
# bench_robust.sh — order-free, warmup-discarded bench for upstream redis-server
# vs redis_idiomatic.  Both servers are started once and run concurrently on
# their respective ports; each repeat randomises the binary order for every
# command so systematic order biases (thermal drift, TIME_WAIT backlog
# accumulating through the run, scheduler warmup) average out.
#
# Output: one summary block per command with mean/stddev/min/median/max/range
# in RPS, computed over REPEATS rounds (first round is warmup, discarded).
#
# Environment:
#   REPEATS          measured rounds (default 6)
#   REQUESTS         requests per round per binary (default 500000)
#   CLIENTS          concurrent clients (default 50)
#   PIPELINE         pipeline depth (default 16)
#   RANDOM_KEYS      keyspace for -r (default 50000)
#   BENCH_TESTS      comma-separated command list (default set,get,incr)
#   UPSTREAM_PORT    default 6391
#   IDIOMATIC_PORT   default 6393

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_BIN="$SCRIPT_DIR/redis_c/src/redis-benchmark"
UPSTREAM_BIN="$SCRIPT_DIR/redis_c/src/redis-server"
IDIOMATIC_BIN="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"

REPEATS="${REPEATS:-6}"
REQUESTS="${REQUESTS:-500000}"
CLIENTS="${CLIENTS:-50}"
PIPELINE="${PIPELINE:-16}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
BENCH_TESTS="${BENCH_TESTS:-set,get,incr}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6393}"

need() {
    if [[ ! -x "$1" ]]; then
        echo "missing $2 at $1" >&2
        exit 1
    fi
}
need "$BENCH_BIN" "redis-benchmark"
need "$UPSTREAM_BIN" "redis-server"
need "$IDIOMATIC_BIN" "redis_idiomatic"

TMP_DIR="$(mktemp -d -t bench_robust.XXXXXX)"
echo "tmp dir: $TMP_DIR" >&2
# Kill servers on exit but keep the tmp dir for post-mortem inspection.
trap 'for p in ${ALL_PIDS:-}; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done' EXIT

wait_port() {
    local port="$1"
    python3 - "$port" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
deadline = time.time() + 10.0
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

# --- start both servers concurrently ---
"$UPSTREAM_BIN"  --save "" --appendonly no --port "$UPSTREAM_PORT" >"$TMP_DIR/upstream.log"  2>&1 &
UPSTREAM_PID=$!
"$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$TMP_DIR/idiomatic.log" 2>&1 &
IDIOMATIC_PID=$!
ALL_PIDS="$UPSTREAM_PID $IDIOMATIC_PID"

wait_port "$UPSTREAM_PORT"
wait_port "$IDIOMATIC_PORT"

ulimit -n 65536 2>/dev/null || ulimit -n 8192 2>/dev/null || true

# Parse "SET: 1234567.89 requests per second" → "1234567.89".
# redis-benchmark prints a leading space on the final line (" SET: ...").
extract_rps() {
    grep -E "${1}: [0-9.]+ requests per second" "$2" | tail -1 \
        | sed -E 's/.*: ([0-9.]+) requests per second.*/\1/'
}

run_one() {
    # args: label port cmd outfile
    "$BENCH_BIN" -h 127.0.0.1 -p "$2" -n "$REQUESTS" -c "$CLIENTS" \
        -P "$PIPELINE" -q -r "$RANDOM_KEYS" -t "$3" \
        >"$4" 2>&1
}

IFS=',' read -ra CMDS <<< "$BENCH_TESTS"

port_for() {
    case "$1" in
        upstream)  echo "$UPSTREAM_PORT"  ;;
        idiomatic) echo "$IDIOMATIC_PORT" ;;
        *) echo "unknown label: $1" >&2; exit 1 ;;
    esac
}

RESULT_CSV="$TMP_DIR/results.csv"
echo "round,cmd,label,rps" >"$RESULT_CSV"

# Repeat 0 = warmup (discarded). Total runs = REPEATS + 1.
TOTAL=$((REPEATS + 1))
for r in $(seq 0 "$REPEATS"); do
    tag=$([[ $r == 0 ]] && echo "[warmup]" || echo "[round $r/$REPEATS]")
    echo >&2 "$tag"
    for cmd in "${CMDS[@]}"; do
        # Shuffle binary order for this (round, cmd) using python-backed rng.
        order_str="$(python3 -c "import random, sys
labels=['upstream','idiomatic']
random.shuffle(labels)
print(' '.join(labels))")"
        read -ra ORDER <<< "$order_str"
        for label in "${ORDER[@]}"; do
            out="$TMP_DIR/${label}_${cmd}_r${r}.log"
            run_one "$label" "$(port_for "$label")" "$cmd" "$out" || true
            # command is uppercased in bench output (e.g. SET)
            upper="$(echo "$cmd" | tr '[:lower:]' '[:upper:]')"
            rps="$(extract_rps "$upper" "$out" || true)"
            if [[ -z "${rps:-}" ]]; then
                echo >&2 "   $label $cmd: failed to parse rps; log=$out"
                continue
            fi
            printf "   %-9s %-5s rps=%s\n" "$label" "$cmd" "$rps" >&2
            if [[ $r -gt 0 ]]; then
                echo "$r,$cmd,$label,$rps" >>"$RESULT_CSV"
            fi
        done
    done
done

# --- stats ---
python3 - <<PY
import csv, statistics
from collections import defaultdict

rows = list(csv.DictReader(open("$RESULT_CSV")))
by = defaultdict(list)
for r in rows:
    by[(r["cmd"], r["label"])].append(float(r["rps"]))

cmds = sorted({r["cmd"] for r in rows}, key=lambda c: ["set","get","incr"].index(c) if c in ["set","get","incr"] else 99)
labels = ["upstream", "idiomatic"]

def fmt(x): return f"{x/1e6:.3f}M"

print()
print("== bench_robust summary ==")
print(f"rounds={$REPEATS} requests_per_round={$REQUESTS} clients={$CLIENTS} pipeline={$PIPELINE}")
print()
for cmd in cmds:
    print(f"[{cmd.upper()}]")
    print(f"  {'label':<10} {'mean':>8} {'median':>8} {'min':>8} {'max':>8} {'stddev':>8} {'cv%':>6}")
    for label in labels:
        vals = by.get((cmd, label), [])
        if not vals:
            continue
        mean = statistics.mean(vals)
        med  = statistics.median(vals)
        sd   = statistics.stdev(vals) if len(vals) > 1 else 0.0
        cv   = 100*sd/mean if mean else 0.0
        print(f"  {label:<10} {fmt(mean):>8} {fmt(med):>8} {fmt(min(vals)):>8} {fmt(max(vals)):>8} {fmt(sd):>8} {cv:>5.1f}%")
    # pairwise ratio (median-of-medians)
    ups = by.get((cmd, "upstream"), [])
    idm = by.get((cmd, "idiomatic"), [])
    if ups and idm:
        ratio = statistics.median(idm) / statistics.median(ups)
        overlap = not (max(idm) < min(ups) or max(ups) < min(idm))
        tag = "OVERLAP" if overlap else "SEPARATED"
        print(f"  idiomatic/upstream median: {ratio:.3f}x  ({tag})")
    print()
PY
