#!/bin/bash
# Compression-only benchmark: pigz (pthread) vs pigz_idiomatic (fibers) vs pigz_pthread (cc_thread_spawn)
# Usage: ./bench_compress_only.sh [size_mb] [runs]
# Reports BEST time out of N runs.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"
SIZE_MB=${1:-200}
RUNS=${2:-5}

echo "=============================================="
echo "Compression-Only Benchmark (best of ${RUNS})"
echo "=============================================="
echo "Input:   ${SIZE_MB} MB (Silesia corpus)"
echo "Runs:    ${RUNS}"
echo "Workers: 8 (pigz -p 8)"
echo ""

# Generate test data if needed
mkdir -p "$DATA_DIR"
INPUT="$DATA_DIR/text_${SIZE_MB}mb.bin"

if [ ! -f "$INPUT" ]; then
    echo "Generating ${SIZE_MB}MB test data from Silesia corpus..."
    target_bytes=$((SIZE_MB * 1000000))
    : > "$INPUT"
    files=$(find "$DATA_DIR/silesia" -type f -print | LC_ALL=C sort)
    while [ "$(wc -c < "$INPUT")" -lt "$target_bytes" ]; do
        cat $files >> "$INPUT"
    done
    head -c "$target_bytes" "$INPUT" > "${INPUT}.tmp" && mv "${INPUT}.tmp" "$INPUT"
fi

input_size=$(wc -c < "$INPUT")
echo "Input file: $(du -h "$INPUT" | cut -f1) ($input_size bytes)"
echo ""

# Benchmark function — reports best time
run_bench() {
    local label=$1
    local cmd=$2
    local best_elapsed="999"
    local best_tp="0"
    local all_ok=1

    echo "=== $label ==="
    for run in $(seq 1 $RUNS); do
        cp "$INPUT" /tmp/bench_pigz_input.bin
        sync

        start=$(python3 -c 'import time; print(time.time())')
        $cmd /tmp/bench_pigz_input.bin 2>/dev/null
        end=$(python3 -c 'import time; print(time.time())')

        elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        out_size=$(wc -c < /tmp/bench_pigz_input.bin.gz)
        throughput=$(python3 -c "print(f'{$input_size / 1024 / 1024 / $elapsed:.1f}')")
        ratio=$(python3 -c "print(f'{$out_size / $input_size * 100:.1f}')")

        # Correctness: verify output decompresses to input
        gunzip -c /tmp/bench_pigz_input.bin.gz > /tmp/bench_pigz_verify.bin 2>/dev/null
        if cmp -s "$INPUT" /tmp/bench_pigz_verify.bin; then
            ok="OK"
        else
            ok="CORRUPT"
            all_ok=0
        fi
        rm -f /tmp/bench_pigz_verify.bin

        echo "  Run $run: ${elapsed}s  ${throughput} MB/s  ratio=${ratio}%  (${out_size} bytes)  [$ok]"
        rm -f /tmp/bench_pigz_input.bin /tmp/bench_pigz_input.bin.gz

        best_elapsed=$(python3 -c "print(f'{min($best_elapsed, $elapsed):.3f}')")
    done

    best_tp=$(python3 -c "print(f'{$input_size / 1024 / 1024 / $best_elapsed:.1f}')")
    echo "  >> Best: ${best_elapsed}s  ${best_tp} MB/s"
    if [ "$all_ok" -eq 0 ]; then
        echo "  ** WARNING: CORRUPT OUTPUT DETECTED **"
    fi
    echo ""

    # Export for summary
    eval "RES_${3}=${best_tp}"
    eval "RES_${3}_S=${best_elapsed}"
    eval "RES_${3}_OK=${all_ok}"
}

# Verify all binaries exist
for bin in pigz pigz_idiomatic pigz_pthread; do
    if [ ! -f "$OUT_DIR/$bin" ]; then
        echo "Error: $OUT_DIR/$bin not found"
        exit 1
    fi
done

# Run benchmarks
run_bench "pigz (original, pthreads)"           "$OUT_DIR/pigz -k -p 8"       ORIG
run_bench "pigz_idiomatic (fibers, @spawn)"      "$OUT_DIR/pigz_idiomatic"     FIBER
run_bench "pigz_pthread (cc_thread_spawn)"        "$OUT_DIR/pigz_pthread"       THREAD

# Summary table
echo "=============================================="
echo "SUMMARY (${SIZE_MB}MB, best of ${RUNS}, compression only)"
echo "=============================================="
printf "%-32s  %10s  %10s  %10s\n" "Implementation" "Best(s)" "MB/s" "Correct"
printf "%-32s  %10s  %10s  %10s\n" "-------------------------------" "--------" "--------" "--------"
for key in ORIG FIBER THREAD; do
    eval "name_ORIG='pigz (original, pthreads)'"
    eval "name_FIBER='pigz_idiomatic (fibers)'"
    eval "name_THREAD='pigz_pthread (cc_thread_spawn)'"
    eval "n=\$name_${key}"
    eval "s=\$RES_${key}_S"
    eval "t=\$RES_${key}"
    eval "ok=\$RES_${key}_OK"
    if [ "$ok" = "1" ]; then correct="ALL OK"; else correct="CORRUPT!"; fi
    printf "%-32s  %10s  %10s  %10s\n" "$n" "${s:-?}" "${t:-?}" "$correct"
done
echo ""
