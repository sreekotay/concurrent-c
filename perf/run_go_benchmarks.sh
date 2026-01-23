#!/bin/bash
# Go Performance Benchmark Runner
# Runs Go versions of benchmarks and formats results

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GO_DIR="$SCRIPT_DIR/go"

# Check if go is available
if ! command -v go &> /dev/null; then
    echo -e "${RED}Error: Go is not installed or not in PATH${NC}"
    exit 1
fi

echo -e "${BLUE}Go Performance Benchmarks${NC}"
echo "========================"

# Function to run a single benchmark
run_benchmark() {
    local name="$1"
    local file="$2"
    local desc="$3"

    echo -e "${YELLOW}Running $name...${NC}"

    # Run benchmark and capture output
    local output
    if ! output=$(cd "$GO_DIR" && go run "$file" 2>&1); then
        echo -e "${RED}FAILED${NC}"
        return 1
    fi

    # Extract and display the performance metrics
    echo "$output" | grep -E "^[[:space:]]+[a-zA-Z].*:" | sed 's/^[[:space:]]*//'
}

# Main benchmarks to run
BENCHMARKS=(
    "spawn_nursery:spawn_nursery.go:Nursery-based spawn throughput"
    "spawn_sequential:spawn_sequential.go:Sequential spawn+join throughput"
    "channel_throughput:channel_throughput.go:Channel operations throughput"
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
echo -e "${GREEN}All Go benchmarks completed!${NC}"