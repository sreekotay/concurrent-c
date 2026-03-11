#!/bin/bash
# compare_contention.sh - Compare Concurrent-C vs Pthread channel isolation
#
# This script runs the "Channel Isolation Challenge" for both implementations and
# measures how much independent channels interfere with each other under load.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

echo "================================================================="
echo "CHANNEL ISOLATION COMPARISON"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
mkdir -p "$SCRIPT_DIR/out"
$CCC build --release "$SCRIPT_DIR/channel_contention.ccs" -o "$SCRIPT_DIR/out/channel_contention"
gcc -O2 "$SCRIPT_DIR/pthread_contention_baseline.c" -o "$SCRIPT_DIR/out/pthread_contention_baseline" -lpthread
echo "Done."
echo ""

# 2. Run Pthread Baseline
echo "--- Running Pthread Baseline ---"
"$SCRIPT_DIR/out/pthread_contention_baseline" | tee pthread_out.txt
echo ""

# 3. Run Concurrent-C
echo "--- Running Concurrent-C ---"
"$SCRIPT_DIR/out/channel_contention" | tee cc_out.txt
echo ""

# 4. Extract results
PTHREAD_INTF=$(grep "^Interference:" pthread_out.txt | awk '{print $2}' | tr -d '%')
CC_INTF=$(grep "^Interference:" cc_out.txt | awk '{print $2}' | tr -d '%')

echo "DATA_PTHREAD_INTERFERENCE: $PTHREAD_INTF"
echo "DATA_CC_INTERFERENCE: $CC_INTF"

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Interference"
printf "%-20s %-15s\n" "Pthread (Baseline)" "${PTHREAD_INTF}%"
printf "%-20s %-15s\n" "Concurrent-C" "${CC_INTF}%"
echo "-----------------------------------------------------------------"

# Closer to 0% = better isolation. Negative = no interference at all.
if [ "$(python3 -c "print(1 if float($CC_INTF) <= float($PTHREAD_INTF) + 10 else 0)")" -eq 1 ]; then
    echo "RESULT: SUCCESS - Concurrent-C channels are well-isolated!"
else
    echo "RESULT: FAIL - Concurrent-C shows significant cross-channel interference."
fi
echo "================================================================="

rm -f pthread_out.txt cc_out.txt
