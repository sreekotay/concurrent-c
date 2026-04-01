#!/bin/bash
# compare_contention.sh - Compare Concurrent-C, Pthread, and Go shared-channel contention

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/cc/bin/ccc"

echo "================================================================="
echo "SHARED CHANNEL CONTENTION COMPARISON"
echo "================================================================="
echo ""

: "${CC_CONTENTION_ITERATIONS:=200000}"
: "${CC_CONTENTION_TRIALS:=2}"
: "${CC_CONTENTION_PRODUCERS:=8}"
: "${CC_CONTENTION_CONSUMERS:=8}"
export CC_CONTENTION_ITERATIONS
export CC_CONTENTION_TRIALS
export CC_CONTENTION_PRODUCERS
export CC_CONTENTION_CONSUMERS

CONTENTION_SHAPE="${CC_CONTENTION_PRODUCERS}x${CC_CONTENTION_CONSUMERS}"

# 1. Build implementations
echo "Building tests..."
mkdir -p "$SCRIPT_DIR/out"
$CCC build --release "$SCRIPT_DIR/channel_contention.ccs" -o "$SCRIPT_DIR/out/channel_contention"
gcc -O2 "$SCRIPT_DIR/pthread_contention_baseline.c" -o "$SCRIPT_DIR/out/pthread_contention_baseline" -lpthread
echo "Messages: ${CC_CONTENTION_ITERATIONS}"
echo "Trials: ${CC_CONTENTION_TRIALS}"
echo "Baseline shape: 1x1"
echo "Contention shape: ${CONTENTION_SHAPE}"
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
PTHREAD_BASE=$(grep "^  Best baseline:" pthread_out.txt | awk '{print $3}')
PTHREAD_CONT=$(grep "^  Best contention:" pthread_out.txt | awk '{print $3}')
PTHREAD_INTF=$(grep "^Interference:" pthread_out.txt | awk '{print $2}' | tr -d '%')
CC_BASE=$(grep "^  Best baseline:" cc_out.txt | awk '{print $3}')
CC_CONT=$(grep "^  Best contention:" cc_out.txt | awk '{print $3}')
CC_INTF=$(grep "^Interference:" cc_out.txt | awk '{print $2}' | tr -d '%')
if [ -f go_out.txt ]; then
    GO_BASE=$(grep "^  Best baseline:" go_out.txt | awk '{print $3}')
    GO_CONT=$(grep "^  Best contention:" go_out.txt | awk '{print $3}')
    GO_INTF=$(grep "^Interference:" go_out.txt | awk '{print $2}' | tr -d '%')
fi

echo "DATA_PTHREAD_BASELINE_MS: $PTHREAD_BASE"
echo "DATA_PTHREAD_CONTENTION_MS: $PTHREAD_CONT"
echo "DATA_PTHREAD_INTERFERENCE: $PTHREAD_INTF"
echo "DATA_CC_BASELINE_MS: $CC_BASE"
echo "DATA_CC_CONTENTION_MS: $CC_CONT"
echo "DATA_CC_INTERFERENCE: $CC_INTF"
if [ -n "$GO_INTF" ]; then
    echo "DATA_GO_BASELINE_MS: $GO_BASE"
    echo "DATA_GO_CONTENTION_MS: $GO_CONT"
    echo "DATA_GO_INTERFERENCE: $GO_INTF"
fi

echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
echo "Baseline 1x1 vs contention ${CONTENTION_SHAPE}"
printf "%-20s %-14s %-16s %-15s\n" "Implementation" "Baseline (ms)" "Contention (ms)" "Interference"
printf "%-20s %-14s %-16s %-15s\n" "Pthread (Baseline)" "$PTHREAD_BASE" "$PTHREAD_CONT" "${PTHREAD_INTF}%"
printf "%-20s %-14s %-16s %-15s\n" "Concurrent-C" "$CC_BASE" "$CC_CONT" "${CC_INTF}%"
if [ -n "$GO_INTF" ]; then
    printf "%-20s %-14s %-16s %-15s\n" "Go" "$GO_BASE" "$GO_CONT" "${GO_INTF}"
fi
echo "-----------------------------------------------------------------"

# Closer to 0% = lower overhead relative to the 1x1 baseline.
if [ "$(python3 -c "print(1 if float($CC_INTF) <= float($PTHREAD_INTF) + 10 else 0)")" -eq 1 ]; then
    echo "RESULT: SUCCESS - Concurrent-C stays competitive under shared contention."
else
    echo "RESULT: FAIL - Concurrent-C adds significant shared-contention overhead."
fi
echo "================================================================="

rm -f pthread_out.txt cc_out.txt go_out.txt
