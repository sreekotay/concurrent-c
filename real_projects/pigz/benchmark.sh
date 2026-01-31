#!/bin/bash
# Benchmark script using REAL compressible data (not random)
# Downloads Silesia corpus or uses existing test files

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"

# Configuration
WORKERS=${1:-4}
RUNS=${2:-3}

echo "=============================================="
echo "pigz Benchmark: Real Compressible Data"
echo "=============================================="
echo ""
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

# Create data directory
mkdir -p "$DATA_DIR"
cd "$DATA_DIR"

# Store absolute paths
PIGZ_ORIG="$OUT_DIR/pigz"
PIGZ_CC="$OUT_DIR/pigz_cc"

# Generate enwik8 alternative (text-like data that compresses well)
generate_text_data() {
    local size_mb=$1
    local outfile="text_${size_mb}mb.bin"
    
    if [ ! -f "$outfile" ]; then
        echo "Generating ${size_mb}MB of compressible text data..."
        # Download enwik8 (100MB Wikipedia dump) - classic benchmark
        if [ ! -f "enwik8" ]; then
            curl -L -o enwik8.zip "https://mattmahoney.net/dc/enwik8.zip"
            unzip -q enwik8.zip
        fi
        # Use enwik8 repeated or truncated to desired size
        if [ "$size_mb" -le 100 ]; then
            head -c ${size_mb}000000 enwik8 > "$outfile"
        else
            # Repeat enwik8 to get larger sizes
            local copies=$((size_mb / 100 + 1))
            for i in $(seq 1 $copies); do
                cat enwik8
            done | head -c ${size_mb}000000 > "$outfile"
        fi
        echo "Text data ready: $(du -h $outfile | cut -f1)"
    fi
}

# Function to run compression benchmark
run_compress() {
    local name=$1
    local cmd=$2
    local input=$3
    local total_time=0
    
    local input_size=$(wc -c < "$input")
    echo "=== $name (Compression) ==="
    echo "Input: $(du -h $input | cut -f1) ($input_size bytes)"
    
    for run in $(seq 1 $RUNS); do
        # Ensure fresh input
        cp "$input" bench_test.bin
        
        # Drop caches (optional, skip silently)
        sync
        
        # Time compression
        start=$(python3 -c 'import time; print(time.time())')
        $cmd bench_test.bin
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        output_size=$(wc -c < bench_test.bin.gz)
        ratio=$(python3 -c "print(f'{$output_size / $input_size * 100:.1f}%')")
        throughput=$(python3 -c "print(f'{$input_size / 1024 / 1024 / $elapsed:.1f}')")
        
        echo "  Run $run: ${elapsed}s, ${output_size} bytes (${ratio}), ${throughput} MB/s"
        rm -f bench_test.bin bench_test.bin.gz
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    avg_throughput=$(python3 -c "print(f'{$input_size / 1024 / 1024 / ($total_time / $RUNS):.1f}')")
    echo "  Average: ${avg}s (${avg_throughput} MB/s)"
    echo ""
}

# Function to run decompression benchmark
run_decompress() {
    local name=$1
    local cmd=$2
    local compressed=$3
    local total_time=0
    
    local compressed_size=$(wc -c < "$compressed")
    echo "=== $name (Decompression) ==="
    
    for run in $(seq 1 $RUNS); do
        # Ensure fresh compressed input
        cp "$compressed" bench_test.gz
        
        # Drop caches (optional, skip silently)
        sync
        
        # Time decompression
        start=$(python3 -c 'import time; print(time.time())')
        $cmd bench_test.gz
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        output_size=$(wc -c < bench_test)
        throughput=$(python3 -c "print(f'{$output_size / 1024 / 1024 / $elapsed:.1f}')")
        
        echo "  Run $run: ${elapsed}s, ${output_size} bytes decompressed, ${throughput} MB/s"
        rm -f bench_test.gz bench_test
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    echo "  Average: ${avg}s"
    echo ""
}

# Download/generate test data
echo "=== Preparing Test Data ==="
generate_text_data 200  # 200MB of enwik8 (Wikipedia XML)

INPUT_FILE="text_200mb.bin"
echo ""

# Run compression benchmarks
echo "=============================================="
echo "COMPRESSION BENCHMARKS"
echo "=============================================="
echo ""

run_compress "Original pigz (pthread)" "$PIGZ_ORIG -k -p $WORKERS" "$INPUT_FILE"
run_compress "CC pigz (Concurrent-C)"  "$PIGZ_CC -k -p $WORKERS" "$INPUT_FILE"

# Create compressed files for decompression benchmarks
echo "=== Preparing Compressed Files ==="
cp "$INPUT_FILE" bench_orig.bin
$PIGZ_ORIG -k -p $WORKERS bench_orig.bin
mv bench_orig.bin.gz bench_orig.gz
echo "Original pigz compressed: $(du -h bench_orig.gz | cut -f1)"

cp "$INPUT_FILE" bench_cc.bin
$PIGZ_CC -k -p $WORKERS bench_cc.bin
mv bench_cc.bin.gz bench_cc.gz
echo "CC pigz compressed: $(du -h bench_cc.gz | cut -f1)"
echo ""

# Run decompression benchmarks
echo "=============================================="
echo "DECOMPRESSION BENCHMARKS"
echo "=============================================="
echo ""

run_decompress "Original pigz (pthread)" "$PIGZ_ORIG -d -k -p $WORKERS" "bench_orig.gz"
run_decompress "CC pigz (Concurrent-C)"  "$PIGZ_CC -d -k -p $WORKERS" "bench_cc.gz"

# Verify correctness
echo "=== Correctness Check ==="
cp "$INPUT_FILE" verify_input.bin

$PIGZ_ORIG -k -c verify_input.bin > orig.gz
$PIGZ_CC -k -c verify_input.bin > cc.gz

gunzip -c orig.gz > orig_decomp.bin
gunzip -c cc.gz > cc_decomp.bin

echo -n "Original pigz: "
if cmp -s verify_input.bin orig_decomp.bin; then
    echo "PASS"
else
    echo "FAIL"
fi

echo -n "CC pigz:       "
if cmp -s verify_input.bin cc_decomp.bin; then
    echo "PASS"
else
    echo "FAIL"
fi

# Show compression ratios
echo ""
echo "=== Compression Summary ==="
orig_size=$(wc -c < "$INPUT_FILE")
orig_compressed=$(wc -c < orig.gz)
cc_compressed=$(wc -c < cc.gz)
orig_ratio=$(python3 -c "print(f'{$orig_compressed / $orig_size * 100:.2f}%')")
cc_ratio=$(python3 -c "print(f'{$cc_compressed / $orig_size * 100:.2f}%')")

echo "Input:          $(du -h $INPUT_FILE | cut -f1) ($orig_size bytes)"
echo "Original pigz:  $(du -h orig.gz | cut -f1) ($orig_compressed bytes) - ${orig_ratio}"
echo "CC pigz:        $(du -h cc.gz | cut -f1) ($cc_compressed bytes) - ${cc_ratio}"

# Cleanup temp files
rm -f verify_input.bin orig.gz cc.gz orig_decomp.bin cc_decomp.bin
rm -f bench_orig.bin bench_cc.bin bench_orig.gz bench_cc.gz
rm -f bench_test.bin bench_test.gz bench_test

echo ""
echo "Done."
