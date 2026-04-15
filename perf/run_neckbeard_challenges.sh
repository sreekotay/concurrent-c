#!/bin/bash
# run_neckbeard_challenges.sh - Run all robustness and fairness comparisons
#
# This script executes all the "Neckbeard" comparison tests and summarizes the results.
# Now includes Go and Zig benchmarks for the ultimate M:N showdown.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

mkdir -p "$SCRIPT_DIR/out"

echo "================================================================="
echo "CONCURRENT-C: THE NECKBEARD CHALLENGES"
echo "================================================================="
echo "Running all robustness and fairness comparisons..."
echo ""

SKIP_CC=0
SKIP_GO=0
SKIP_ZIG=0

if ! command -v "$CCC" &>/dev/null && [ ! -x "$CCC" ]; then
    echo "WARNING: CCC compiler not found at $CCC"
    echo "         CC and Pthread tests will be skipped."
    echo "         Build with: make -C $REPO_ROOT"
    echo ""
    SKIP_CC=1
fi

if ! command -v go &>/dev/null; then
    echo "WARNING: Go not found on PATH"
    echo "         Go tests will be skipped."
    echo ""
    SKIP_GO=1
fi

if ! command -v zig &>/dev/null; then
    echo "WARNING: Zig not found on PATH"
    echo "         Zig tests will be skipped."
    echo ""
    SKIP_ZIG=1
fi

val_or_na() {
    if [ -n "$1" ]; then echo "$1"; else echo "N/A"; fi
}

extract() {
    grep "$1" "$2" 2>/dev/null | tail -n 1 | awk '{print $2}'
}

build_zig() {
    local src=$1
    local out=$2
    zig build-exe "$src" -O ReleaseFast -lc -femit-bin="$out" >/dev/null
}

# 1. Syscall Kidnapping
echo "[1/5] Syscall Kidnapping Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_syscall.sh" > cc_syscall_out.txt 2>&1; then
        CC_SYSCALL_BEATS=$(extract "DATA_CC_SYSCALL_BEATS" cc_syscall_out.txt)
        PTHREAD_SYSCALL_BEATS=$(extract "DATA_PTHREAD_SYSCALL_BEATS" cc_syscall_out.txt)
    else
        echo "  [WARN] compare_syscall.sh failed (exit $?). Check cc_syscall_out.txt for details."
    fi
fi
if [ "$SKIP_GO" -eq 0 ]; then
    if go run "$SCRIPT_DIR/go/syscall_kidnap.go" > go_syscall_out.txt 2>&1; then
        GO_SYSCALL_BEATS=$(grep "Total Heartbeats" go_syscall_out.txt | awk '{print $3}')
    else
        echo "  [WARN] Go syscall_kidnap failed (exit $?)."
    fi
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    if build_zig "$SCRIPT_DIR/zig/syscall_kidnap.zig" "$SCRIPT_DIR/out/zig_syscall_kidnap" &&
       "$SCRIPT_DIR/out/zig_syscall_kidnap" > zig_syscall_out.txt 2>&1; then
        ZIG_SYSCALL_BEATS=$(grep "Total Heartbeats" zig_syscall_out.txt | awk '{print $3}')
    else
        echo "  [WARN] Zig syscall_kidnap failed (exit $?)."
    fi
fi

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Heartbeats"
printf "%-20s %-15s\n" "Pthread" "$(val_or_na "$PTHREAD_SYSCALL_BEATS")"
printf "%-20s %-15s\n" "Concurrent-C" "$(val_or_na "$CC_SYSCALL_BEATS")"
printf "%-20s %-15s\n" "Go" "$(val_or_na "$GO_SYSCALL_BEATS")"
printf "%-20s %-15s\n" "Zig" "$(val_or_na "$ZIG_SYSCALL_BEATS")"
echo "-----------------------------------------------------------------"
echo ""

# 2. Thundering Herd
echo "[2/5] Thundering Herd Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_herd.sh" > cc_herd_out.txt 2>&1; then
        CC_HERD_LATENCY=$(extract "DATA_CC_HERD_LATENCY" cc_herd_out.txt)
        PTHREAD_HERD_LATENCY=$(extract "DATA_PTHREAD_HERD_LATENCY" cc_herd_out.txt)
    else
        echo "  [WARN] compare_herd.sh failed (exit $?). Check cc_herd_out.txt for details."
    fi
fi
if [ "$SKIP_GO" -eq 0 ]; then
    if go run "$SCRIPT_DIR/go/thundering_herd.go" > go_herd_out.txt 2>&1; then
        GO_HERD_LATENCY=$(grep "Sample" go_herd_out.txt | awk '{sum+=$8} END {if (NR>0) printf "%.4f", sum/NR; else print "0"}')
    else
        echo "  [WARN] Go thundering_herd failed (exit $?)."
    fi
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    if build_zig "$SCRIPT_DIR/zig/thundering_herd.zig" "$SCRIPT_DIR/out/zig_thundering_herd" &&
       "$SCRIPT_DIR/out/zig_thundering_herd" > zig_herd_out.txt 2>&1; then
        ZIG_HERD_LATENCY=$(grep "Sample" zig_herd_out.txt | awk '{sum+=$8} END {if (NR>0) printf "%.4f", sum/NR; else print "0"}')
    else
        echo "  [WARN] Zig thundering_herd failed (exit $?)."
    fi
fi

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Avg Latency (ms)"
printf "%-20s %-15s\n" "Pthread" "$(val_or_na "$PTHREAD_HERD_LATENCY")"
printf "%-20s %-15s\n" "Concurrent-C" "$(val_or_na "$CC_HERD_LATENCY")"
printf "%-20s %-15s\n" "Go" "$(val_or_na "$GO_HERD_LATENCY")"
printf "%-20s %-15s\n" "Zig" "$(val_or_na "$ZIG_HERD_LATENCY")"
echo "-----------------------------------------------------------------"
echo ""

# 3. Channel Isolation
echo "[3/5] Channel Isolation Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_contention_stability.sh" 5 > cc_contention_out.txt 2>&1; then
        :
    else
        echo "  [WARN] compare_contention_stability.sh failed (exit $?). Check cc_contention_out.txt for details."
    fi
fi

if [ -f cc_contention_out.txt ]; then
    sed -n '/^min \/ mean \/ max/,$p' cc_contention_out.txt
else
    echo "-----------------------------------------------------------------"
    echo "Channel isolation stability output unavailable."
    echo "-----------------------------------------------------------------"
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    if build_zig "$SCRIPT_DIR/zig/channel_contention.zig" "$SCRIPT_DIR/out/zig_channel_contention" &&
       "$SCRIPT_DIR/out/zig_channel_contention" > zig_contention_out.txt 2>&1; then
        echo "-----------------------------------------------------------------"
        echo "Zig channel isolation summary:"
        grep -E "Best baseline:|Best contention:|Interference:" zig_contention_out.txt || true
        echo "-----------------------------------------------------------------"
    else
        echo "  [WARN] Zig channel_contention failed (exit $?)."
    fi
fi
echo ""

# 4. Noisy Neighbor
echo "[4/5] Noisy Neighbor Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_preemption.sh" > cc_preemption_out.txt 2>&1; then
        CC_PRE_BEATS=$(extract "DATA_CC_PRE_BEATS" cc_preemption_out.txt)
        PTHREAD_PRE_BEATS=$(extract "DATA_PTHREAD_PRE_BEATS" cc_preemption_out.txt)
    else
        echo "  [WARN] compare_preemption.sh failed (exit $?). Check cc_preemption_out.txt for details."
    fi
fi
if [ "$SKIP_GO" -eq 0 ]; then
    if go run "$SCRIPT_DIR/go/noisy_neighbor.go" > go_preemption_out.txt 2>&1; then
        GO_PRE_BEATS=$(grep "Total Heartbeats" go_preemption_out.txt | awk '{print $3}')
    else
        echo "  [WARN] Go noisy_neighbor failed (exit $?)."
    fi
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    if build_zig "$SCRIPT_DIR/zig/noisy_neighbor.zig" "$SCRIPT_DIR/out/zig_noisy_neighbor" &&
       "$SCRIPT_DIR/out/zig_noisy_neighbor" > zig_preemption_out.txt 2>&1; then
        ZIG_PRE_BEATS=$(grep "Total Heartbeats" zig_preemption_out.txt | awk '{print $3}')
    else
        echo "  [WARN] Zig noisy_neighbor failed (exit $?)."
    fi
fi

echo "-----------------------------------------------------------------"
printf "%-20s %-15s\n" "Implementation" "Heartbeats"
printf "%-20s %-15s\n" "Pthread" "$(val_or_na "$PTHREAD_PRE_BEATS")"
printf "%-20s %-15s\n" "Concurrent-C" "$(val_or_na "$CC_PRE_BEATS")"
printf "%-20s %-15s\n" "Go" "$(val_or_na "$GO_PRE_BEATS")"
printf "%-20s %-15s\n" "Zig" "$(val_or_na "$ZIG_PRE_BEATS")"
echo "-----------------------------------------------------------------"
echo ""

# 5. Arena Contention
echo "[5/5] Arena Contention Challenge..."
if [ "$SKIP_CC" -eq 0 ]; then
    if "$SCRIPT_DIR/compare_arena.sh" > cc_arena_out.txt 2>&1; then
        CC_ARENA_TP=$(extract "DATA_CC_ARENA_TP" cc_arena_out.txt)
        PTHREAD_ARENA_TP=$(extract "DATA_PTHREAD_ARENA_TP" cc_arena_out.txt)
    else
        echo "  [WARN] compare_arena.sh failed (exit $?). Check cc_arena_out.txt for details."
    fi
fi
if [ "$SKIP_GO" -eq 0 ]; then
    if go run "$SCRIPT_DIR/go/arena_contention.go" > go_arena_out.txt 2>&1; then
        GO_ARENA_TP=$(grep "Throughput" go_arena_out.txt | awk '{print $2}')
    else
        echo "  [WARN] Go arena_contention failed (exit $?)."
    fi
fi
if [ "$SKIP_ZIG" -eq 0 ]; then
    if build_zig "$SCRIPT_DIR/zig/arena_contention.zig" "$SCRIPT_DIR/out/zig_arena_contention" &&
       "$SCRIPT_DIR/out/zig_arena_contention" > zig_arena_out.txt 2>&1; then
        ZIG_ARENA_TP=$(grep "Throughput" zig_arena_out.txt | awk '{print $2}')
    else
        echo "  [WARN] Zig arena_contention failed (exit $?)."
    fi
fi

echo "-----------------------------------------------------------------"
printf "%-20s %-20s\n" "Implementation" "Throughput (M/sec)"
printf "%-20s %-20s\n" "Pthread (Arena)" "$(val_or_na "$PTHREAD_ARENA_TP")"
printf "%-20s %-20s\n" "Concurrent-C (Arena)" "$(val_or_na "$CC_ARENA_TP")"
printf "%-20s %-20s\n" "Go (mcache)" "$(val_or_na "$GO_ARENA_TP")"
printf "%-20s %-20s\n" "Zig" "$(val_or_na "$ZIG_ARENA_TP")"
echo "-----------------------------------------------------------------"
echo ""

echo "================================================================="
if [ "$SKIP_CC" -eq 1 ]; then
    echo "COMPLETED (Go only — CCC compiler not found)"
elif [ "$SKIP_GO" -eq 1 ]; then
    echo "COMPLETED (CC/Pthread only — Go not found)"
else
    echo "ALL CHALLENGES COMPLETED"
fi
echo "================================================================="

rm -f cc_syscall_out.txt go_syscall_out.txt cc_herd_out.txt go_herd_out.txt \
      cc_contention_out.txt zig_contention_out.txt cc_preemption_out.txt \
      go_preemption_out.txt zig_preemption_out.txt cc_arena_out.txt go_arena_out.txt \
      zig_syscall_out.txt zig_herd_out.txt zig_arena_out.txt
