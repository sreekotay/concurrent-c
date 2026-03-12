#!/bin/bash
# compare_contention.sh - Compare Concurrent-C, Pthread, and Go channel isolation
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
echo "Using best-of-15 samples."
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

# 4. Run Go
GO_INTF=""
if command -v go >/dev/null 2>&1; then
    echo "--- Running Go ---"
    go run "$SCRIPT_DIR/go/channel_contention.go" | tee go_out.txt
    echo ""
else
    echo "--- Skipping Go (not found on PATH) ---"
    echo ""
fi

# 5. Extract results
PTHREAD_INTF=$(grep "^Interference:" pthread_out.txt | awk '{print $2}' | tr -d '%')
CC_INTF=$(grep "^Interference:" cc_out.txt | awk '{print $2}' | tr -d '%')
if [ -f go_out.txt ]; then
    GO_INTF=$(grep "^Interference:" go_out.txt | awk '{print $2}')
fi

echo "DATA_PTHREAD_INTERFERENCE: $PTHREAD_INTF"
echo "DATA_CC_INTERFERENCE: $CC_INTF"
if [ -n "$GO_INTF" ]; then
    echo "DATA_GO_INTERFERENCE: $GO_INTF"
fi

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Interference"
printf "%-20s %-15s\n" "Pthread (Baseline)" "${PTHREAD_INTF}%"
printf "%-20s %-15s\n" "Concurrent-C" "${CC_INTF}%"
if [ -n "$GO_INTF" ]; then
    printf "%-20s %-15s\n" "Go" "${GO_INTF}"
fi
echo "-----------------------------------------------------------------"

# Closer to 0% = better isolation. Negative = no interference at all.
if [ "$(python3 -c "print(1 if float($CC_INTF) <= float($PTHREAD_INTF) + 10 else 0)")" -eq 1 ]; then
    echo "RESULT: SUCCESS - Concurrent-C channels are well-isolated!"
else
    echo "RESULT: FAIL - Concurrent-C shows significant cross-channel interference."
fi
echo "================================================================="

rm -f pthread_out.txt cc_out.txt go_out.txt
