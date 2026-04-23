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
#   BENCH_TESTS      comma-separated command list (default set,get)
#   UPSTREAM_PORT    default 6391
#   IDIOMATIC_PORT   default 6393
#   SAMPLE_INTERVAL  seconds between rss/thread samples (default 0.05)

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
BENCH_TESTS="${BENCH_TESTS:-set,get}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6393}"
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-0.05}"

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
CC_WORKERS_OVERRIDE="${CC_WORKERS:-}"
env_prefix=""
[[ -n "$CC_WORKERS_OVERRIDE"   ]] && env_prefix+="CC_WORKERS=$CC_WORKERS_OVERRIDE "
if [[ -n "$env_prefix" ]]; then
    env $env_prefix "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$TMP_DIR/idiomatic.log" 2>&1 &
else
    "$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$TMP_DIR/idiomatic.log" 2>&1 &
fi
IDIOMATIC_PID=$!
ALL_PIDS="$UPSTREAM_PID $IDIOMATIC_PID"

wait_port "$UPSTREAM_PORT"
wait_port "$IDIOMATIC_PORT"

check_server_alive() {
    local label="$1"
    local pid="$2"
    local log="$3"
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "$label server exited unexpectedly; log=$log" >&2
        exit 1
    fi
}

# --- rss/thread sampler (per-server, for the whole benchmark run) ---
# Writes "<rss_kb> <threads>" lines; final peak is max over the whole run.
cat > "$TMP_DIR/sampler.py" <<'PY'
import os, subprocess, sys, time
pid = int(sys.argv[1])
out_path = sys.argv[2]
interval = float(sys.argv[3])

def probe(pid):
    try:
        rss = int(subprocess.check_output(
            ["ps", "-p", str(pid), "-o", "rss="],
            stderr=subprocess.DEVNULL, text=True).strip())
    except Exception:
        return None
    try:
        if sys.platform == "darwin":
            lines = subprocess.check_output(
                ["ps", "-M", str(pid)],
                stderr=subprocess.DEVNULL, text=True).splitlines()
            # ps -M: header + USER line + 1 line per thread; thread-count
            # is the number of stack-indented lines.
            threads = max(1, sum(1 for ln in lines[1:] if ln.startswith(" ")))
        else:
            threads = int(subprocess.check_output(
                ["ps", "-o", "nlwp=", "-p", str(pid)],
                stderr=subprocess.DEVNULL, text=True).strip())
    except Exception:
        threads = 0
    return rss, threads

with open(out_path, "w", buffering=1) as f:
    while True:
        try:
            os.kill(pid, 0)
        except OSError:
            break
        s = probe(pid)
        if s is not None:
            f.write(f"{s[0]} {s[1]}\n")
        time.sleep(interval)
PY

python3 "$TMP_DIR/sampler.py" "$UPSTREAM_PID"  "$TMP_DIR/upstream_samples.txt"  "$SAMPLE_INTERVAL" &
UPSTREAM_SAMPLER_PID=$!
python3 "$TMP_DIR/sampler.py" "$IDIOMATIC_PID" "$TMP_DIR/idiomatic_samples.txt" "$SAMPLE_INTERVAL" &
IDIOMATIC_SAMPLER_PID=$!
ALL_PIDS="$ALL_PIDS $UPSTREAM_SAMPLER_PID $IDIOMATIC_SAMPLER_PID"

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
            check_server_alive "upstream" "$UPSTREAM_PID" "$TMP_DIR/upstream.log"
            check_server_alive "idiomatic" "$IDIOMATIC_PID" "$TMP_DIR/idiomatic.log"
            if ! run_one "$label" "$(port_for "$label")" "$cmd" "$out"; then
                echo >&2 "   $label $cmd: benchmark command failed; log=$out"
                exit 1
            fi
            check_server_alive "upstream" "$UPSTREAM_PID" "$TMP_DIR/upstream.log"
            check_server_alive "idiomatic" "$IDIOMATIC_PID" "$TMP_DIR/idiomatic.log"
            # command is uppercased in bench output (e.g. SET)
            upper="$(echo "$cmd" | tr '[:lower:]' '[:upper:]')"
            rps="$(extract_rps "$upper" "$out" || true)"
            if [[ -z "${rps:-}" ]]; then
                echo >&2 "   $label $cmd: failed to parse rps; log=$out"
                exit 1
            fi
            printf "   %-9s %-5s rps=%s\n" "$label" "$cmd" "$rps" >&2
            if [[ $r -gt 0 ]]; then
                echo "$r,$cmd,$label,$rps" >>"$RESULT_CSV"
            fi
        done
    done
done

# --- stop samplers and let them flush ---
for p in "$UPSTREAM_SAMPLER_PID" "$IDIOMATIC_SAMPLER_PID"; do
    kill "$p" 2>/dev/null || true
    wait "$p" 2>/dev/null || true
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

def read_samples(path):
    peaks = {"rss_kb": 0, "threads": 0, "n": 0}
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) != 2:
                    continue
                try:
                    rss = int(parts[0]); thr = int(parts[1])
                except ValueError:
                    continue
                peaks["rss_kb"] = max(peaks["rss_kb"], rss)
                peaks["threads"] = max(peaks["threads"], thr)
                peaks["n"] += 1
    except FileNotFoundError:
        pass
    return peaks

sample_peaks = {
    "upstream":  read_samples("$TMP_DIR/upstream_samples.txt"),
    "idiomatic": read_samples("$TMP_DIR/idiomatic_samples.txt"),
}

def fmt(x): return f"{x/1e6:.3f}M"
def fmt_mb(kb): return f"{kb/1024:.1f}MB" if kb else "  n/a"
def fmt_thr(t): return f"{t}" if t else "n/a"

print()
print("== bench_robust summary ==")
print(f"rounds={$REPEATS} requests_per_round={$REQUESTS} clients={$CLIENTS} pipeline={$PIPELINE} cc_workers={'$CC_WORKERS_OVERRIDE' or 'default'}")
print(f"sample_interval={$SAMPLE_INTERVAL}s  samples_taken: upstream={sample_peaks['upstream']['n']}  idiomatic={sample_peaks['idiomatic']['n']}")
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
    ups = by.get((cmd, "upstream"), [])
    idm = by.get((cmd, "idiomatic"), [])
    if ups and idm:
        ratio = statistics.median(idm) / statistics.median(ups)
        overlap = not (max(idm) < min(ups) or max(ups) < min(idm))
        tag = "OVERLAP" if overlap else "SEPARATED"
        print(f"  idiomatic/upstream median: {ratio:.3f}x  ({tag})")
    print()

print("[RESOURCE PEAKS] (sampled every $SAMPLE_INTERVAL s across the whole run)")
print(f"  {'label':<10} {'peak_rss':>10} {'peak_threads':>14}")
for label in labels:
    p = sample_peaks[label]
    print(f"  {label:<10} {fmt_mb(p['rss_kb']):>10} {fmt_thr(p['threads']):>14}")
print()
PY
