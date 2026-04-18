#!/bin/bash
# compare_herd.sh - "Wake one of many" latency comparison
#
# Measures time to wake the 1st of NUM_WAITERS parked consumers across:
#   - Pthread (condvar)     : idiomatic C wake-one baseline (pthread_cond_signal)
#   - Pthread (pipe herd)   : classic thundering-herd worst case (N readers on pipe)
#   - Concurrent-C          : chan wake-one (user-space, sched_v2)
#   - Go                    : unbuffered chan, goroutines
#   - Zig                   : std.Thread.Condition + Mutex (same idiom as pthread condvar)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
GO_SRC="$SCRIPT_DIR/go/thundering_herd.go"
ZIG_SRC="$SCRIPT_DIR/zig/thundering_herd.zig"

echo "================================================================="
echo "WAKE-ONE OF MANY — idiomatic-primitive comparison"
echo "================================================================="
echo "Goal: compare how each runtime's idiomatic wake-one primitive behaves"
echo "with 1000 parked consumers. Not a scoreboard — a tradeoff map."
echo "================================================================="
echo ""

# 1. Build implementations
echo "Building tests..."
mkdir -p "$SCRIPT_DIR/out"
$CCC build --release "$SCRIPT_DIR/thundering_herd.ccs" -o "$SCRIPT_DIR/out/thundering_herd" >/dev/null
gcc -O2 "$SCRIPT_DIR/pthread_herd_baseline.c"  -o "$SCRIPT_DIR/out/pthread_herd_baseline"  -lpthread
gcc -O2 "$SCRIPT_DIR/pthread_herd_pipe_herd.c" -o "$SCRIPT_DIR/out/pthread_herd_pipe_herd" -lpthread
if command -v zig >/dev/null 2>&1; then
    (cd "$SCRIPT_DIR/out" && zig build-exe "$ZIG_SRC" -O ReleaseFast -lc -femit-bin=thundering_herd_zig >/dev/null 2>&1) || true
fi
echo "Done."
echo ""

# Shared sample-line averager. Tolerates missing files (e.g. zig not installed).
avg_from() {
    [ -f "$1" ] || { echo "-"; return; }
    grep "Sample" "$1" | sed 's/.*: *//; s/ ms//' | \
        awk '{sum+=$1} END {if (NR>0) printf "%.4f", sum/NR; else print "-"}'
}

run_and_tee() {
    local name=$1; local cmd=$2; local out=$3
    echo "--- Running $name ---"
    eval "$cmd" | tee "$out"
    echo ""
}

run_and_tee "Pthread (condvar, idiomatic)" \
    "$SCRIPT_DIR/out/pthread_herd_baseline" \
    herd_pthread_cond_out.txt

run_and_tee "Pthread (pipe, thundering herd)" \
    "$SCRIPT_DIR/out/pthread_herd_pipe_herd" \
    herd_pthread_pipe_out.txt

run_and_tee "Concurrent-C" \
    "$SCRIPT_DIR/out/thundering_herd" \
    herd_cc_out.txt

GO_AVG="-"
if command -v go >/dev/null 2>&1; then
    run_and_tee "Go" "go run $GO_SRC" herd_go_out.txt
    GO_AVG=$(avg_from herd_go_out.txt)
fi

ZIG_AVG="-"
if [ -x "$SCRIPT_DIR/out/thundering_herd_zig" ]; then
    run_and_tee "Zig" "$SCRIPT_DIR/out/thundering_herd_zig" herd_zig_out.txt
    ZIG_AVG=$(avg_from herd_zig_out.txt)
fi

PTHREAD_COND_AVG=$(avg_from herd_pthread_cond_out.txt)
PTHREAD_PIPE_AVG=$(avg_from herd_pthread_pipe_out.txt)
CC_AVG=$(avg_from herd_cc_out.txt)

echo "DATA_PTHREAD_HERD_LATENCY:      $PTHREAD_COND_AVG"
echo "DATA_PTHREAD_HERD_PIPE_LATENCY: $PTHREAD_PIPE_AVG"
echo "DATA_CC_HERD_LATENCY:           $CC_AVG"
echo "DATA_GO_HERD_LATENCY:           $GO_AVG"
echo "DATA_ZIG_HERD_LATENCY:          $ZIG_AVG"

echo "================================================================="
echo "RESULTS — wake-one latency, 1000 parked consumers"
echo "================================================================="
printf "%-28s %-18s %s\n" "Implementation" "Avg Latency (ms)" "Wake primitive"
printf "%-28s %-18s %s\n" "Pthread (condvar)"     "$PTHREAD_COND_AVG" "pthread_cond_signal (exclusive, kernel futex)"
printf "%-28s %-18s %s\n" "Pthread (pipe herd)"   "$PTHREAD_PIPE_AVG" "pipe write (NON-exclusive wait queue -> herd)"
printf "%-28s %-18s %s\n" "Concurrent-C"          "$CC_AVG"           "chan wake-one (user-space, no syscall)"
printf "%-28s %-18s %s\n" "Go"                    "$GO_AVG"           "chan wake-one (Go runtime, sudog queue)"
printf "%-28s %-18s %s\n" "Zig"                   "$ZIG_AVG"          "std.Thread.Condition.signal (pthread_cond equiv.)"
echo "-----------------------------------------------------------------"
echo "Tradeoffs:"
echo "  - condvar, Zig, and CC-chan all wake exactly one waiter; condvar/"
echo "    Zig pay a kernel futex round-trip per wake, CC-chan pays scheduler"
echo "    dispatch in user-space."
echo "  - Go wakes one goroutine via its runtime's sudog queue (no kernel"
echo "    syscall for the wake path itself)."
echo "  - pipe row is NOT a competing baseline: it's the textbook herd case,"
echo "    reported to show what a naive 'wake via fd' design costs at N=1000."
echo "================================================================="

rm -f herd_pthread_cond_out.txt herd_pthread_pipe_out.txt herd_cc_out.txt \
      herd_go_out.txt herd_zig_out.txt
