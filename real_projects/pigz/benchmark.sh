#!/bin/bash
# Benchmark script using REAL compressible data (not random)
# Auto-downloads a real corpus if missing, and generates a sized input file.
#
# Usage:
#   ./benchmark.sh <size_mb> <workers> <runs> <mem_limit_mb>
#
# Examples:
#   Standard:      ./benchmark.sh 100 8 4
#   Neckbeard OOM: ./benchmark.sh 100 8 4 64    (Limits address space to 64MB)
#   Metadata Storm: ./benchmark.sh 100 128 1 0   (Many workers, tiny blocks)
#
# To test backpressure (Slow Consumer):
#   ./out/pigz_cc -p 8 -k <input> | pv -L 1M > /dev/null
#   (Monitor RSS to ensure it stays flat)

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"

# Configuration
SIZE_MB=${1:-100}
WORKERS=${2:-8}
RUNS=${3:-4}
MEM_LIMIT_MB=${4:-0}  # 0 = no limit

# For fiber-based pigz_cc, use 2x CPU workers for CPU-bound compression
export CC_WORKERS=${CC_WORKERS:-16}

echo "=============================================="
echo "pigz Benchmark: Real Compressible Data"
echo "=============================================="
echo ""
echo "Input size:   ${SIZE_MB} MB"
echo "pigz workers: ${WORKERS}"
echo "CC_WORKERS:   ${CC_WORKERS} (fiber scheduler)"
echo "Runs:         ${RUNS}"
if [ "$MEM_LIMIT_MB" -gt 0 ]; then
    echo "Memory Limit: ${MEM_LIMIT_MB} MB"
fi
echo ""

# Ensure benchmark binaries (always under this script's out/, not cwd).
ensure_pigz_bins() {
    if [ ! -x "$OUT_DIR/pigz" ] || [ ! -x "$OUT_DIR/pigz_cc" ]; then
        echo "Building $OUT_DIR/pigz and pigz_cc ..."
        make -C "$SCRIPT_DIR" pigz pigz_cc
    fi
    if [ ! -x "$OUT_DIR/pigz" ]; then
        echo "Error: $OUT_DIR/pigz is missing or not executable after make."
        exit 1
    fi
    if [ ! -x "$OUT_DIR/pigz_cc" ]; then
        echo "Error: $OUT_DIR/pigz_cc is missing or not executable after make."
        exit 1
    fi
}

ensure_pigz_bins

PIGZ_ORIG="$OUT_DIR/pigz"
PIGZ_CC="$OUT_DIR/pigz_cc"

# Create data directory
mkdir -p "$DATA_DIR"
# shellcheck source=download_silesia.sh
source "$SCRIPT_DIR/download_silesia.sh"

# Generate sized data by concatenating real corpus files (more realistic than repeating one file)
generate_text_data() {
    local size_mb=$1
    local outfile="text_${size_mb}mb.bin"
    
    if [ ! -f "$outfile" ]; then
        echo "Generating ${size_mb}MB of real corpus data (concatenated)..."
        download_silesia "$DATA_DIR"
        cd "$DATA_DIR"

        local target_bytes=$((size_mb * 1000000))
        : > "$outfile"

        # Deterministic order for reproducibility.
        # Use only regular files; skip directories/metadata.
        local files
        files=$(find "$DATA_DIR/silesia" -type f -print | LC_ALL=C sort)

        if [ -z "$files" ]; then
            echo "Error: no files found under testdata/silesia/"
            exit 1
        fi

        # Append corpus files repeatedly until target size reached, then truncate.
        while [ "$(wc -c < "$outfile")" -lt "$target_bytes" ]; do
            # shellcheck disable=SC2086
            cat $files >> "$outfile"
        done
        head -c "$target_bytes" "$outfile" > "${outfile}.tmp" && mv "${outfile}.tmp" "$outfile"

        echo "Text data ready: $(du -h $outfile | cut -f1)"
    fi
}

# Function to run compression benchmark
run_compress() {
    local key=$1
    local name=$2
    local binary=$3
    local input=$4
    local total_time=0
    local total_out=0
    
    local input_size=$(wc -c < "$input")
    echo "=== $name (Compression) ==="
    echo "Input: $(du -h $input | cut -f1) ($input_size bytes)"
    
    for run in $(seq 1 $RUNS); do
        if [ ! -x "$binary" ]; then
            echo "Error: benchmark binary missing mid-run: $binary"
            exit 1
        fi
        # Ensure fresh input
        cp "$input" bench_test.bin
        
        # Drop caches (optional, skip silently)
        sync
        
        # Time compression
        start=$(python3 -c 'import time; print(time.time())')
        
        if [ "$MEM_LIMIT_MB" -gt 0 ]; then
            # Use ulimit to restrict virtual memory (approximate memory limit)
            # 1024 * MEM_LIMIT_MB
            (ulimit -v $((MEM_LIMIT_MB * 1024)); "$binary" -k -p "$WORKERS" bench_test.bin)
        else
            "$binary" -k -p "$WORKERS" bench_test.bin
        fi
        
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        output_size=$(wc -c < bench_test.bin.gz)
        ratio=$(python3 -c "print(f'{$output_size / $input_size * 100:.1f}%')")
        throughput=$(python3 -c "print(f'{$input_size / 1024 / 1024 / $elapsed:.1f}')")
        
        echo "  Run $run: ${elapsed}s, ${output_size} bytes (${ratio}), ${throughput} MB/s"
        rm -f bench_test.bin bench_test.bin.gz
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
        total_out=$(python3 -c "print($total_out + $output_size)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    avg_throughput=$(python3 -c "print(f'{$input_size / 1024 / 1024 / ($total_time / $RUNS):.1f}')")
    avg_out=$(python3 -c "print(int($total_out / $RUNS))")
    avg_ratio=$(python3 -c "print(f'{$avg_out / $input_size * 100:.1f}%')")
    echo "  Average: ${avg}s (${avg_throughput} MB/s), avg ratio ${avg_ratio}"
    echo ""

    # Export summary numbers for the final table (keys are expected to be small/simple)
    eval "COMP_${key}_AVG_S=${avg}"
    eval "COMP_${key}_AVG_MBPS=${avg_throughput}"
    eval "COMP_${key}_AVG_RATIO=${avg_ratio}"
}

# Function to run decompression benchmark
run_decompress() {
    local key=$1
    local name=$2
    local binary=$3
    local compressed=$4
    local total_time=0
    local total_out=0
    
    echo "=== $name (Decompression) ==="
    
    for run in $(seq 1 $RUNS); do
        if [ ! -x "$binary" ]; then
            echo "Error: benchmark binary missing mid-run: $binary"
            exit 1
        fi
        # Ensure fresh compressed input
        cp "$compressed" bench_test.gz
        
        # Drop caches (optional, skip silently)
        sync
        
        # Time decompression
        start=$(python3 -c 'import time; print(time.time())')
        
        if [ "$MEM_LIMIT_MB" -gt 0 ]; then
            (ulimit -v $((MEM_LIMIT_MB * 1024)) 2>/dev/null || true; "$binary" -d -k -p "$WORKERS" bench_test.gz)
        else
            "$binary" -d -k -p "$WORKERS" bench_test.gz
        fi
        
        end=$(python3 -c 'import time; print(time.time())')
        
        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        output_size=$(wc -c < bench_test)
        throughput=$(python3 -c "print(f'{$output_size / 1024 / 1024 / $elapsed:.1f}')")
        
        echo "  Run $run: ${elapsed}s, ${output_size} bytes decompressed, ${throughput} MB/s"
        rm -f bench_test.gz bench_test
        
        total_time=$(python3 -c "print($total_time + $elapsed)")
        total_out=$(python3 -c "print($total_out + $output_size)")
    done
    
    avg=$(python3 -c "print(f'{$total_time / $RUNS:.3f}')")
    avg_out=$(python3 -c "print(int($total_out / $RUNS))")
    avg_throughput=$(python3 -c "print(f'{$avg_out / 1024 / 1024 / ($total_time / $RUNS):.1f}')")
    echo "  Average: ${avg}s (${avg_throughput} MB/s)"
    echo ""

    eval "DECOMP_${key}_AVG_S=${avg}"
    eval "DECOMP_${key}_AVG_MBPS=${avg_throughput}"
}

# Download/generate test data
echo "=== Preparing Test Data ==="
generate_text_data "$SIZE_MB"

INPUT_FILE="text_${SIZE_MB}mb.bin"
echo ""

# Run compression benchmarks
echo "=============================================="
echo "COMPRESSION BENCHMARKS"
echo "=============================================="
echo ""

run_compress cc   "CC pigz (Concurrent-C)"  "$PIGZ_CC" "$INPUT_FILE"
run_compress orig "Original pigz (pthread)" "$PIGZ_ORIG" "$INPUT_FILE"

# Create compressed files for decompression benchmarks
echo "=== Preparing Compressed Files ==="
cp "$INPUT_FILE" bench_cc.bin
$PIGZ_CC -k -p $WORKERS bench_cc.bin
mv bench_cc.bin.gz bench_cc.gz
echo "CC pigz compressed: $(du -h bench_cc.gz | cut -f1)"

cp "$INPUT_FILE" bench_orig.bin
$PIGZ_ORIG -k -p $WORKERS bench_orig.bin
mv bench_orig.bin.gz bench_orig.gz
echo "Original pigz compressed: $(du -h bench_orig.gz | cut -f1)"
echo ""

# Run decompression benchmarks
echo "=============================================="
echo "DECOMPRESSION BENCHMARKS"
echo "=============================================="
echo ""

run_decompress cc   "CC pigz (Concurrent-C)"  "$PIGZ_CC" "bench_cc.gz"
run_decompress orig "Original pigz (pthread)" "$PIGZ_ORIG" "bench_orig.gz"

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

# Consolidated summary (easy to paste into issues/PRs)
echo ""
echo "=== Benchmark Summary (avg over ${RUNS} runs) ==="
printf "%-24s  %10s  %12s  %10s  %10s  %12s\n" "Implementation" "Comp(s)" "Comp(MB/s)" "Ratio" "Decomp(s)" "Decomp(MB/s)"
printf "%-24s  %10s  %12s  %10s  %10s  %12s\n" \
  "pigz (pthread)" \
  "${COMP_orig_AVG_S:-?}" "${COMP_orig_AVG_MBPS:-?}" "${COMP_orig_AVG_RATIO:-?}" \
  "${DECOMP_orig_AVG_S:-?}" "${DECOMP_orig_AVG_MBPS:-?}"
printf "%-24s  %10s  %12s  %10s  %10s  %12s\n" \
  "pigz_cc (Concurrent-C)" \
  "${COMP_cc_AVG_S:-?}" "${COMP_cc_AVG_MBPS:-?}" "${COMP_cc_AVG_RATIO:-?}" \
  "${DECOMP_cc_AVG_S:-?}" "${DECOMP_cc_AVG_MBPS:-?}"

# Cleanup temp files
rm -f verify_input.bin orig.gz cc.gz orig_decomp.bin cc_decomp.bin
rm -f bench_orig.bin bench_cc.bin bench_orig.gz bench_cc.gz
rm -f bench_test.bin bench_test.gz bench_test

echo ""
echo "Done."
