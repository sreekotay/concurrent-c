#!/bin/bash
# run_ccc_cancellation_avalanche_breakdown.sh - isolate CCC waiter-class costs

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
BIN="$SCRIPT_DIR/out/cancellation_avalanche"
AVALANCHE_DEPTH="${CC_AVALANCHE_DEPTH:-2}"
AVALANCHE_WIDTH="${CC_AVALANCHE_WIDTH:-6}"
AVALANCHE_ITERATIONS="${CC_AVALANCHE_ITERATIONS:-5}"
AVALANCHE_CANCEL_DELAY_MS="${CC_AVALANCHE_CANCEL_DELAY_MS:-20}"

echo "================================================================="
echo "CCC CANCELLATION AVALANCHE BREAKDOWN"
echo "Modes: mixed, recv, send, deadline, recv_send, recv_deadline, send_deadline, recv_sharded, recv_deadline_shared"
echo "Depth: $AVALANCHE_DEPTH | Width: $AVALANCHE_WIDTH | Iterations: $AVALANCHE_ITERATIONS | Cancel delay: ${AVALANCHE_CANCEL_DELAY_MS}ms"
echo "================================================================="

"$CCC" build --release "$SCRIPT_DIR/cancellation_avalanche.ccs" -o "$BIN"

run_mode() {
    local mode="$1"
    local out="$SCRIPT_DIR/out/ccc_cancellation_avalanche_${mode}.txt"
    CC_AVALANCHE_MODE="$mode" \
    CC_AVALANCHE_DEPTH="$AVALANCHE_DEPTH" \
    CC_AVALANCHE_WIDTH="$AVALANCHE_WIDTH" \
    CC_AVALANCHE_ITERATIONS="$AVALANCHE_ITERATIONS" \
    CC_AVALANCHE_CANCEL_DELAY_MS="$AVALANCHE_CANCEL_DELAY_MS" \
    "$BIN" > "$out" 2>&1
    awk -v mode="$mode" '
        /^SUMMARY / {
            avg="N/A"; max="N/A"; recv="N/A"; send="N/A"; deadline="N/A";
            for (i = 1; i <= NF; i++) {
                split($i, a, "=");
                if (a[1] == "avg_teardown_ms") avg = a[2];
                else if (a[1] == "max_teardown_ms") max = a[2];
                else if (a[1] == "recv") recv = a[2];
                else if (a[1] == "send") send = a[2];
                else if (a[1] == "deadline") deadline = a[2];
            }
            printf "%-20s %-12s %-12s %-8s %-8s %-8s\n", mode, avg, max, recv, send, deadline;
            exit;
        }
    ' "$out"
}

echo "-----------------------------------------------------------------"
printf "%-20s %-12s %-12s %-8s %-8s %-8s\n" "Mode" "Avg ms" "Max ms" "Recv" "Send" "Deadline"
run_mode mixed
run_mode recv
run_mode send
run_mode deadline
run_mode recv_send
run_mode recv_deadline
run_mode send_deadline
run_mode recv_sharded
run_mode recv_deadline_shared
echo "-----------------------------------------------------------------"
