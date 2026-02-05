#!/bin/bash
# Run tests with ThreadSanitizer to detect data races
#
# Usage:
#   ./scripts/test_tsan.sh                    # Run quick TSan tests
#   ./scripts/test_tsan.sh --all              # Run all tests with TSan
#   ./scripts/test_tsan.sh --timeout 10s      # Run with 10s timeout per test
#   ./scripts/test_tsan.sh --all --timeout 10s # Run all tests with 10s timeout

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Check for clang (required for TSan on macOS)
if ! command -v clang &> /dev/null; then
    echo -e "${RED}Error: clang not found (required for ThreadSanitizer)${NC}"
    exit 1
fi

# Helper function to run command with timeout (works on macOS)
run_with_timeout() {
    local timeout_sec="$1"
    shift
    local cmd=("$@")
    
    # Try to use timeout command (gtimeout on macOS if coreutils installed)
    if command -v timeout &> /dev/null; then
        timeout "$timeout_sec" "${cmd[@]}" 2>&1
    elif command -v gtimeout &> /dev/null; then
        gtimeout "$timeout_sec" "${cmd[@]}" 2>&1
    else
        # Fallback: use background process with kill
        local pid
        local tmpfile=$(mktemp)
        ("${cmd[@]}" > "$tmpfile" 2>&1) &
        pid=$!
        (
            sleep "$timeout_sec"
            if kill -0 $pid 2>/dev/null; then
                kill $pid 2>/dev/null || true
                echo "timeout: command terminated after ${timeout_sec}s" >> "$tmpfile"
            fi
        ) &
        local killer_pid=$!
        wait $pid 2>/dev/null
        local exit_code=$?
        kill $killer_pid 2>/dev/null || true
        cat "$tmpfile"
        rm -f "$tmpfile"
        return $exit_code
    fi
}

run_test() {
    local test_file="$1"
    local name
    local output
    local exit_code
    if [[ "$test_file" == *.ccs ]]; then
        name=$(basename "$test_file" .ccs)
    else
        name=$(basename "$test_file" .c)
    fi
    
    printf "  %-40s " "$name"
    
    if [[ "$test_file" == *.c ]]; then
        local bin="/tmp/tsan_${name}"
        # Compile first (no timeout needed)
        if ! output=$(clang -fsanitize=thread -g -Icc/include "$test_file" -lpthread -o "$bin" 2>&1); then
            echo -e "${RED}COMPILE FAIL${NC}"
            echo "$output"
            rm -f "$bin"
            return 1
        fi
        
        # Run with timeout if specified
        if [ -n "$TIMEOUT" ]; then
            output=$(run_with_timeout "$TIMEOUT" "$bin" 2>&1) || true
            exit_code=$?
        else
            output=$("$bin" 2>&1) || true
            exit_code=$?
        fi
        rm -f "$bin"
    else
        # Run with timeout if specified
        if [ -n "$TIMEOUT" ]; then
            output=$(run_with_timeout "$TIMEOUT" bash -c "CC=clang CFLAGS=\"-fsanitize=thread -g\" ./cc/bin/ccc run \"$test_file\" --no-cache 2>&1") || true
            exit_code=$?
        else
            output=$(CC=clang CFLAGS="-fsanitize=thread -g" \
                ./cc/bin/ccc run "$test_file" --no-cache 2>&1) || true
            exit_code=$?
        fi
    fi
    
    # Check for timeout
    if echo "$output" | grep -qE "(timeout|Terminated|killed)"; then
        echo -e "${RED}TIMEOUT${NC}"
        echo "$output" | tail -5
        return 1
    fi
    
    # Check for TSan errors
    if echo "$output" | grep -qE "ThreadSanitizer.*data race"; then
        echo -e "${RED}RACE${NC}"
        echo "$output" | grep -A5 "WARNING: ThreadSanitizer" | head -20
        echo ""
        return 1
    elif [ $exit_code -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (exit $exit_code)"
        echo "$output" | tail -5
        return 1
    else
        echo -e "${GREEN}OK${NC}"
        return 0
    fi
}

# TSan-focused tests
TSAN_TESTS=(
    tests/tsan_closure_capture_smoke.ccs
    tests/tsan_closure_make_stress.c
    tests/fiber_join_waiter_race.ccs
    tests/nursery_multi_join_race.ccs
    tests/join_done_before_wait_smoke.ccs
    stress/nested_nursery_race.ccs
    stress/nested_nursery_deep.ccs
    stress/work_stealing_race.ccs
    stress/fanout_join_race.ccs
    stress/fiber_spawn_join_tight.ccs
    stress/join_handoff_storm.ccs
    stress/park_unpark_storm.ccs
    stress/inbox_cross_worker_storm.ccs
)

failed=0
passed=0

mode="quick"
TIMEOUT=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --all)
            mode="--all"
            shift
            ;;
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo -e "${YELLOW}=== ThreadSanitizer Tests ===${NC}"
if [ -n "$TIMEOUT" ]; then
    echo -e "${YELLOW}Timeout: ${TIMEOUT} per test${NC}"
fi
echo ""

for test in "${TSAN_TESTS[@]}"; do
    if [ -f "$test" ]; then
        if run_test "$test"; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1))
        fi
    fi
done

if [ "$mode" = "--all" ]; then
    echo ""
    echo "--- Additional smoke tests ---"
    
    for test in tests/nursery_*_smoke.ccs tests/closure_spawn_smoke.ccs tests/channel_select_smoke.ccs; do
        if [ -f "$test" ]; then
            if run_test "$test"; then
                passed=$((passed + 1))
            else
                failed=$((failed + 1))
            fi
        fi
    done
fi

echo ""
if [ $failed -eq 0 ]; then
    echo -e "${GREEN}TSan: All $passed tests passed (no data races detected)${NC}"
    exit 0
else
    echo -e "${RED}TSan: $failed tests with data races, $passed passed${NC}"
    exit 1
fi
