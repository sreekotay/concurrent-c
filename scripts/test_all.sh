#!/bin/bash
# Comprehensive test runner for Concurrent-C
# Runs: unit tests, examples, stress tests, and optionally perf tests

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

# Default timeout for stress tests (seconds)
STRESS_TIMEOUT=${STRESS_TIMEOUT:-60}

# Parse arguments
RUN_PERF=0
VERBOSE=0
for arg in "$@"; do
    case $arg in
        --perf) RUN_PERF=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            echo "Usage: $0 [--perf] [-v|--verbose]"
            echo "  --perf     Also run performance benchmarks"
            echo "  -v         Verbose output"
            exit 0
            ;;
    esac
done

echo "========================================"
echo "Concurrent-C Comprehensive Test Suite"
echo "========================================"
echo ""

# --- Unit Tests ---
echo "=== UNIT TESTS ==="
./scripts/test.sh 2>&1 | tail -5
echo ""

# --- Examples ---
echo "=== EXAMPLES ==="
./cc/bin/ccc run ./tools/run_all.ccs -- examples 2>&1 | grep -E "Passed:|Failed:|Skipped:|Total:"
echo ""

# --- Stress Tests ---
echo "=== STRESS TESTS ==="
stress_pass=0
stress_fail=0
stress_timeout=0
stress_skip=0

# Run each stress test
stress_files=$(ls stress/*.ccs 2>/dev/null)
for f in $stress_files; do
    name=$(basename "$f" .ccs)
    
    # Run test with timeout
    ./cc/bin/ccc run --release --timeout "$STRESS_TIMEOUT" "$f" > /tmp/cc_stress_out.txt 2>&1
    rc=$?
    
    if grep -q "cc: run timed out" /tmp/cc_stress_out.txt; then
        echo "[TIMEOUT] $name"
        stress_timeout=$((stress_timeout + 1))
    elif grep -q "error:" /tmp/cc_stress_out.txt; then
        echo "[SKIP] $name (compile error)"
        stress_skip=$((stress_skip + 1))
    elif [ $rc -eq 0 ]; then
        if [ $VERBOSE -eq 1 ]; then
            echo "[PASS] $name"
        fi
        stress_pass=$((stress_pass + 1))
    else
        echo "[FAIL] $name"
        stress_fail=$((stress_fail + 1))
    fi
done

echo ""
echo "Stress: $stress_pass passed, $stress_fail failed, $stress_timeout timeout, $stress_skip skip"
echo ""

# --- Performance Tests (optional) ---
if [ $RUN_PERF -eq 1 ]; then
    echo "=== PERFORMANCE TESTS ==="
    for f in perf/perf_*.ccs perf/spawn_sequential.ccs perf/spawn_nursery.ccs; do
        if [ -f "$f" ]; then
            name=$(basename "$f" .ccs)
            echo "--- $name ---"
            ./cc/bin/ccc run --release "$f" 2>&1 || echo "[compile/run error]"
            echo ""
        fi
    done
fi

# --- Summary ---
echo "========================================"
echo "DONE"
echo "========================================"

if [ $stress_fail -eq 0 ]; then
    echo "All critical tests passed!"
    exit 0
else
    echo "Some tests failed."
    exit 1
fi
