#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"

SIZE_MB=${1:-100}
RUNS=${2:-10}
ZIG_SOURCE="$SCRIPT_DIR/pigz_zig.zig"
ZIG_BIN="$OUT_DIR/pigz_zig"

# Validate SIZE_MB is a number; reject non-numeric arguments
if ! [[ "$SIZE_MB" =~ ^[0-9]+$ ]]; then
    echo "Usage: $0 [size_mb] [runs]  (e.g. $0 100 10)"
    echo "Error: '$SIZE_MB' is not a valid size in MB"
    exit 1
fi

# Ensure ccc runtime object and generated headers are fresh.
make -C "$SCRIPT_DIR/../../cc" > /dev/null
make -C "$SCRIPT_DIR/../../cc" lower-headers > /dev/null

# Ensure binaries are built
make pigz pigz_cc pigz_channel pigz_pthread > /dev/null
go build -ldflags="-s -w" -o out/pigz_go pigz_go.go > /dev/null
ZIG_AVAILABLE=0
if command -v zig >/dev/null 2>&1 && [ -f "$ZIG_SOURCE" ]; then
    if zig build-exe "$ZIG_SOURCE" -O ReleaseFast -lc \
        -I /opt/homebrew/include -L /opt/homebrew/lib -lz \
        -femit-bin="$ZIG_BIN" > /dev/null; then
        ZIG_AVAILABLE=1
    fi
fi

# Ensure test data exists
mkdir -p "$DATA_DIR"
INPUT_FILE="$DATA_DIR/text_${SIZE_MB}mb.bin"

if [ ! -f "$INPUT_FILE" ]; then
    echo "Generating test data from Silesia corpus..."
    # shellcheck source=download_silesia.sh
    source "$SCRIPT_DIR/download_silesia.sh"
    download_silesia "$DATA_DIR"
    TARGET=$((SIZE_MB * 1000000))
    : > "$INPUT_FILE"
    files=$(find "$DATA_DIR/silesia" -type f -print | LC_ALL=C sort)
    while [ "$(wc -c < "$INPUT_FILE" 2>/dev/null || echo 0)" -lt "$TARGET" ]; do
        # shellcheck disable=SC2086
        cat $files >> "$INPUT_FILE"
    done
    head -c "$TARGET" "$INPUT_FILE" > "${INPUT_FILE}.tmp" && mv "${INPUT_FILE}.tmp" "$INPUT_FILE"
fi

# --- benchmark definitions: name, cmd, flags ---
# CC (Pthread) is the ratio baseline (find by name below).
# "CC (Full -i)" is Full with --independent: no dict chaining, fair vs Channel.
# Always pass -k for pigz-compatible CLIs so a run cannot unlink the corpus
# (that is what aborted the interleaved loop under set -e).
BENCH_NAMES=("CC (Channel)" "CC (Full)" "CC (Full -i)" "CC (Pthread)" "Go (CGO+zlib)")
BENCH_CMDS=("$OUT_DIR/pigz_channel" "$OUT_DIR/pigz_cc" "$OUT_DIR/pigz_cc" "$OUT_DIR/pigz_pthread" "$OUT_DIR/pigz_go")
BENCH_FLAGS=("" "-k -f" "-k -f -i" "" "")
if [ "$ZIG_AVAILABLE" -eq 1 ]; then
    BENCH_NAMES+=("Zig")
    BENCH_CMDS+=("$ZIG_BIN")
    BENCH_FLAGS+=("")
fi
BENCH_NAMES+=("Original pigz")
BENCH_CMDS+=("$OUT_DIR/pigz")
BENCH_FLAGS+=("-k -f")

N=${#BENCH_NAMES[@]}

# Per-benchmark arrays: accumulated times and RSS
declare -a BEST_TIMES
declare -a RSS_VALS
declare -a ALL_TIMES   # flat: ALL_TIMES[bench*RUNS + run]

for ((b=0; b<N; b++)); do
    BEST_TIMES[$b]="9999"
    RSS_VALS[$b]="?"
    for ((r=0; r<RUNS; r++)); do
        ALL_TIMES[$((b*RUNS + r))]="?"
    done
done

echo "=========================================================="
echo "pigz Full Comparison: CC vs Go vs Zig vs Original"
echo "=========================================================="
echo "Input:  $INPUT_FILE ($(wc -c < "$INPUT_FILE" | awk '{printf "%.0f MB", $1/1000000}'))"
echo "Runs:   $RUNS  (interleaved, best reported)"
echo "OS:     $(uname -s) $(uname -r)"
if [ "$ZIG_AVAILABLE" -eq 0 ]; then
    echo "Zig:    unavailable (skipping Zig benchmark)"
fi
echo "=========================================================="
echo ""

# Work on a copy so gzip-style unlink (-k omitted / buggy keep) cannot
# destroy the corpus mid-interleave under set -e.
WORK_FILE="$DATA_DIR/.bench_work_${SIZE_MB}mb.bin"
restore_work() {
    if [ ! -f "$WORK_FILE" ]; then
        cp "$INPUT_FILE" "$WORK_FILE"
    fi
}
cp "$INPUT_FILE" "$WORK_FILE"

# Measure RSS once per benchmark before the main loop
echo "Measuring RSS (one pass per binary)..."
for ((b=0; b<N; b++)); do
    restore_work
    rm -f "$WORK_FILE.gz"
    cmd="${BENCH_CMDS[$b]}"
    flags="${BENCH_FLAGS[$b]}"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        rss_bytes=$(/usr/bin/time -l $cmd $flags "$WORK_FILE" > /dev/null 2>&1 || \
                    /usr/bin/time -l $cmd $flags "$WORK_FILE" 2>&1 >/dev/null | \
                    grep "maximum resident set size" | awk '{print $1}')
        # Retry with stderr redirect
        rss_raw=$(2>&1 /usr/bin/time -l $cmd $flags "$WORK_FILE" > /dev/null | grep "maximum resident set size" | awk '{print $1}')
        if [ -n "$rss_raw" ] && [ "$rss_raw" -gt 0 ] 2>/dev/null; then
            RSS_VALS[$b]=$(python3 -c "print(f'{$rss_raw/1024/1024:.0f}')")
        fi
    fi
done
rm -f "$WORK_FILE.gz"
restore_work
echo ""

# --- Interleaved timing runs ---
echo "Running $RUNS interleaved rounds..."
printf "Round"
for ((b=0; b<N; b++)); do printf "  %-14s" "${BENCH_NAMES[$b]}"; done
echo ""

for ((r=0; r<RUNS; r++)); do
    printf "  %3d" $((r+1))
    for ((b=0; b<N; b++)); do
        restore_work
        rm -f "$WORK_FILE.gz"
        cmd="${BENCH_CMDS[$b]}"
        flags="${BENCH_FLAGS[$b]}"
        T0=$(python3 -c 'import time; print(time.time())')
        $cmd $flags "$WORK_FILE" > /dev/null 2>&1
        T1=$(python3 -c 'import time; print(time.time())')
        elapsed=$(python3 -c "print(f'{$T1-$T0:.3f}')")
        ALL_TIMES[$((b*RUNS + r))]="$elapsed"
        # Track best
        cur_best="${BEST_TIMES[$b]}"
        is_better=$(python3 -c "print(1 if $elapsed < $cur_best else 0)")
        if [ "$is_better" = "1" ]; then BEST_TIMES[$b]="$elapsed"; fi
        printf "  %-14s" "$elapsed"
    done
    echo ""
done
rm -f "$WORK_FILE" "$WORK_FILE.gz"

# --- Summary ---
echo ""
echo "============================================================================"
echo "FINAL SUMMARY  (best of $RUNS interleaved runs)"
echo "============================================================================"
printf "%-15s | %-8s | %-8s | %-8s | %-12s | %-8s | %-8s\n" \
    "Implementation" "Best(s)" "Worst(s)" "Var" "Throughput" "Max RSS" "Bin Size"
printf "%-15s | %-8s | %-8s | %-8s | %-12s | %-8s | %-8s\n" \
    "---------------" "--------" "--------" "--------" "------------" "--------" "--------"

for ((b=0; b<N; b++)); do
    best="${BEST_TIMES[$b]}"
    # Collect all times for this benchmark into a space-separated string
    times_str=""
    for ((r=0; r<RUNS; r++)); do
        t="${ALL_TIMES[$((b*RUNS + r))]}"
        times_str="$times_str $t"
    done
    worst=$(python3 -c "import sys; ts=[float(x) for x in '$times_str'.split()]; print(f'{max(ts):.3f}')")
    variance=$(python3 -c "print(f'{$worst / $best:.2f}x')")
    throughput=$(python3 -c "print(f'{${SIZE_MB} / $best:.1f}')")
    bsize=$(ls -lh "${BENCH_CMDS[$b]}" | awk '{print $5}')
    printf "%-15s | %-8s | %-8s | %-8s | %-12s | %-8s | %-8s\n" \
        "${BENCH_NAMES[$b]}" "${best}s" "${worst}s" "$variance" "${throughput} MB/s" "${RSS_VALS[$b]}MB" "$bsize"
done
echo "============================================================================"
echo ""

# Ratio vs CC (Pthread) as baseline
PT_IDX=-1
for ((b=0; b<N; b++)); do
    if [ "${BENCH_NAMES[$b]}" = "CC (Pthread)" ]; then PT_IDX=$b; break; fi
done
if [ "$PT_IDX" -lt 0 ]; then
    echo "Ratios: CC (Pthread) row missing" >&2
    exit 1
fi
pt_best="${BEST_TIMES[$PT_IDX]}"
echo "Ratios vs CC (Pthread):"
for ((b=0; b<N; b++)); do
    ratio=$(python3 -c "print(f'{${BEST_TIMES[$b]} / $pt_best:.3f}x')")
    printf "  %-15s  %s\n" "${BENCH_NAMES[$b]}" "$ratio"
done
