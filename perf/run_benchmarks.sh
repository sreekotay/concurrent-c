#!/bin/bash
# Performance Benchmark Runner
# Runs all Concurrent-C benchmarks and formats results as a table

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CCC="$PROJECT_ROOT/cc/bin/ccc"

# Check if ccc exists
if [ ! -x "$CCC" ]; then
    echo -e "${RED}Error: $CCC not found or not executable${NC}"
    echo "Make sure you've built the Concurrent-C compiler"
    exit 1
fi

echo -e "${BLUE}Concurrent-C Performance Benchmarks${NC}"
echo "=================================="

# Function to extract metric from benchmark output
extract_metric() {
    local output="$1"
    local pattern="$2"
    echo "$output" | grep "$pattern" | sed 's/.*: //' | sed 's/ (.*//'
}

# Function to run a single benchmark and collect results
run_benchmark() {
    local name="$1"
    local file="$2"
    local desc="$3"

    # Run benchmark and capture output
    local output
    if ! output=$("$CCC" run "$file" 2>&1); then
        echo "$name:FAILED"
        return 1
    fi

    # Extract performance metrics and format as "name:metric"
    echo "$output" | grep -E "^[[:space:]]+[a-zA-Z].*:" | sed 's/^[[:space:]]*//' | while IFS= read -r line; do
        echo "$name:$line"
    done
}

# Function to format number with commas
format_number() {
    printf "%'d" "$1" 2>/dev/null || echo "$1"
}

# Function to extract value and unit from metric line
extract_value_unit() {
    local line="$1"
    # Extract the number before the unit
    local value=$(echo "$line" | sed 's/.*: \([0-9,]*\).*/\1/' | tr -d ',')
    local unit=$(echo "$line" | sed 's/.*: [0-9,]* \([a-zA-Z/]*\/sec\).*/\1/')
    if [ "$unit" = "$line" ]; then
        unit="N/A"
    fi
    echo "$value|$unit"
}

# Main benchmarks to run
BENCHMARKS=(
    "spawn_nursery:perf/spawn_nursery.ccs:Nursery-based spawn throughput"
    "spawn_sequential:perf/spawn_sequential.ccs:Sequential spawn+join throughput"
    "channel_throughput:perf/perf_channel_throughput.ccs:Channel operations throughput"
    "async_overhead:perf/perf_async_overhead.ccs:Async task creation and execution"
    "gobench_blocking:perf/perf_gobench_blocking_pressure.ccs:GoBench-derived blocking pressure"
    "match_select:perf/perf_match_select.ccs:Multi-channel select performance"
    "zero_copy:perf/perf_zero_copy.ccs:Memory copy throughput by payload size"
)

# Collect all results
echo -e "${YELLOW}Running Concurrent-C benchmarks...${NC}"

results_file=$(mktemp)
failed_benchmarks=()

for bench_spec in "${BENCHMARKS[@]}"; do
    IFS=':' read -r name file desc <<< "$bench_spec"
    echo -e "  ${BLUE}$name${NC}"

    bench_output=$(run_benchmark "$name" "$file" "$desc")
    if echo "$bench_output" | grep -q ":FAILED$"; then
        failed_benchmarks+=("$name")
    else
        # Store results in temp file
        echo "$bench_output" >> "$results_file"
    fi
done

# Display results table
echo ""
echo -e "${BLUE}Concurrent-C Performance Benchmarks${NC}"
echo "========================================"

if [ ! -s "$results_file" ]; then
    echo -e "${RED}No benchmark results collected!${NC}"
    rm -f "$results_file"
    exit 1
fi

# Table header
printf "${CYAN}%-20s %-40s ${GREEN}%-15s${NC}\n" "Benchmark" "Metric" "Result"
echo "--------------------------------------------------------------------------------"

# Sort and display results
sort "$results_file" | while IFS= read -r line; do
    IFS=':' read -r bench_name metric_line <<< "$line"

    # Extract value and unit
    IFS='|' read -r value unit <<< "$(extract_value_unit "$metric_line")"

    if [ -n "$value" ]; then
        formatted_value=$(format_number "$value")
        printf "%-20s %-40s ${GREEN}%-15s${NC}\n" "$bench_name" "$(echo "$metric_line" | cut -d: -f1)" "$formatted_value $unit"
    else
        printf "%-20s %-40s ${GREEN}%-15s${NC}\n" "$bench_name" "$metric_line" "N/A"
    fi
done

rm -f "$results_file"

echo ""
if [ ${#failed_benchmarks[@]} -gt 0 ]; then
    echo -e "${RED}Failed benchmarks: ${failed_benchmarks[*]}${NC}"
else
    echo -e "${GREEN}All benchmarks completed successfully!${NC}"
fi