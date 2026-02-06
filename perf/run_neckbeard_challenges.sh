#!/bin/bash
# run_neckbeard_challenges.sh - Run all robustness and fairness comparisons
#
# This script executes all the "Neckbeard" comparison tests and summarizes the results.
# Now includes Go benchmarks for the ultimate M:N showdown.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "================================================================="
echo "CONCURRENT-C: THE NECKBEARD CHALLENGES"
echo "================================================================="
echo "Running all robustness and fairness comparisons..."
echo ""

# Helper to extract data from output files
extract() {
    grep "$1" "$2" | tail -n 1 | awk '{print $2}'
}

# 1. Syscall Kidnapping
echo "[1/5] Syscall Kidnapping Challenge..."
"$SCRIPT_DIR/compare_syscall.sh" > cc_syscall_out.txt 2>&1 || true
go run "$SCRIPT_DIR/go/syscall_kidnap.go" > go_syscall_out.txt 2>&1 || true

CC_SYSCALL_BEATS=$(extract "DATA_CC_SYSCALL_BEATS" cc_syscall_out.txt)
PTHREAD_SYSCALL_BEATS=$(extract "DATA_PTHREAD_SYSCALL_BEATS" cc_syscall_out.txt)
GO_SYSCALL_BEATS=$(grep "Total Heartbeats" go_syscall_out.txt | awk '{print $3}')

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Heartbeats"
printf "%-20s %-15s\n" "Pthread" "$PTHREAD_SYSCALL_BEATS"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_SYSCALL_BEATS"
printf "%-20s %-15s\n" "Go" "$GO_SYSCALL_BEATS"
echo "-----------------------------------------------------------------"
echo ""

# 2. Thundering Herd
echo "[2/5] Thundering Herd Challenge..."
"$SCRIPT_DIR/compare_herd.sh" > cc_herd_out.txt 2>&1 || true
go run "$SCRIPT_DIR/go/thundering_herd.go" > go_herd_out.txt 2>&1 || true

CC_HERD_LATENCY=$(extract "DATA_CC_HERD_LATENCY" cc_herd_out.txt)
PTHREAD_HERD_LATENCY=$(extract "DATA_PTHREAD_HERD_LATENCY" cc_herd_out.txt)
GO_HERD_LATENCY=$(grep "Sample" go_herd_out.txt | awk '{sum+=$7} END {if (NR>0) printf "%.4f", sum/NR; else print "0"}')

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Avg Latency (ms)"
printf "%-20s %-15s\n" "Pthread" "$PTHREAD_HERD_LATENCY"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_HERD_LATENCY"
printf "%-20s %-15s\n" "Go" "$GO_HERD_LATENCY"
echo "-----------------------------------------------------------------"
echo ""

# 3. Cache-Line Contention
echo "[3/5] Cache-Line Contention Challenge..."
"$SCRIPT_DIR/compare_contention.sh" > cc_contention_out.txt 2>&1 || true
go run "$SCRIPT_DIR/go/channel_contention.go" > go_contention_out.txt 2>&1 || true

CC_CONT_DROP=$(extract "DATA_CC_CONT_DROP" cc_contention_out.txt)
PTHREAD_CONT_DROP=$(extract "DATA_PTHREAD_CONT_DROP" cc_contention_out.txt)
GO_CONT_DROP=$(grep "Throughput Drop" go_contention_out.txt | tail -n 1 | awk '{print $3}')

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Throughput Drop"
printf "%-20s %-15s\n" "Pthread" "${PTHREAD_CONT_DROP}%"
printf "%-20s %-15s\n" "Concurrent-C" "${CC_CONT_DROP}%"
printf "%-20s %-15s\n" "Go" "$GO_CONT_DROP"
echo "-----------------------------------------------------------------"
echo ""

# 4. Noisy Neighbor
echo "[4/5] Noisy Neighbor Challenge..."
"$SCRIPT_DIR/compare_preemption.sh" > cc_preemption_out.txt 2>&1 || true
go run "$SCRIPT_DIR/go/noisy_neighbor.go" > go_preemption_out.txt 2>&1 || true

CC_PRE_BEATS=$(extract "DATA_CC_PRE_BEATS" cc_preemption_out.txt)
PTHREAD_PRE_BEATS=$(extract "DATA_PTHREAD_PRE_BEATS" cc_preemption_out.txt)
GO_PRE_BEATS=$(grep "Total Heartbeats" go_preemption_out.txt | awk '{print $3}')

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Heartbeats"
printf "%-20s %-15s\n" "Pthread" "$PTHREAD_PRE_BEATS"
printf "%-20s %-15s\n" "Concurrent-C" "$CC_PRE_BEATS"
printf "%-20s %-15s\n" "Go" "$GO_PRE_BEATS"
echo "-----------------------------------------------------------------"
echo ""

# 5. Arena Contention
echo "[5/5] Arena Contention Challenge..."
"$SCRIPT_DIR/compare_arena.sh" > cc_arena_out.txt 2>&1 || true
go run "$SCRIPT_DIR/go/arena_contention.go" > go_arena_out.txt 2>&1 || true

CC_ARENA_TP=$(extract "DATA_CC_ARENA_TP" cc_arena_out.txt)
PTHREAD_ARENA_TP=$(extract "DATA_PTHREAD_ARENA_TP" cc_arena_out.txt)
GO_ARENA_TP=$(grep "Throughput" go_arena_out.txt | awk '{print $2}')

echo "-----------------------------------------------------------------"
printf "%-20s %-20s\n" "Implementation" "Throughput (M/sec)"
printf "%-20s %-20s\n" "Pthread (Malloc)" "$PTHREAD_ARENA_TP"
printf "%-20s %-20s\n" "Concurrent-C (Arena)" "$CC_ARENA_TP"
printf "%-20s %-20s\n" "Go (Heap)" "$GO_ARENA_TP"
echo "-----------------------------------------------------------------"
echo ""

echo "================================================================="
echo "ALL CHALLENGES COMPLETED"
echo "================================================================="

rm -f cc_syscall_out.txt go_syscall_out.txt cc_herd_out.txt go_herd_out.txt \
      cc_contention_out.txt go_contention_out.txt cc_preemption_out.txt \
      go_preemption_out.txt cc_arena_out.txt go_arena_out.txt
