#!/bin/bash
# compare_herd.sh - Compare Concurrent-C vs Pthread thundering herd
#
# This script measures the latency to wake the first waiter when many are blocked.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

echo "================================================================="
echo "THUNDERING HERD COMPARISON"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
$CCC build "$SCRIPT_DIR/thundering_herd.ccs" -o "$SCRIPT_DIR/thundering_herd"
gcc -O3 "$SCRIPT_DIR/pthread_herd_baseline.c" -o "$SCRIPT_DIR/pthread_herd_baseline" -lpthread
echo "Done."
echo ""

# 2. Run Pthread Baseline
echo "--- Running Pthread Baseline ---"
"$SCRIPT_DIR/pthread_herd_baseline" | tee herd_pthread_out.txt
echo ""

# 3. Run Concurrent-C
echo "--- Running Concurrent-C ---"
"$SCRIPT_DIR/thundering_herd" | tee herd_cc_out.txt
echo ""

# 4. Extract average latency
PTHREAD_AVG=$(grep "Sample" herd_pthread_out.txt | sed 's/.*: *//; s/ ms//' | awk '{sum+=$1} END {if (NR>0) print sum/NR; else print 0}')
CC_AVG=$(grep "Sample" herd_cc_out.txt | sed 's/.*: *//; s/ ms//' | awk '{sum+=$1} END {if (NR>0) print sum/NR; else print 0}')

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Avg Latency (ms)"
printf "%-20s %-15s\n" "Pthread (Baseline)" "$PTHREAD_AVG"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_AVG"
echo "-----------------------------------------------------------------"

if [ "$(python3 -c "print(1 if float($CC_AVG) < float($PTHREAD_AVG) else 0)")" -eq 1 ]; then
    echo "RESULT: SUCCESS - Concurrent-C is faster and avoids thundering herds!"
else
    echo "RESULT: FAIL - Concurrent-C is slower than pthreads at waking waiters."
fi
echo "================================================================="

rm herd_pthread_out.txt herd_cc_out.txt
