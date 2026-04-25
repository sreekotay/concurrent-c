#!/bin/bash
# Snapshot vmmap(1) for redis_idiomatic vs upstream under a short redis-benchmark.
# Explains why RSS >> logical DB size (threads, malloc zones, fiber VM).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIOMATIC="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"
UPSTREAM="${UPSTREAM_BIN:-$SCRIPT_DIR/redis_c/src/redis-server}"
BENCH="${BENCH_BIN:-$SCRIPT_DIR/redis_c/src/redis-benchmark}"
PORT_I="${PORT_I:-6397}"
PORT_U="${PORT_U:-6398}"

wait_port() {
  python3 - "$1" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
deadline = time.time() + 12.0
while time.time() < deadline:
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.2)
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
print("timeout", file=sys.stderr)
sys.exit(1)
PY
}

sample_vmmap() {
  local label="$1" pid="$2"
  echo ""
  echo "======== $label (pid $pid) vmmap -summary ========"
  vmmap -summary "$pid" 2>/dev/null | head -55 || true
}

run_pair() {
  local label="$1" port="$2" cmd="$3"
  shift 3
  "$cmd" "$@" >/tmp/mem_account_${label}.log 2>&1 &
  local pid=$!
  sleep 0.35
  wait_port "$port"
  sample_vmmap "${label}_idle" "$pid"
  "$BENCH" -h 127.0.0.1 -p "$port" -n 400000 -c 50 -P 16 -q -r 50000 -t set,get,incr >/tmp/mem_account_${label}_bench.log 2>&1 &
  local bp=$!
  sleep 2
  sample_vmmap "${label}_load" "$pid"
  kill "$bp" 2>/dev/null || true
  wait "$bp" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

need() { test -x "$1" || { echo "missing $2: $1" >&2; exit 1; }; }
need "$IDIOMATIC" "redis_idiomatic"
need "$UPSTREAM" "redis-server"
need "$BENCH" "redis-benchmark"

echo "Mach physical footprint (vmmap) tracks bench_robust RSS better than logical DB bytes."
echo "Compare MALLOC_* + VM_ALLOCATE resident vs upstream; Memory Tag 22 is mostly reserved VM."
echo ""

run_pair idiomatic "$PORT_I" env CC_REDIS_MEM_REPORT=1 "$IDIOMATIC" "$PORT_I"
run_pair upstream "$PORT_U" "$UPSTREAM" --save "" --appendonly no --port "$PORT_U"

echo ""
echo "Done. Server logs: /tmp/mem_account_idiomatic.log /tmp/mem_account_upstream.log"
