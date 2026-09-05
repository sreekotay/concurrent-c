#!/usr/bin/env bash
# correctness + optional latency bench. --smoke: correctness + short wrk.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SMOKE=0
if [[ "${1:-}" == "--smoke" ]]; then
    SMOKE=1
fi

[[ -f fixtures/manifest.txt ]] || ./gen_fixtures.sh
make staticd
if [[ ! -x out/darkhttpd ]]; then
    make darkhttpd 2>/dev/null || ./setup.sh --darkhttpd-only && make darkhttpd || true
fi

echo "=== correctness ==="
./correctness.sh

if [[ "$SMOKE" == "1" ]]; then
    echo "=== smoke latency (4kb.html @ c=10) ==="
    SMOKE=1 BENCH_OUT="${BENCH_OUT:-benchmarks/smoke_latest.txt}" ./bench_latency.sh
elif [[ "${1:-}" == "--full" || "${FULL:-0}" == "1" ]]; then
    echo "=== full latency matrix (30s receipts) ==="
    DATE=$(date -u +%Y_%m_%d)
    FULL=1 BENCH_OUT="${BENCH_OUT:-benchmarks/staticd_${DATE}.txt}" ./bench_latency.sh
else
    echo "=== directional latency (1s) ==="
    DATE=$(date -u +%Y_%m_%d)
    BENCH_OUT="${BENCH_OUT:-benchmarks/staticd_${DATE}.txt}" ./bench_latency.sh
fi

echo "compare: done"
