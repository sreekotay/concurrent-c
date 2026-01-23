#!/bin/bash
# Performance Benchmark Comparison
# Runs both Concurrent-C and Go benchmarks and presents results in a table

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CCC="$PROJECT_ROOT/cc/bin/ccc"
GO_DIR="$SCRIPT_DIR/go"

# Check dependencies
if [ ! -x "$CCC" ]; then
    echo -e "${RED}Error: $CCC not found${NC}"
    exit 1
fi

if ! command -v go &> /dev/null; then
    echo -e "${RED}Error: Go not found${NC}"
    exit 1
fi

echo -e "${BLUE}Concurrent-C vs Go Performance Comparison${NC}"
echo "=========================================="

# Function to extract numeric value from benchmark output
extract_number() {
    local line="$1"
    echo "$line" | sed 's/.*: \([0-9,]*\).*/\1/' | tr -d ','
}

# Function to extract unit from benchmark output
extract_unit() {
    local line="$1"
    echo "$line" | grep -o '[a-zA-Z/]\+/sec' | head -1
}

# Function to run Concurrent-C benchmark and capture results
run_cc_benchmark() {
    local file="$1"
    local output

    if ! output=$("$CCC" run "$file" 2>&1); then
        echo "FAILED" >&2
        return 1
    fi

    # Extract performance metrics (lines starting with spaces and containing ":")
    echo "$output" | grep -E "^[[:space:]]+[a-zA-Z].*:" | sed 's/^[[:space:]]*//'
}

# Function to run Go benchmark and capture results
run_go_benchmark() {
    local file="$1"
    local output

    if ! output=$(cd "$GO_DIR" && go run "$file" 2>&1); then
        echo "FAILED" >&2
        return 1
    fi

    # Extract performance metrics
    echo "$output" | grep -E "^[[:space:]]+[a-zA-Z].*:" | sed 's/^[[:space:]]*//'
}

# Function to format number with commas
format_number() {
    printf "%'d" "$1" 2>/dev/null || echo "$1"
}

# Benchmarks to compare
BENCHMARKS=(
    "spawn_nursery:perf/spawn_nursery.ccs:spawn_nursery.go:Nursery spawn throughput"
    "spawn_sequential:perf/spawn_sequential.ccs:spawn_sequential.go:Sequential spawn+join"
    "channel_throughput:perf/perf_channel_throughput.ccs:channel_throughput.go:Channel operations"
)

echo ""
echo -e "${BLUE}Performance Comparison: Concurrent-C vs Go${NC}"
echo "=========================================="
printf "${CYAN}%-20s ${GREEN}%-18s ${BLUE}%-18s ${YELLOW}%-8s${NC}\n" "Benchmark" "Concurrent-C" "Go" "C/Go"
echo "------------------------------------------------------------------------------------------"

for bench_spec in "${BENCHMARKS[@]}"; do
    IFS=':' read -r name cc_file go_file desc <<< "$bench_spec"

    echo -e "${YELLOW}Running $name...${NC}" >&2

    # Run Concurrent-C benchmark
    cc_results=$(run_cc_benchmark "$cc_file")
    cc_status=$?

    # Run Go benchmark
    go_results=$(run_go_benchmark "$go_file")
    go_status=$?

    if [ $cc_status -ne 0 ] || [ $go_status -ne 0 ]; then
        printf "%-20s ${RED}%-18s ${RED}%-18s ${RED}%-8s${NC}\n" "$name" "FAILED" "FAILED" "N/A"
        continue
    fi

    # Parse results - we need to compare corresponding metrics
    # For now, just compare the first metric from each
    cc_line=$(echo "$cc_results" | head -1)
    go_line=$(echo "$go_results" | head -1)

    cc_value=$(extract_number "$cc_line")
    go_value=$(extract_number "$go_line")
    unit=$(extract_unit "$cc_line")

    if [ -n "$cc_value" ] && [ -n "$go_value" ] && [ "$go_value" -gt 0 ]; then
        ratio=$(echo "scale=2; $cc_value / $go_value" | bc -l 2>/dev/null || echo "0")
        ratio_str=$(printf "%.2f" "$ratio")
    else
        ratio_str="N/A"
    fi

    cc_formatted=$(format_number "$cc_value")
    go_formatted=$(format_number "$go_value")

    printf "%-20s ${GREEN}%-18s ${BLUE}%-18s ${YELLOW}%-8s${NC}\n" \
           "$name" "${cc_formatted} $unit" "${go_formatted} $unit" "$ratio_str"
done

echo ""
echo -e "${GREEN}✓ Comparison completed${NC}"
echo ""
echo "Ratio: Concurrent-C performance relative to Go (higher = better)"
echo "Note: Comparisons use primary metric from each benchmark"