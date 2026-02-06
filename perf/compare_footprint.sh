#!/bin/bash
# compare_footprint.sh - Compare binary size and memory usage vs Go

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"

mkdir -p "$SCRIPT_DIR/out"

echo "================================================================="
echo "BINARY SIZE COMPARISON (Hello World)"
echo "================================================================="

# 1. Build Hello World
echo "Building Hello World..."
echo '#include <ccc/std/prelude.cch>
int main() { printf("hello\n"); return 0; }' > "$SCRIPT_DIR/out/hello.ccs"
$CCC build "$SCRIPT_DIR/out/hello.ccs" -o "$SCRIPT_DIR/out/hello_cc" --release

echo 'package main
import "fmt"
func main() { fmt.Println("hello") }' > "$SCRIPT_DIR/out/hello.go"
go build -ldflags="-s -w" -o "$SCRIPT_DIR/out/hello_go" "$SCRIPT_DIR/out/hello.go"

# 2. Measure sizes
SIZE_CC=$(stat -f%z "$SCRIPT_DIR/out/hello_cc")
SIZE_GO=$(stat -f%z "$SCRIPT_DIR/out/hello_go")
SIZE_CC_GZ=$(gzip -c "$SCRIPT_DIR/out/hello_cc" | wc -c | tr -d ' ')
SIZE_GO_GZ=$(gzip -c "$SCRIPT_DIR/out/hello_go" | wc -c | tr -d ' ')

printf "%-20s %-15s %-15s\n" "Implementation" "Raw Size" "Gzip Size"
printf "%-20s %-15s %-15s\n" "Concurrent-C" "$SIZE_CC" "$SIZE_CC_GZ"
printf "%-20s %-15s %-15s\n" "Go" "$SIZE_GO" "$SIZE_GO_GZ"
echo ""

echo "================================================================="
echo "MEMORY FOOTPRINT COMPARISON (100k tasks)"
echo "================================================================="

# 3. Build Mem Stress
echo "Building Mem Stress..."
$CCC build "$SCRIPT_DIR/mem_stress.ccs" -o "$SCRIPT_DIR/out/mem_stress_cc" --release
go build -o "$SCRIPT_DIR/out/mem_stress_go" "$SCRIPT_DIR/go/mem_stress.go"

measure_rss() {
    local cmd=$1
    local name=$2
    
    $cmd > /dev/null 2>&1 &
    local pid=$!
    
    # Wait for spawn to complete (approx)
    sleep 3
    
    # Get RSS in KB
    local rss=$(ps -o rss= -p $pid | tr -d ' ')
    
    kill -9 $pid > /dev/null 2>&1 || true
    echo "$rss"
}

RSS_CC_RAW=$(measure_rss "$SCRIPT_DIR/out/mem_stress_cc" "Concurrent-C")
RSS_GO_RAW=$(measure_rss "$SCRIPT_DIR/out/mem_stress_go" "Go")

# Clean up any potential extra output
RSS_CC=$(echo $RSS_CC_RAW | awk '{print $NF}')
RSS_GO=$(echo $RSS_GO_RAW | awk '{print $NF}')

echo "Concurrent-C RSS: ${RSS_CC} KB"
echo "Go RSS: ${RSS_GO} KB"

echo ""
echo "================================================================="
echo "FINAL VERDICT"
echo "================================================================="
printf "%-20s %-15s %-15s\n" "Metric" "Concurrent-C" "Go"
printf "%-20s %-15s %-15s\n" "Binary (Gzip)" "${SIZE_CC_GZ} B" "${SIZE_GO_GZ} B"
printf "%-20s %-15s %-15s\n" "RSS (100k tasks)" "${RSS_CC} KB" "${RSS_GO} KB"
echo "-----------------------------------------------------------------"

RATIO_SIZE=$(python3 -c "print(round($SIZE_GO_GZ / $SIZE_CC_GZ, 1))")
RATIO_MEM=$(python3 -c "print(round($RSS_GO / $RSS_CC, 1))")

echo "CC binary is ${RATIO_SIZE}x smaller than Go."
echo "CC memory is ${RATIO_MEM}x leaner than Go."
echo "================================================================="
