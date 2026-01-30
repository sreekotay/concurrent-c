#!/bin/bash
# Run tests with ThreadSanitizer to detect data races
#
# Usage:
#   ./scripts/test_tsan.sh              # Run quick TSan tests
#   ./scripts/test_tsan.sh --all        # Run all tests with TSan

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

run_test() {
    local test_file="$1"
    local name
    name=$(basename "$test_file" .ccs)
    
    printf "  %-40s " "$name"
    
    # Build and run with TSan
    local output
    output=$(CC=clang CFLAGS="-fsanitize=thread -g" \
        ./cc/bin/ccc run "$test_file" --no-cache 2>&1) || true
    local exit_code=$?
    
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
    tests/fiber_join_waiter_race.ccs
    tests/nursery_multi_join_race.ccs
    tests/join_done_before_wait_smoke.ccs
    stress/nested_nursery_race.ccs
    stress/nested_nursery_deep.ccs
    stress/work_stealing_race.ccs
    stress/fanout_join_race.ccs
    stress/fiber_spawn_join_tight.ccs
)

failed=0
passed=0

mode="${1:-quick}"

echo -e "${YELLOW}=== ThreadSanitizer Tests ===${NC}"
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
