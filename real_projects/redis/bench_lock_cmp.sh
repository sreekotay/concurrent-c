#!/bin/bash
# Head-to-head: upstream redis-server vs redis_idiomatic vs redis_lock.
# Same knobs as bench_robust.sh (subset). Warmup round discarded.
#
#   ./bench_lock_cmp.sh
#   REPEATS=3 PIPELINE=16 CLIENTS=50 ./bench_lock_cmp.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_BIN="$SCRIPT_DIR/redis_c/src/redis-benchmark"
UPSTREAM_BIN="$SCRIPT_DIR/redis_c/src/redis-server"
IDIOMATIC_BIN="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"
LOCK_BIN="${LOCK_BIN:-$SCRIPT_DIR/out/redis_lock}"

REPEATS="${REPEATS:-4}"
REQUESTS="${REQUESTS:-300000}"
CLIENTS="${CLIENTS:-50}"
PIPELINE="${PIPELINE:-16}"
RANDOM_KEYS="${RANDOM_KEYS:-50000}"
BENCH_TESTS="${BENCH_TESTS:-set,get,incr}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6393}"
LOCK_PORT="${LOCK_PORT:-6395}"

need() { [[ -x "$1" ]] || { echo "missing $2 at $1" >&2; exit 1; }; }
need "$BENCH_BIN" "redis-benchmark"
need "$UPSTREAM_BIN" "redis-server"
need "$IDIOMATIC_BIN" "redis_idiomatic"
need "$LOCK_BIN" "redis_lock"

TMP="$(mktemp -d -t bench_lock_cmp.XXXXXX)"
echo "tmp: $TMP" >&2
trap 'for p in ${PIDS:-}; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done' EXIT

wait_port() {
    python3 - "$1" <<'PY'
import socket, sys, time
p = int(sys.argv[1]); d = time.time() + 10
while time.time() < d:
    try:
        with socket.create_connection(("127.0.0.1", p), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
print("port", p, "did not open", file=sys.stderr); sys.exit(1)
PY
}

"$UPSTREAM_BIN" --save "" --appendonly no --port "$UPSTREAM_PORT" >"$TMP/upstream.log" 2>&1 &
PIDS="$!"
"$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$TMP/idiomatic.log" 2>&1 &
PIDS="$PIDS $!"
"$LOCK_BIN" "$LOCK_PORT" >"$TMP/lock.log" 2>&1 &
PIDS="$PIDS $!"

wait_port "$UPSTREAM_PORT"
wait_port "$IDIOMATIC_PORT"
wait_port "$LOCK_PORT"

port_for() {
    case "$1" in
        upstream) echo "$UPSTREAM_PORT" ;;
        idiomatic) echo "$IDIOMATIC_PORT" ;;
        lock) echo "$LOCK_PORT" ;;
    esac
}

extract_rps() {
    # "  throughput summary: 1666666.75 requests per second"
    local _upper="$1" log="$2"
    awk '/throughput summary:/ {
        for (i = 1; i <= NF; i++)
            if ($i + 0 == $i && $i > 1000) { printf "%d\n", $i + 0.5; exit }
    }' "$log"
}

IFS=',' read -ra CMDS <<< "$BENCH_TESTS"
CSV="$TMP/results.csv"
echo "round,cmd,label,rps" >"$CSV"

TOTAL=$((REPEATS + 1))
for r in $(seq 0 "$REPEATS"); do
    tag=$([[ $r == 0 ]] && echo "[warmup]" || echo "[round $r/$REPEATS]")
    echo >&2 "$tag"
    for cmd in "${CMDS[@]}"; do
        order_str="$(python3 -c "import random; L=['upstream','idiomatic','lock']; random.shuffle(L); print(' '.join(L))")"
        read -ra ORDER <<< "$order_str"
        for label in "${ORDER[@]}"; do
            out="$TMP/${label}_${cmd}_r${r}.log"
            port="$(port_for "$label")"
            "$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$CLIENTS" \
                -P "$PIPELINE" -r "$RANDOM_KEYS" -t "$cmd" >"$out" 2>&1 || {
                echo >&2 "   $label $cmd FAILED; see $out"
                exit 1
            }
            upper="$(echo "$cmd" | tr '[:lower:]' '[:upper:]')"
            rps="$(extract_rps "$upper" "$out" || true)"
            if [[ -z "${rps:-}" ]]; then
                echo >&2 "   $label $cmd: parse fail; log=$out"
                exit 1
            fi
            printf "   %-9s %-5s rps=%s\n" "$label" "$cmd" "$rps" >&2
            if [[ $r -gt 0 ]]; then
                echo "$r,$cmd,$label,$rps" >>"$CSV"
            fi
        done
    done
done

python3 - <<PY
import csv, statistics
from collections import defaultdict
rows = list(csv.DictReader(open("$CSV")))
by = defaultdict(list)
for r in rows:
    by[(r["cmd"], r["label"])].append(float(r["rps"]))
cmds = sorted({r["cmd"] for r in rows}, key=lambda c: ["set","get","incr"].index(c) if c in ("set","get","incr") else 99)
labels = ["upstream", "idiomatic", "lock"]
print()
print(f"{'cmd':<6} {'label':<10} {'median':>10} {'mean':>10} {'min':>10} {'max':>10}")
print("-" * 60)
for cmd in cmds:
    for lab in labels:
        xs = by.get((cmd, lab), [])
        if not xs: continue
        med = statistics.median(xs)
        print(f"{cmd:<6} {lab:<10} {med/1e6:9.3f}M {statistics.mean(xs)/1e6:9.3f}M {min(xs)/1e6:9.3f}M {max(xs)/1e6:9.3f}M")
    # relative to idiomatic
    idi = by.get((cmd, "idiomatic"), [])
    loc = by.get((cmd, "lock"), [])
    up = by.get((cmd, "upstream"), [])
    if idi and loc:
        im, lm = statistics.median(idi), statistics.median(loc)
        print(f"       lock/idiomatic = {lm/im:.2%}   lock/upstream = {lm/statistics.median(up):.2%}" if up else f"       lock/idiomatic = {lm/im:.2%}")
    print()
print(f"config: -c $CLIENTS -P $PIPELINE -n $REQUESTS -r $RANDOM_KEYS repeats=$REPEATS")
print(f"bins: idiomatic=$IDIOMATIC_BIN")
print(f"      lock=$LOCK_BIN")
PY
