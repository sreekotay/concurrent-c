#!/bin/bash
# compare_contention.sh - Compare Concurrent-C vs Pthread cache-line contention
#
# This script runs the "Contention Challenge" for both implementations and compares
# how much throughput drops when an adjacent channel is hammered.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

echo "================================================================="
echo "CACHE-LINE CONTENTION COMPARISON"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
$CCC build "$SCRIPT_DIR/channel_contention.ccs" -o "$SCRIPT_DIR/channel_contention"
gcc -O3 "$SCRIPT_DIR/pthread_contention_baseline.c" -o "$SCRIPT_DIR/pthread_contention_baseline" -lpthread
echo "Done."
echo ""

# 2. Run Pthread Baseline
echo "--- Running Pthread Baseline ---"
"$SCRIPT_DIR/pthread_contention_baseline" | tee pthread_out.txt
echo ""

# 3. Run Concurrent-C
echo "--- Running Concurrent-C ---"
"$SCRIPT_DIR/channel_contention" | tee cc_out.txt
echo ""

# 4. Extract results
PTHREAD_DROP=$(grep "Throughput Drop" pthread_out.txt | tail -n 1 | awk '{print $3}' | tr -d '%')
CC_DROP=$(grep "Throughput Drop" cc_out.txt | tail -n 1 | awk '{print $3}' | tr -d '%')

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Throughput Drop"
printf "%-20s %-15s\n" "Pthread (Baseline)" "${PTHREAD_DROP}%"
printf "%-20s %-15s\n" "Concurrent-C" "${CC_DROP}%"
echo "-----------------------------------------------------------------"

# Note: A lower drop is better.
if [ "$(python3 -c "print(1 if float($CC_DROP) <= float($PTHREAD_DROP) + 10 else 0)")" -eq 1 ]; then
    echo "RESULT: SUCCESS - Concurrent-C handles contention well!"
else
    echo "RESULT: FAIL - Concurrent-C shows significant cache-line interference."
    echo "Hint: Add CACHE_LINE_SIZE padding to the CCChan struct."
fi
echo "================================================================="

rm pthread_out.txt cc_out.txt
