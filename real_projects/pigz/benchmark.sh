#!/bin/bash
# Benchmark script using REAL compressible data (not random)
# Auto-downloads a real corpus if missing, and generates a sized input file.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"

# Configuration
SIZE_MB=${1:-100}
WORKERS=${2:-8}
RUNS=${3:-4}

echo "=============================================="
echo "pigz Benchmark: Real Compressible Data"
echo "=============================================="
echo ""
echo "Input size: ${SIZE_MB} MB"
echo "Workers:    ${WORKERS}"
echo "Runs:       ${RUNS}"
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

download_silesia() {
    # We keep the extracted corpus under testdata/silesia/ to avoid cluttering testdata/.
    if [ -d "silesia" ] && [ -n "$(ls -A silesia 2>/dev/null)" ]; then
        return 0
    fi

    local url="http://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
    echo "Downloading Silesia corpus from $url ..."

    rm -rf silesia
    mkdir -p silesia

    if command -v curl >/dev/null 2>&1; then
        curl -L --fail -o silesia.zip "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -O silesia.zip "$url"
    else
        echo "Error: need curl or wget to download silesia.zip"
        exit 1
    fi

    echo "Extracting silesia.zip ..."
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o silesia.zip -d silesia
    elif command -v python3 >/dev/null 2>&1; then
        python3 - <<'PY'
import zipfile, os
os.makedirs("silesia", exist_ok=True)
with zipfile.ZipFile("silesia.zip") as z:
    z.extractall("silesia")
PY
    else
        echo "Error: need unzip or python3 to extract silesia.zip"
        exit 1
    fi

    # Some zips contain a nested directory; normalize to silesia/*files*
    if [ -d "silesia/silesia" ] && [ -n "$(ls -A silesia/silesia 2>/dev/null)" ]; then
        rm -rf silesia.tmp
        mv silesia/silesia silesia.tmp
        rm -rf silesia
        mv silesia.tmp silesia
    fi

    if [ ! -d "silesia" ] || [ -z "$(ls -A silesia 2>/dev/null)" ]; then
        echo "Error: extraction failed (silesia directory is empty)"
        exit 1
    fi
}

# Generate sized data by concatenating real corpus files (more realistic than repeating one file)
generate_text_data() {
    local size_mb=$1
    local outfile="text_${size_mb}mb.bin"
    
    if [ ! -f "$outfile" ]; then
        echo "Generating ${size_mb}MB of real corpus data (concatenated)..."
        download_silesia

        local target_bytes=$((size_mb * 1000000))
        : > "$outfile"

        # Deterministic order for reproducibility.
        # Use only regular files; skip directories/metadata.
        local files
        files=$(find silesia -type f -print | LC_ALL=C sort)

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
    local cmd=$3
    local input=$4
    local total_time=0
    local total_out=0
    
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
    local cmd=$3
    local compressed=$4
    local total_time=0
    local total_out=0
    
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

run_compress cc   "CC pigz (Concurrent-C)"  "$PIGZ_CC -k -p $WORKERS" "$INPUT_FILE"
run_compress orig "Original pigz (pthread)" "$PIGZ_ORIG -k -p $WORKERS" "$INPUT_FILE"

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

run_decompress cc   "CC pigz (Concurrent-C)"  "$PIGZ_CC -d -k -p $WORKERS" "bench_cc.gz"
run_decompress orig "Original pigz (pthread)" "$PIGZ_ORIG -d -k -p $WORKERS" "bench_orig.gz"

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
