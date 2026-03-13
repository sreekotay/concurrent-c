#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CCC="./cc/bin/ccc"
BIN_DIR="$ROOT/bin"

ITERS="${CC_BUF_ITERS:-200000}"
SAMPLES="${CC_BUF_SAMPLES:-5}"
WORKERS="${CC_WORKERS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

echo "Buffered focus"
echo "=============="
echo "iters=$ITERS samples=$SAMPLES cap=1000 workers=$WORKERS"
echo

mkdir -p "$BIN_DIR"

"$CCC" build --release --no-cache --out-dir out --bin-dir bin --link perf/perf_buffered_base.ccs -o bin/perf_buffered_base >/dev/null
"$CCC" build --release --no-cache --out-dir out --bin-dir bin --link perf/perf_buffered_core.ccs -o bin/perf_buffered_core >/dev/null
"$CCC" build --release --no-cache --out-dir out --bin-dir bin --link perf/perf_buffered_ladder.ccs -o bin/perf_buffered_ladder >/dev/null

echo "== Concurrent-C buffered base =="
CC_WORKERS="$WORKERS" CC_BUF_ITERS="$ITERS" CC_BUF_SAMPLES="$SAMPLES" "$BIN_DIR/perf_buffered_base"
echo
echo "== Concurrent-C buffered core =="
CC_WORKERS="$WORKERS" CC_BUF_ITERS="$ITERS" CC_BUF_SAMPLES="$SAMPLES" "$BIN_DIR/perf_buffered_core"
echo
echo "== Concurrent-C buffered ladder =="
CC_WORKERS="$WORKERS" CC_BUF_ITERS="$ITERS" CC_BUF_SAMPLES="$SAMPLES" "$BIN_DIR/perf_buffered_ladder"
echo
echo "== Go buffered reference =="
(cd perf/go && go run channel_throughput.go | rg "buffered|single-thread|unbuffered")
