#!/bin/bash
# compare_preemption.sh - Compare Concurrent-C vs Pthread fairness against CPU hogs
#
# This script runs the "Noisy Neighbor Challenge" for both implementations and compares
# their ability to maintain a "heartbeat" while other tasks are hogging the CPU.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
STRESS_DIR="$REPO_ROOT/stress"

# Configuration
HOGS=15
WORKERS=16
DURATION=5

echo "================================================================="
echo "NOISY NEIGHBOR COMPARISON (PREEMPTION TEST)"
echo "Workers: $WORKERS | CPU Hogs: $HOGS | Duration: ${DURATION}s"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
$CCC build "$STRESS_DIR/noisy_neighbor.ccs" -o "$STRESS_DIR/noisy_neighbor"
gcc -O3 "$STRESS_DIR/pthread_noisy_baseline.c" -o "$STRESS_DIR/pthread_noisy_baseline" -lpthread
echo "Done."
echo ""

run_test() {
    local name=$1
    local cmd=$2
    echo "--- Running $name ---"
    
    # Run and capture output
    $cmd > test_out.txt 2>&1 &
    local pid=$!
    
    # Wait for duration plus a little buffer
    sleep $((DURATION + 2))
    
    # Kill process if still running
    kill -9 $pid 2>/dev/null || true
    
    # Extract total heartbeats
    local b=$(grep "Tick" test_out.txt | wc -l | tr -d ' ')
    echo "$name Heartbeats: $b"
    echo "$b" > last_beats.txt
}

# 2. Run Pthread Baseline
run_test "Pthread (Baseline)" "$STRESS_DIR/pthread_noisy_baseline"
PTHREAD_BEATS=$(cat last_beats.txt)

echo ""

# 3. Run Concurrent-C
export CC_FIBER_WORKERS=$WORKERS
run_test "Concurrent-C" "$REPO_ROOT/bin/noisy_neighbor"
CC_BEATS=$(cat last_beats.txt)
rm last_beats.txt

echo ""
echo "================================================================="
echo "VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Total Heartbeats"
printf "%-20s %-15s\n" "Pthread (Baseline)" "$PTHREAD_BEATS"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_BEATS"
echo "-----------------------------------------------------------------"

# Expected beats: DURATION * 10 (since interval is 100ms)
EXPECTED=$((DURATION * 10))

if [ "$CC_BEATS" -ge "$((PTHREAD_BEATS - 5))" ]; then
    echo "RESULT: SUCCESS - Concurrent-C matched or exceeded Pthread fairness!"
    echo "The scheduler correctly preempted CPU-intensive fibers."
elif [ "$CC_BEATS" -gt 5 ]; then
    echo "RESULT: PARTIAL - Concurrent-C survived but showed significant jitter."
    echo "Heartbeat efficiency: $(python3 -c "print(f'{$CC_BEATS * 100.0 / $EXPECTED:.1f}%')")"
else
    echo "RESULT: FAIL - Concurrent-C was starved by CPU hogs."
    echo "The scheduler is purely cooperative and cannot handle noisy neighbors."
fi
echo "================================================================="
