#!/bin/bash
# Run stress tests with sanitizers and/or different compilers
#
# Usage:
#   ./scripts/stress_sanitize.sh              # run with default compiler
#   ./scripts/stress_sanitize.sh tsan         # ThreadSanitizer (clang/gcc only)
#   ./scripts/stress_sanitize.sh asan         # AddressSanitizer (clang/gcc only)
#   ./scripts/stress_sanitize.sh compilers    # test with clang, gcc, tcc
#   ./scripts/stress_sanitize.sh all          # sanitizers + all compilers

set -e
cd "$(dirname "$0")/.."

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

STRESS_TESTS=(
    stress/spawn_storm.ccs
    stress/channel_flood.ccs
    stress/closure_capture_storm.ccs
    stress/nursery_deep.ccs
    stress/pipeline_long.ccs
    stress/fanout_fanin.ccs
    stress/worker_pool_heavy.ccs
    stress/deadline_race.ccs
)

run_with_sanitizer() {
    local san_name="$1"
    local san_flags="$2"
    local failed=0
    local passed=0

    echo -e "${YELLOW}=== Running stress tests with $san_name ===${NC}"
    echo ""

    for test in "${STRESS_TESTS[@]}"; do
        name=$(basename "$test" .ccs)
        printf "  %-30s " "$name"

        # Build and run with sanitizer
        # Note: --no-cache ensures fresh build with sanitizer flags
        output=$(./cc/bin/ccc build run "$test" --cc-flags "$san_flags" --no-cache 2>&1) || true
        exit_code=$?

        # Check for sanitizer errors in output
        if echo "$output" | grep -qiE "ThreadSanitizer|AddressSanitizer|LeakSanitizer|data race|heap-use-after-free|buffer-overflow"; then
            echo -e "${RED}FAIL${NC} (sanitizer error)"
            echo "$output" | grep -iE "SUMMARY|WARNING.*Sanitizer|ERROR" | head -5
            echo ""
            ((failed++))
        elif [ $exit_code -ne 0 ]; then
            echo -e "${RED}FAIL${NC} (exit $exit_code)"
            echo "$output" | tail -3
            echo ""
            ((failed++))
        else
            echo -e "${GREEN}OK${NC}"
            ((passed++))
        fi
    done

    echo ""
    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}$san_name: All $passed tests passed${NC}"
    else
        echo -e "${RED}$san_name: $failed/${#STRESS_TESTS[@]} tests failed${NC}"
    fi
    echo ""

    return $failed
}

run_with_compiler() {
    local cc_name="$1"
    local cc_bin="$2"
    local failed=0
    local passed=0

    # Check if compiler exists
    if [[ "$cc_bin" == "./"* ]]; then
        # Relative path - check file exists
        if [ ! -x "$cc_bin" ]; then
            echo -e "${YELLOW}=== Skipping $cc_name (not found) ===${NC}"
            echo ""
            return 0
        fi
    elif ! command -v "$cc_bin" &> /dev/null; then
        echo -e "${YELLOW}=== Skipping $cc_name (not found) ===${NC}"
        echo ""
        return 0
    fi

    echo -e "${CYAN}=== Running stress tests with $cc_name ===${NC}"
    echo ""

    # TCC doesn't support -MMD/-MF/-MT dependency flags
    local extra_flags=""
    if [[ "$cc_bin" == *"tcc"* ]]; then
        extra_flags="--no-cache"  # Force rebuild, skip dep tracking
    fi

    for test in "${STRESS_TESTS[@]}"; do
        name=$(basename "$test" .ccs)
        printf "  %-30s " "$name"

        # Build and run with specific compiler
        output=$(CC="$cc_bin" CFLAGS="-w" ./cc/bin/ccc build run "$test" --no-cache 2>&1) || true
        exit_code=$?

        # Check exit code and output
        if [ $exit_code -ne 0 ]; then
            echo -e "${RED}FAIL${NC} (exit $exit_code)"
            echo "$output" | tail -3
            echo ""
            ((failed++))
        else
            # Verify test passed (check for expected output)
            if echo "$output" | grep -qE "(expected|completed|received)"; then
                echo -e "${GREEN}OK${NC}"
                ((passed++))
            else
                echo -e "${YELLOW}OK${NC} (no verification)"
                ((passed++))
            fi
        fi
    done

    echo ""
    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}$cc_name: All $passed tests passed${NC}"
    else
        echo -e "${RED}$cc_name: $failed/${#STRESS_TESTS[@]} tests failed${NC}"
    fi
    echo ""

    return $failed
}

mode="${1:-default}"
total_failed=0

case "$mode" in
    tsan|thread)
        run_with_sanitizer "ThreadSanitizer" "-fsanitize=thread" || ((total_failed++))
        ;;
    asan|address)
        run_with_sanitizer "AddressSanitizer" "-fsanitize=address -fno-omit-frame-pointer" || ((total_failed++))
        ;;
    sanitizers)
        # TSan and ASan are mutually exclusive, run separately
        run_with_sanitizer "ThreadSanitizer" "-fsanitize=thread" || ((total_failed++))
        run_with_sanitizer "AddressSanitizer" "-fsanitize=address -fno-omit-frame-pointer" || ((total_failed++))
        ;;
    compilers)
        # Test with different compilers
        run_with_compiler "clang" "clang" || ((total_failed++))
        run_with_compiler "gcc" "gcc" || ((total_failed++))
        # TCC as final compiler: supported on Linux, but NOT on macOS (TCC lacks __thread TLS support on macOS)
        # The ccc compiler now handles TCC-specific flags (-MMD/-MF/-MT, -ffunction-sections, etc.)
        # but the runtime uses __thread for TLS which TCC/macOS doesn't support.
        if [[ "$(uname)" == "Linux" ]]; then
            run_with_compiler "tcc (bundled)" "./third_party/tcc/tcc" || ((total_failed++))
        else
            echo -e "${YELLOW}=== Skipping tcc as final compiler (macOS: TCC lacks __thread TLS support) ===${NC}"
            echo "    Note: TCC preprocessing works; TCC as final compiler only supported on Linux"
            echo ""
        fi
        ;;
    all)
        # Run everything
        run_with_compiler "clang" "clang" || ((total_failed++))
        run_with_compiler "gcc" "gcc" || ((total_failed++))
        run_with_compiler "tcc" "tcc" || ((total_failed++))
        echo ""
        run_with_sanitizer "ThreadSanitizer" "-fsanitize=thread" || ((total_failed++))
        run_with_sanitizer "AddressSanitizer" "-fsanitize=address -fno-omit-frame-pointer" || ((total_failed++))
        ;;
    default|"")
        # Just run with default compiler
        run_with_compiler "default (cc)" "cc" || ((total_failed++))
        ;;
    *)
        echo "Usage: $0 [tsan|asan|sanitizers|compilers|all|default]"
        echo ""
        echo "Modes:"
        echo "  default     Run with system compiler (cc)"
        echo "  tsan        Run with ThreadSanitizer"
        echo "  asan        Run with AddressSanitizer"
        echo "  sanitizers  Run both TSan and ASan"
        echo "  compilers   Run with clang, gcc, tcc"
        echo "  all         Run compilers + sanitizers"
        exit 1
        ;;
esac

if [ $total_failed -gt 0 ]; then
    echo -e "${RED}Some tests failed${NC}"
    exit 1
fi

echo -e "${GREEN}All tests passed${NC}"
