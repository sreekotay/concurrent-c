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

# Function to run a single benchmark
run_benchmark() {
    local name="$1"
    local file="$2"
    local desc="$3"

    echo -e "${YELLOW}Running $name...${NC}"

    # Run benchmark and capture output
    local output
    if ! output=$("$CCC" run "$file" 2>&1); then
        echo -e "${RED}FAILED${NC}"
        return 1
    fi

    # Extract and display the performance metrics
    echo "$output" | grep -E "^[[:space:]]+[a-zA-Z].*:" | sed 's/^[[:space:]]*//'
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

# Run all benchmarks
for bench_spec in "${BENCHMARKS[@]}"; do
    IFS=':' read -r name file desc <<< "$bench_spec"

    echo -e "\n${BLUE}$name${NC} ($desc)"
    echo "----------------------------------------"

    if ! run_benchmark "$name" "$file" "$desc"; then
        echo -e "${RED}✗ Failed${NC}"
    else
        echo -e "${GREEN}✓ Completed${NC}"
    fi
done

echo ""
echo -e "${GREEN}All benchmarks completed!${NC}"