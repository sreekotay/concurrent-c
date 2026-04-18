#!/bin/bash
# compare_syscall.sh - Compare Concurrent-C (V1 + V2) vs Pthread vs Go vs Zig
# on the "Kidnapping Challenge" (blocking-syscall robustness).
#
# Two axes are now tracked:
#   - Heartbeats (liveness): can the scheduler keep a ticker fiber running?
#   - Kidnappers Completed (throughput): how many 2s blocking-sleep tasks
#     actually drained before the test window closed?
#
# The liveness axis alone stopped discriminating — every runtime keeps ticking.
# Throughput exposes whether sysmon is promoting replacement workers when
# existing ones get pinned in blocking syscalls.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
STRESS_DIR="$REPO_ROOT/stress"
GO_SRC="$REPO_ROOT/perf/go/syscall_kidnap.go"
ZIG_SRC="$REPO_ROOT/perf/zig/syscall_kidnap.zig"

KIDNAPPERS=100
WORKERS=16
DURATION=3

# Hard wall-clock cap per run. The test has no warmup: heartbeat and kidnappers
# race for workers from T=0, and the driver loop ends at DURATION. CC variants
# may still be draining kidnappers when cancelled; we kill shortly after.
MAX_WAIT=${MAX_WAIT:-6}

echo "================================================================="
echo "SYSCALL KIDNAPPING COMPARISON"
echo "Workers: $WORKERS | Kidnappers: $KIDNAPPERS | Test window: ${DURATION}s"
echo "Max wall time per run: ${MAX_WAIT}s"
echo "================================================================="
echo ""

echo "Building tests..."
mkdir -p "$STRESS_DIR/out"
$CCC build --release "$STRESS_DIR/syscall_kidnap.ccs" -o "$STRESS_DIR/out/syscall_kidnap" >/dev/null
gcc -O2 "$STRESS_DIR/adler_baseline_kidnap.c" -o "$STRESS_DIR/out/adler_baseline_kidnap" -lpthread
if command -v zig >/dev/null 2>&1; then
    (cd "$STRESS_DIR/out" && zig build-exe "$ZIG_SRC" -O ReleaseFast -lc -femit-bin=syscall_kidnap_zig >/dev/null 2>&1)
fi
echo "Done."
echo ""

# Run a command, cap it at MAX_WAIT seconds, extract heartbeats + kidnappers-done.
# Args: NAME CMD [ENV_PREFIX]
run_test() {
    local name=$1
    local cmd=$2
    local env_prefix=${3:-}
    echo "--- Running $name ---"

    local out=/tmp/kidnap_out_$$.txt
    rm -f "$out"
    eval "$env_prefix $cmd" >"$out" 2>&1 &
    local pid=$!

    local elapsed=0
    while kill -0 $pid 2>/dev/null && [ $elapsed -lt $MAX_WAIT ]; do
        sleep 1
        elapsed=$((elapsed + 1))
    done
    kill -9 $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true

    local b
    b=$(grep -c "\[Heartbeat\] Tick" "$out" || true)
    [ -z "$b" ] && b=0
    local d
    d=$(grep -E "Kidnappers Completed" "$out" | head -1 | grep -oE '[0-9]+' | head -1)
    [ -z "$d" ] && d=0

    echo "  Heartbeats: $b | Kidnappers Completed: $d / $KIDNAPPERS | walltime: ${elapsed}s"
    LAST_BEATS=$b
    LAST_DONE=$d
    rm -f "$out"
}

run_test "Pthread (Adler)" "$STRESS_DIR/out/adler_baseline_kidnap"
PTHREAD_BEATS=$LAST_BEATS;  PTHREAD_DONE=$LAST_DONE

run_test "Concurrent-C" "$STRESS_DIR/out/syscall_kidnap" "CC_V2_THREADS=$WORKERS"
CC_BEATS=$LAST_BEATS;       CC_DONE=$LAST_DONE

GO_BEATS="-"; GO_DONE="-"
if command -v go >/dev/null 2>&1; then
    run_test "Go" "go run $GO_SRC"
    GO_BEATS=$LAST_BEATS;   GO_DONE=$LAST_DONE
fi

ZIG_BEATS="-"; ZIG_DONE="-"
if [ -x "$STRESS_DIR/out/syscall_kidnap_zig" ]; then
    run_test "Zig" "$STRESS_DIR/out/syscall_kidnap_zig"
    ZIG_BEATS=$LAST_BEATS;  ZIG_DONE=$LAST_DONE
fi

echo ""
echo "DATA_PTHREAD_SYSCALL_BEATS: $PTHREAD_BEATS"
echo "DATA_PTHREAD_SYSCALL_DONE:  $PTHREAD_DONE"
echo "DATA_CC_SYSCALL_BEATS:      $CC_BEATS"
echo "DATA_CC_SYSCALL_DONE:       $CC_DONE"
echo "DATA_GO_SYSCALL_BEATS:      $GO_BEATS"
echo "DATA_GO_SYSCALL_DONE:       $GO_DONE"
echo "DATA_ZIG_SYSCALL_BEATS:     $ZIG_BEATS"
echo "DATA_ZIG_SYSCALL_DONE:      $ZIG_DONE"

echo ""
echo "================================================================="
echo "VERDICT"
echo "================================================================="
printf "%-20s %-12s %-22s\n" "Implementation" "Heartbeats" "Kidnappers Completed"
printf "%-20s %-12s %-22s\n" "Pthread (Adler)"    "$PTHREAD_BEATS" "$PTHREAD_DONE / $KIDNAPPERS"
printf "%-20s %-12s %-22s\n" "Concurrent-C"       "$CC_BEATS"      "$CC_DONE / $KIDNAPPERS"
printf "%-20s %-12s %-22s\n" "Go"                 "$GO_BEATS"      "$GO_DONE / $KIDNAPPERS"
printf "%-20s %-12s %-22s\n" "Zig"                "$ZIG_BEATS"     "$ZIG_DONE / $KIDNAPPERS"
echo "-----------------------------------------------------------------"
echo "Liveness: all runtimes should tick ~${DURATION}0 heartbeats."
echo "Throughput: a 1:1 runtime (pthread/go/zig) drains all $KIDNAPPERS kidnappers"
echo "in ~2s; an M:N runtime capped at $WORKERS workers with no sysmon-driven"
echo "worker promotion tops out near (${WORKERS} workers * ${DURATION}s / 2s)"
echo "= $((WORKERS * DURATION / 2)) kidnappers."
echo "================================================================="
