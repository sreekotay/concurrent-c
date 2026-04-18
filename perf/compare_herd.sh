#!/bin/bash
# compare_herd.sh - "Wake one of many" latency comparison
#
# Measures time to wake the 1st of NUM_WAITERS parked consumers.
#
# Two pthread rows are reported:
#   - Pthread (condvar)   : idiomatic C wake-one baseline (pthread_cond_signal)
#   - Pthread (pipe herd) : classic thundering-herd worst case (N readers on pipe)
#
# Concurrent-C uses its channel wake-one path.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

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
$CCC build --release "$SCRIPT_DIR/thundering_herd.ccs" -o "$SCRIPT_DIR/out/thundering_herd"
gcc -O2 "$SCRIPT_DIR/pthread_herd_baseline.c"  -o "$SCRIPT_DIR/out/pthread_herd_baseline"  -lpthread
gcc -O2 "$SCRIPT_DIR/pthread_herd_pipe_herd.c" -o "$SCRIPT_DIR/out/pthread_herd_pipe_herd" -lpthread
echo "Done."
echo ""

# 2. Run Pthread condvar baseline (idiomatic)
echo "--- Running Pthread (condvar, idiomatic) ---"
"$SCRIPT_DIR/out/pthread_herd_baseline" | tee herd_pthread_cond_out.txt
echo ""

# 3. Run Pthread pipe (thundering herd worst case)
echo "--- Running Pthread (pipe, thundering herd) ---"
"$SCRIPT_DIR/out/pthread_herd_pipe_herd" | tee herd_pthread_pipe_out.txt
echo ""

# 4. Run Concurrent-C
echo "--- Running Concurrent-C ---"
"$SCRIPT_DIR/out/thundering_herd" | tee herd_cc_out.txt
echo ""

# 5. Extract average latency
avg_from() {
    grep "Sample" "$1" | sed 's/.*: *//; s/ ms//' | awk '{sum+=$1} END {if (NR>0) printf "%.4f", sum/NR; else print "0"}'
}

PTHREAD_COND_AVG=$(avg_from herd_pthread_cond_out.txt)
PTHREAD_PIPE_AVG=$(avg_from herd_pthread_pipe_out.txt)
CC_AVG=$(avg_from herd_cc_out.txt)

# Primary pthread number for downstream aggregators is the idiomatic condvar one
echo "DATA_PTHREAD_HERD_LATENCY: $PTHREAD_COND_AVG"
echo "DATA_PTHREAD_HERD_PIPE_LATENCY: $PTHREAD_PIPE_AVG"
echo "DATA_CC_HERD_LATENCY: $CC_AVG"

echo "================================================================="
echo "RESULTS — wake-one latency, 1000 parked consumers"
echo "================================================================="
printf "%-28s %-18s %s\n" "Implementation" "Avg Latency (ms)" "Wake primitive"
printf "%-28s %-18s %s\n" "Pthread (condvar)"     "$PTHREAD_COND_AVG" "pthread_cond_signal (exclusive, kernel futex)"
printf "%-28s %-18s %s\n" "Pthread (pipe herd)"   "$PTHREAD_PIPE_AVG" "pipe write (NON-exclusive wait queue -> herd)"
printf "%-28s %-18s %s\n" "Concurrent-C"          "$CC_AVG"           "chan wake-one (user-space, no syscall)"
echo "-----------------------------------------------------------------"
echo "Tradeoffs:"
echo "  - condvar and CC-chan both wake exactly one waiter; condvar pays a"
echo "    syscall round-trip, CC-chan pays scheduler dispatch."
echo "  - pipe row is NOT a competing baseline: it's the textbook herd case,"
echo "    reported to show what a naive 'wake via fd' design costs at N=1000."
echo "================================================================="

rm -f herd_pthread_cond_out.txt herd_pthread_pipe_out.txt herd_cc_out.txt
