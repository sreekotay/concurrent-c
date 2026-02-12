#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "== Build ladder benchmarks =="
cc/bin/ccc build --release --no-cache --out-dir out --bin-dir bin --link perf/perf_buffered_ladder.ccs -o bin/perf_buffered_ladder >/dev/null
cc -O3 -std=c11 -pthread perf/perf_buffered_ladder_ring_core.c -o bin/perf_buffered_ladder_ring_core

echo
for w in 1 2 8; do
  echo "== perf_buffered_ladder (CC_WORKERS=$w) =="
  CC_WORKERS="$w" bin/perf_buffered_ladder
  echo
done

echo "== perf_buffered_ladder_ring_core (native pthread) =="
bin/perf_buffered_ladder_ring_core
