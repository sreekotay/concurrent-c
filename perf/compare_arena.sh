#!/bin/bash
# compare_arena.sh - Compare Concurrent-C Arena vs Pthread Malloc
#
# This script measures allocation throughput under high concurrency.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

echo "================================================================="
echo "ARENA CONTENTION COMPARISON"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
mkdir -p "$SCRIPT_DIR/out"
$CCC build "$SCRIPT_DIR/arena_contention_storm.ccs" -o "$SCRIPT_DIR/out/arena_contention_storm"
gcc -O3 "$SCRIPT_DIR/pthread_malloc_baseline.c" -o "$SCRIPT_DIR/out/pthread_malloc_baseline" -lpthread
echo "Done."
echo ""

# 2. Run Pthread Baseline
echo "--- Running Pthread Malloc Baseline ---"
"$SCRIPT_DIR/out/pthread_malloc_baseline" | tee arena_pthread_out.txt
echo ""

# 3. Run Concurrent-C Arena
echo "--- Running Concurrent-C Arena ---"
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
printf "%-20s %-20s\n" "Pthread (Malloc)" "$PTHREAD_TP"
printf "%-20s %-20s\n" "Concurrent-C (Arena)" "$CC_TP"
echo "-----------------------------------------------------------------"

if [ "$(python3 -c "print(1 if float($CC_TP) > float($PTHREAD_TP) else 0)")" -eq 1 ]; then
    GAIN=$(python3 -c "print(round(float($CC_TP)/float($PTHREAD_TP), 1))")
    echo "RESULT: SUCCESS - Concurrent-C Arena is ${GAIN}x faster than malloc!"
else
    echo "RESULT: FAIL - Concurrent-C Arena is slower than system malloc."
fi
echo "================================================================="

rm arena_pthread_out.txt arena_cc_out.txt
