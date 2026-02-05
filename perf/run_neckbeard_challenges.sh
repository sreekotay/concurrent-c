#!/bin/bash
# run_neckbeard_challenges.sh - Run all robustness and fairness comparisons
#
# This script executes all the "Neckbeard" comparison tests and summarizes the results.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "================================================================="
echo "CONCURRENT-C: THE NECKBEARD CHALLENGES"
echo "================================================================="
echo "Running all robustness and fairness comparisons..."
echo ""

# 1. Syscall Kidnapping
echo "[1/5] Syscall Kidnapping Challenge..."
"$SCRIPT_DIR/compare_syscall.sh" | grep -E "Implementation|Pthread|Concurrent-C|RESULT"
echo ""

# 2. Thundering Herd
echo "[2/5] Thundering Herd Challenge..."
"$SCRIPT_DIR/compare_herd.sh" | grep -E "Implementation|Pthread|Concurrent-C|RESULT"
echo ""

# 3. Cache-Line Contention
echo "[3/5] Cache-Line Contention Challenge..."
"$SCRIPT_DIR/compare_contention.sh" | grep -E "Implementation|Pthread|Concurrent-C|RESULT"
echo ""

# 4. Noisy Neighbor
echo "[4/5] Noisy Neighbor Challenge..."
"$SCRIPT_DIR/compare_preemption.sh" | grep -E "Implementation|Pthread|Concurrent-C|RESULT"
echo ""

# 5. Arena Contention
echo "[5/5] Arena Contention Challenge..."
"$SCRIPT_DIR/compare_arena.sh" | grep -E "Implementation|Pthread|Concurrent-C|RESULT"
echo ""

echo "================================================================="
echo "ALL CHALLENGES COMPLETED"
echo "================================================================="
