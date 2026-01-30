#!/bin/bash
# Benchmark script for comparing original pigz vs CC implementation

set -e
SCRIPT_DIR="$(dirname "$0")"

# Configuration
DATA_SIZE_MB=${1:-100}
WORKERS=${2:-4}
RUNS=${3:-3}
OUT_DIR="$SCRIPT_DIR/out"

echo "=============================================="
echo "pigz Benchmark: Original vs Concurrent-C"
echo "=============================================="
echo ""
echo "Data size: ${DATA_SIZE_MB}MB"
echo "Workers: ${WORKERS}"
echo "Runs: ${RUNS}"
echo ""

# Check binaries exist
if [ ! -f "$OUT_DIR/pigz" ]; then
    echo "Error: $OUT_DIR/pigz not found. Run 'make pigz' first."
    exit 1
fi

if [ ! -f "$OUT_DIR/pigz_cc" ]; then
    echo "Error: $OUT_DIR/pigz_cc not found. Run 'make pigz_cc' first."
    exit 1
fi

if [ ! -f "$OUT_DIR/pigz_idiomatic" ]; then
    echo "Warning: $OUT_DIR/pigz_idiomatic not found, skipping idiomatic benchmark."
    SKIP_IDIOMATIC=1
fi

# Change to out directory so all temp files go there
cd "$OUT_DIR"

# Generate test data
echo "Generating ${DATA_SIZE_MB}MB test data..."
dd if=/dev/urandom of=bench_input.bin bs=1M count=$DATA_SIZE_MB 2>/dev/null
echo ""

# Function to run benchmark
run_benchmark() {
    local name=$1
    local cmd=$2
    local total_time=0
    
    echo "=== $name ==="
    for run in $(seq 1 $RUNS); do
        # Ensure fresh input
        cp bench_input.bin bench_test.bin
        
        # Time compression
        start=$(python3 -c 'import time; print(time.time())')
        $cmd bench_test.bin
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        size=$(wc -c < bench_test.bin.gz)
        
        echo "  Run $run: ${elapsed}s, output: ${size} bytes"
        rm -f bench_test.bin bench_test.bin.gz
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    echo "  Average: ${avg}s"
    echo ""
}

# Function to run decompression benchmark
run_decomp_benchmark() {
    local name=$1
    local cmd=$2
    local total_time=0
    
    echo "=== $name (Decompression) ==="
    for run in $(seq 1 $RUNS); do
        # Ensure fresh compressed input
        cp bench_compressed.gz bench_test.gz
        
        # Time decompression
        start=$(python3 -c 'import time; print(time.time())')
        $cmd bench_test.gz
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        
        echo "  Run $run: ${elapsed}s"
        rm -f bench_test.gz bench_test
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    echo "  Average: ${avg}s"
    echo ""
}

# Run benchmarks (binaries are in current directory now)
run_benchmark "Original pigz (pthread)" "./pigz -k -p $WORKERS"
run_benchmark "CC pigz (Concurrent-C)"  "./pigz_cc -k -p $WORKERS"
if [ -z "$SKIP_IDIOMATIC" ]; then
    run_benchmark "CC pigz Idiomatic (T!E)" "./pigz_idiomatic"
fi

# Prepare compressed file for decompression benchmarks
./pigz -k -p $WORKERS -c bench_input.bin > bench_compressed.gz

run_decomp_benchmark "Original pigz (pthread)" "./pigz -d -k -p $WORKERS"
run_decomp_benchmark "CC pigz (Concurrent-C)"  "./pigz_cc -d -k -p $WORKERS"

# Verify correctness
echo "=== Correctness Check ==="
cp bench_input.bin bench_verify.bin

./pigz -k -c bench_verify.bin > orig.gz
./pigz_cc -k -c bench_verify.bin > cc.gz

gunzip -c orig.gz > orig_decomp.bin
gunzip -c cc.gz > cc_decomp.bin

if cmp -s bench_verify.bin orig_decomp.bin; then
    echo "Original: PASS (decompresses correctly)"
else
    echo "Original: FAIL"
fi

if cmp -s bench_verify.bin cc_decomp.bin; then
    echo "CC:       PASS (decompresses correctly)"
else
    echo "CC:       FAIL"
fi

if [ -z "$SKIP_IDIOMATIC" ]; then
    ./pigz_idiomatic bench_verify.bin
    mv bench_verify.bin.gz idiomatic.gz
    gunzip -c idiomatic.gz > idiomatic_decomp.bin
    if cmp -s bench_verify.bin idiomatic_decomp.bin; then
        echo "Idiomatic: PASS (decompresses correctly)"
    else
        echo "Idiomatic: FAIL"
    fi
    rm -f idiomatic.gz idiomatic_decomp.bin
fi

echo ""
echo "Compressed sizes:"
echo "  Original: $(wc -c < orig.gz) bytes"
echo "  CC:       $(wc -c < cc.gz) bytes"
if [ -z "$SKIP_IDIOMATIC" ]; then
    cp bench_input.bin bench_verify.bin
    ./pigz_idiomatic bench_verify.bin
    echo "  Idiomatic: $(wc -c < bench_verify.bin.gz) bytes"
    rm -f bench_verify.bin.gz
fi

# Cleanup
rm -f bench_input.bin bench_verify.bin bench_test.bin bench_compressed.gz
rm -f orig.gz cc.gz orig_decomp.bin cc_decomp.bin

echo ""
echo "Done."
