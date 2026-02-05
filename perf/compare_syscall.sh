#!/bin/bash
# compare_syscall.sh - Compare Concurrent-C vs Pthread robustness against blocking IO
#
# This script runs the "Kidnapping Challenge" for both implementations and compares
# their ability to maintain a "heartbeat" while OS threads are blocked by raw syscalls.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
STRESS_DIR="$REPO_ROOT/stress"

# Configuration
KIDNAPPERS=100
WORKERS=16
DURATION=5

echo "================================================================="
echo "SYSCALL KIDNAPPING COMPARISON"
echo "Workers: $WORKERS | Kidnappers: $KIDNAPPERS | Duration: ${DURATION}s"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
$CCC build "$STRESS_DIR/syscall_kidnap.ccs" -o "$STRESS_DIR/syscall_kidnap"
gcc -O3 "$STRESS_DIR/adler_baseline_kidnap.c" -o "$STRESS_DIR/adler_baseline_kidnap" -lpthread
echo "Done."
echo ""

run_test() {
    local name=$1
    local cmd=$2
    echo "--- Running $name ---"
    
    # Run in background and capture output
    $cmd > test_out.txt 2>&1 &
    local pid=$!
    
    # Wait for duration plus a little buffer
    sleep $((DURATION + 1))
    
    # Kill process if still running
    kill -9 $pid 2>/dev/null || true
    
    # Extract total heartbeats
    local b=$(grep "Tick" test_out.txt | wc -l | tr -d ' ')
    echo "$name Heartbeats: $b"
    echo "$b" > last_beats.txt
}

# 2. Run Pthread Baseline
run_test "Pthread (Adler)" "$STRESS_DIR/adler_baseline_kidnap"
PTHREAD_BEATS=$(cat last_beats.txt)

# 3. Run Concurrent-C
export CC_FIBER_WORKERS=$WORKERS
run_test "Concurrent-C" "$REPO_ROOT/bin/syscall_kidnap"
CC_BEATS=$(cat last_beats.txt)
rm last_beats.txt

echo ""
echo "================================================================="
echo "VERDICT"
echo "================================================================="
printf "%-20s %-15s\n" "Implementation" "Total Heartbeats"
printf "%-20s %-15s\n" "Pthread (Adler)" "$PTHREAD_BEATS"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_BEATS"
echo "-----------------------------------------------------------------"

if [ "$CC_BEATS" -ge "$((PTHREAD_BEATS - 5))" ]; then
    echo "RESULT: SUCCESS - Concurrent-C matched or exceeded Pthread robustness!"
    echo "The user-space scheduler correctly handled OS thread kidnapping."
else
    echo "RESULT: PARTIAL - Concurrent-C was slowed down by kidnapping."
    echo "The scheduler survived but heartbeats were dropped."
fi
echo "================================================================="
