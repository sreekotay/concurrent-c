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
# Oversubscribed: 15 hogs on 4 workers for M:N userspace runtimes (CC).
# 1:1 pthread baseline runs 16 native OS threads (the kernel does its job).
# Hard 3s wall clock, no warmup — matches Challenge 1 discipline.
HOGS=15
WORKERS=4
DURATION=3

echo "================================================================="
echo "NOISY NEIGHBOR COMPARISON (PREEMPTION TEST)"
echo "Userspace workers: $WORKERS | CPU Hogs: $HOGS | Duration: ${DURATION}s"
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
mkdir -p "$STRESS_DIR/out" "$SCRIPT_DIR/out"
$CCC build --release "$STRESS_DIR/noisy_neighbor.ccs" -o "$STRESS_DIR/out/noisy_neighbor"
gcc -O2 "$STRESS_DIR/pthread_noisy_baseline.c" -o "$STRESS_DIR/out/pthread_noisy_baseline" -lpthread
if command -v zig &>/dev/null; then
    zig build-exe "$SCRIPT_DIR/zig/noisy_neighbor.zig" -O ReleaseFast -lc \
        -femit-bin="$SCRIPT_DIR/out/zig_noisy_neighbor" >/dev/null
    HAVE_ZIG=1
else
    HAVE_ZIG=0
fi
HAVE_GO=0
if command -v go &>/dev/null; then
    (cd "$SCRIPT_DIR/go" && go build -o "$SCRIPT_DIR/out/go_noisy_neighbor" noisy_neighbor.go)
    HAVE_GO=1
fi
echo "Done."
echo ""

# Binaries self-exit at DURATION seconds; harness SIGKILLs as a safety net,
# and samples `ps -M $pid` every 100ms to track peak OS-thread count.
run_test() {
    local name=$1
    local cmd=$2
    echo "--- Running $name ---"

    $cmd > test_out.txt 2>&1 &
    local pid=$!
    local peak=1
    local waited=0
    local limit=$(((DURATION + 1) * 10))
    while [ $waited -lt $limit ]; do
        if ! kill -0 $pid 2>/dev/null; then break; fi
        local n
        n=$(ps -M $pid 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')
        [ -n "$n" ] && [ "$n" -gt "$peak" ] && peak=$n
        sleep 0.1
        waited=$((waited + 1))
    done
    kill -9 $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    local b
    b=$(grep "Tick" test_out.txt | wc -l | tr -d ' ')
    echo "$name Heartbeats: $b  Peak threads: $peak"
    echo "$b $peak" > last_beats.txt
}

# 2. Run Pthread Baseline (16 OS threads; kernel does the scheduling)
run_test "Pthread (Baseline)" "$STRESS_DIR/out/pthread_noisy_baseline"
read PTHREAD_BEATS PTHREAD_PEAK < last_beats.txt
echo ""

# 3. Run Concurrent-C (oversubscribed workers — V2 via spawnhybrid)
export CC_FIBER_WORKERS=$WORKERS
export CC_V2_THREADS=$WORKERS
run_test "Concurrent-C (${WORKERS}w)" "$STRESS_DIR/out/noisy_neighbor"
read CC_BEATS CC_PEAK < last_beats.txt
unset CC_FIBER_WORKERS
unset CC_V2_THREADS
echo ""

# 4. Run Go (GOMAXPROCS=4, async preemption since Go 1.14)
GO_BEATS=""; GO_PEAK=""
if [ "$HAVE_GO" -eq 1 ]; then
    GOMAXPROCS=$WORKERS run_test "Go (${WORKERS}P)" "$SCRIPT_DIR/out/go_noisy_neighbor"
    read GO_BEATS GO_PEAK < last_beats.txt
    echo ""
fi

# 5. Run Zig (OS threads; kernel does the scheduling)
ZIG_BEATS=""; ZIG_PEAK=""
if [ "$HAVE_ZIG" -eq 1 ]; then
    run_test "Zig" "$SCRIPT_DIR/out/zig_noisy_neighbor"
    read ZIG_BEATS ZIG_PEAK < last_beats.txt
    echo ""
fi

rm -f last_beats.txt test_out.txt

echo "DATA_PTHREAD_PRE_BEATS: $PTHREAD_BEATS"
echo "DATA_PTHREAD_PRE_PEAK: $PTHREAD_PEAK"
echo "DATA_CC_PRE_BEATS: $CC_BEATS"
echo "DATA_CC_PRE_PEAK: $CC_PEAK"
[ -n "$GO_BEATS" ] && echo "DATA_GO_PRE_BEATS: $GO_BEATS"
[ -n "$GO_BEATS" ] && echo "DATA_GO_PRE_PEAK: $GO_PEAK"
[ -n "$ZIG_BEATS" ] && echo "DATA_ZIG_PRE_BEATS: $ZIG_BEATS"
[ -n "$ZIG_BEATS" ] && echo "DATA_ZIG_PRE_PEAK: $ZIG_PEAK"

EXPECTED=$((DURATION * 10))
echo ""
echo "================================================================="
echo "VERDICT  (expected ${EXPECTED} heartbeats; ${DURATION}s @ 100ms)"
echo "================================================================="
printf "%-22s %-12s %-14s\n" "Implementation" "Heartbeats" "Peak Threads"
printf "%-22s %-12s %-14s\n" "Pthread (1:1)"         "$PTHREAD_BEATS"  "$PTHREAD_PEAK"
printf "%-22s %-12s %-14s\n" "Concurrent-C (${WORKERS}w)" "$CC_BEATS"       "$CC_PEAK"
[ -n "$GO_BEATS" ]  && printf "%-22s %-12s %-14s\n" "Go (${WORKERS}P)"      "$GO_BEATS"       "$GO_PEAK"
[ -n "$ZIG_BEATS" ] && printf "%-22s %-12s %-14s\n" "Zig (1:1)"         "$ZIG_BEATS"      "$ZIG_PEAK"
echo "================================================================="
