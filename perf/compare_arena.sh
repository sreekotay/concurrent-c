#!/bin/bash
# compare_arena.sh - Compare per-fiber vs per-thread bump arena allocation throughput
#
# Each implementation uses a private bump-pointer arena per fiber/thread —
# no shared allocator, no malloc/free per alloc. Measures pure allocation
# throughput of the arena strategy.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

echo "================================================================="
echo "ARENA ALLOCATION STRATEGY COMPARISON"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
mkdir -p "$SCRIPT_DIR/out"
$CCC build --release "$SCRIPT_DIR/arena_contention_storm.ccs" -o "$SCRIPT_DIR/out/arena_contention_storm"
gcc -O2 "$SCRIPT_DIR/pthread_malloc_baseline.c" -o "$SCRIPT_DIR/out/pthread_malloc_baseline" -lpthread
echo "Done."
echo ""

# 2. Run Pthread Per-Thread Arena
echo "--- Running Pthread Per-Thread Arena ---"
"$SCRIPT_DIR/out/pthread_malloc_baseline" | tee arena_pthread_out.txt
echo ""

# 3. Run Concurrent-C Per-Fiber Arena
echo "--- Running Concurrent-C Per-Fiber Arena ---"
"$SCRIPT_DIR/out/arena_contention_storm" | tee arena_cc_out.txt
echo ""

# 4. Extract throughput
PTHREAD_TP=$(grep "Throughput" arena_pthread_out.txt | awk '{print $2}')
CC_TP=$(grep "Throughput" arena_cc_out.txt | awk '{print $2}')

echo "DATA_PTHREAD_ARENA_TP: $PTHREAD_TP"
echo "DATA_CC_ARENA_TP: $CC_TP"

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-20s\n" "Implementation" "Throughput (M/sec)"
printf "%-20s %-20s\n" "Pthread (Arena)" "$PTHREAD_TP"
printf "%-20s %-20s\n" "Concurrent-C (Arena)" "$CC_TP"
echo "-----------------------------------------------------------------"

if [ "$(python3 -c "print(1 if float($CC_TP) > float($PTHREAD_TP) else 0)")" -eq 1 ]; then
    GAIN=$(python3 -c "print(round(float($CC_TP)/float($PTHREAD_TP), 1))")
    echo "RESULT: SUCCESS - Concurrent-C per-fiber arena is ${GAIN}x faster than Pthread per-thread arena!"
else
    echo "RESULT: COMPARABLE - Pthread per-thread arena matches or beats CC per-fiber arena."
fi
echo "================================================================="

rm arena_pthread_out.txt arena_cc_out.txt
