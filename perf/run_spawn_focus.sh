#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CCC="./cc/bin/ccc"
BIN_DIR="$ROOT/bin"

TOTAL="${CC_SPAWN_TOTAL:-100000}"
BATCH="${CC_SPAWN_BATCH:-1000}"
SAMPLES="${CC_SPAWN_SAMPLES:-7}"
WORKERS="${CC_WORKERS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"

echo "Spawn focus"
echo "==========="
echo "total=$TOTAL batch=$BATCH samples=$SAMPLES workers=$WORKERS"
echo

mkdir -p "$BIN_DIR"
rm -f "$ROOT/out/cc/obj/runtime/concurrent_c.o"

"$CCC" build --release --no-cache --out-dir out --bin-dir bin --link perf/perf_spawn_ladder.ccs -o bin/perf_spawn_ladder >/dev/null

echo "== Concurrent-C spawn ladder =="
CC_WORKERS="$WORKERS" CC_SPAWN_TOTAL="$TOTAL" CC_SPAWN_BATCH="$BATCH" CC_SPAWN_SAMPLES="$SAMPLES" "$BIN_DIR/perf_spawn_ladder"
echo
echo "== Concurrent-C nursery benchmark =="
CC_WORKERS="$WORKERS" "$CCC" run --release perf/spawn_nursery.ccs
echo
echo "== Concurrent-C simple nursery benchmark =="
CC_WORKERS="$WORKERS" "$CCC" run --release perf/spawn_nursery_simple.ccs
echo
echo "== Concurrent-C direct nursery benchmark =="
CC_WORKERS="$WORKERS" "$CCC" run --release perf/spawn_nursery_direct.ccs
echo
echo "== Go nursery reference =="
(cd perf/go && go run spawn_nursery.go)
