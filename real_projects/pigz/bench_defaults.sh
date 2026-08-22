#!/usr/bin/env bash
# Defaults-only pigz compare — the method in benchmarks/latest.txt.
#
#   ./bench_defaults.sh [size_mb] [runs] [names]
#
#   size_mb  default 50 (MiB, 1024*1024 — matches testdata/text_50mb.bin)
#   runs     default 5 interleaved rounds
#   names    comma list, default all built binaries
#            e.g. pigz,pigz_wait
#
# Each invoke is `<bin> <file>` only. No -p / -k / -i.
# CC_WORKERS, CC_THREADS, PIGZ_* are unset so each program picks its width.
# Fresh work copy per invoke (pigz deletes the source). gunzip -t after each.
# Page-caches the corpus once. Round 1 is cold; median is the fair column.
#
# Writes the same table shape as benchmarks/latest.txt to stdout.
# Optional: BENCH_OUT=benchmarks/latest.txt to also save.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
DATA_DIR="$SCRIPT_DIR/testdata"

SIZE_MB="${1:-50}"
RUNS="${2:-5}"
WANT="${3:-}"

if ! [[ "$SIZE_MB" =~ ^[0-9]+$ && "$RUNS" =~ ^[0-9]+$ && "$RUNS" -ge 1 ]]; then
    echo "Usage: $0 [size_mb] [runs] [comma,names]" >&2
    echo "  e.g. $0 50 5 pigz,pigz_wait" >&2
    exit 1
fi

unset CC_WORKERS CC_THREADS PIGZ_CAP PIGZ_DICT PIGZ_SEQ

# shellcheck source=download_silesia.sh
source "$SCRIPT_DIR/download_silesia.sh"

INPUT="$DATA_DIR/text_${SIZE_MB}mb.bin"
if [ ! -f "$INPUT" ]; then
    download_silesia "$DATA_DIR"
    target=$((SIZE_MB * 1024 * 1024))
    : > "$INPUT"
    files=$(find "$DATA_DIR/silesia" -type f -print | LC_ALL=C sort)
    if [ -z "$files" ]; then
        echo "Error: no files under testdata/silesia/" >&2
        exit 1
    fi
    while [ "$(wc -c < "$INPUT")" -lt "$target" ]; do
        # shellcheck disable=SC2086
        cat $files >> "$INPUT"
    done
    head -c "$target" "$INPUT" > "${INPUT}.tmp" && mv "${INPUT}.tmp" "$INPUT"
fi

# name|make-target|dict|extra   extra is empty, go, or zig
CANDIDATES=(
    "pigz|pigz|chain|"
    "pigz_cc|pigz_cc|chain|"
    "pigz_wait|pigz_wait|chain|"
    "pigz_idiomatic|pigz_idiomatic|indep|"
    "pigz_parallel|pigz_parallel|indep|"
    "pigz_hybrid|pigz_hybrid|indep|"
    "pigz_pthread|pigz_pthread|indep|"
    "pigz_go||indep|go"
    "pigz_zig||indep|zig"
)

want_name() {
    local n="$1"
    if [ -z "$WANT" ]; then
        return 0
    fi
    case ",$WANT," in
        *",$n,"*) return 0 ;;
        *) return 1 ;;
    esac
}

NAMES=()
BINS=()
DICTS=()

for row in "${CANDIDATES[@]}"; do
    IFS='|' read -r name target dict extra <<<"$row"
    want_name "$name" || continue
    bin="$OUT_DIR/$name"
    case "$extra" in
        go)
            if [ ! -x "$bin" ]; then
                if ! command -v go >/dev/null 2>&1; then
                    echo "skip $name (no go)" >&2
                    continue
                fi
                echo "Building $name ..." >&2
                (cd "$SCRIPT_DIR" && go build -ldflags="-s -w" -o "$bin" pigz_go.go)
            fi
            ;;
        zig)
            if [ ! -x "$bin" ]; then
                if ! command -v zig >/dev/null 2>&1; then
                    echo "skip $name (no zig)" >&2
                    continue
                fi
                echo "Building $name ..." >&2
                mkdir -p "$OUT_DIR"
                zig build-exe "$SCRIPT_DIR/pigz_zig.zig" -O ReleaseFast -lc \
                    -I /opt/homebrew/include -L /opt/homebrew/lib -lz \
                    -femit-bin="$bin"
            fi
            ;;
        *)
            if [ ! -x "$bin" ]; then
                echo "Building $name ..." >&2
                make -C "$SCRIPT_DIR" "$target"
            fi
            ;;
    esac
    if [ ! -x "$bin" ]; then
        echo "skip $name (build missing)" >&2
        continue
    fi
    NAMES+=("$name")
    BINS+=("$bin")
    DICTS+=("$dict")
done

if [ "${#NAMES[@]}" -eq 0 ]; then
    echo "Error: no binaries to run" >&2
    exit 1
fi

cat "$INPUT" >/dev/null

WORKDIR=$(mktemp -d /tmp/pigz_defaults_XXXXXX)
trap 'rm -rf "$WORKDIR"' EXIT

N=${#NAMES[@]}
TIMES=()
OUTS=()
for ((b = 0; b < N; b++)); do
    OUTS[$b]=0
    for ((r = 0; r < RUNS; r++)); do
        TIMES[$((b * RUNS + r))]="?"
    done
done

emit() {
    if [ -n "${BENCH_OUT:-}" ]; then
        printf '%s\n' "$*" | tee -a "$BENCH_OUT"
    else
        printf '%s\n' "$*"
    fi
}

if [ -n "${BENCH_OUT:-}" ]; then
    : > "$BENCH_OUT"
fi

bytes=$(wc -c < "$INPUT" | tr -d ' ')
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?')
cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -s)

emit "# All pigz versions — defaults only"
emit "# Date: $(date +%Y-%m-%d)"
emit "# Machine: $cpu, $cores cores"
emit "# Input: testdata/text_${SIZE_MB}mb.bin ($bytes bytes), Silesia concat"
emit "# Method: $RUNS interleaved rounds, page-cached input, invoke \`<bin> <file>\`"
emit "#         no -p / -k / -i; CC_WORKERS, CC_THREADS, PIGZ_* unset"
emit "#         each program picks its own width. gunzip -t after each run."
emit "#         Fresh work copy per invoke so pigz's delete-source default"
emit "#         does not consume the corpus."
emit ""
emit "Each binary was invoked as \`<bin> <file>\` only. No -p, no CC_WORKERS /"
emit "CC_THREADS / PIGZ_CAP / PIGZ_DICT / PIGZ_SEQ. pigz_wait defaults to"
emit "chained 32 KiB dictionaries (one gzip stream); PIGZ_DICT=0 opts out."
emit ""
emit "=== Interleaved wall seconds ==="

hdr=$(printf '%-5s' 'round')
for ((b = 0; b < N; b++)); do
    hdr+=$(printf ' %10s' "${NAMES[$b]}")
done
emit "$hdr"

for ((r = 0; r < RUNS; r++)); do
    row=$(printf '%5d' $((r + 1)))
    for ((b = 0; b < N; b++)); do
        work="$WORKDIR/in.bin"
        cp "$INPUT" "$work"
        rm -f "$work.gz"
        t0=$(python3 -c 'import time; print(time.perf_counter())')
        "${BINS[$b]}" "$work" >/dev/null
        t1=$(python3 -c 'import time; print(time.perf_counter())')
        elapsed=$(python3 -c "print(f'{$t1 - $t0:.3f}')")
        TIMES[$((b * RUNS + r))]="$elapsed"
        gz="$work.gz"
        if [ ! -f "$gz" ]; then
            echo "Error: ${NAMES[$b]} did not write $gz" >&2
            exit 1
        fi
        gunzip -t "$gz"
        OUTS[$b]=$(wc -c < "$gz" | tr -d ' ')
        rm -f "$work" "$gz"
        row+=$(printf ' %10s' "$elapsed")
    done
    emit "$row"
done

emit ""
emit "=== Summary (best / median / mean wall, output bytes, gunzip) ==="
emit "$(printf '%-18s %-6s %7s %8s %8s %12s %8s' \
    binary dict best median mean out gunzip)"

while IFS= read -r line; do
    emit "$line"
done < <(python3 - "$N" "$RUNS" "${NAMES[@]}" -- "${DICTS[@]}" -- "${OUTS[@]}" -- "${TIMES[@]}" <<'PY'
import sys
args = sys.argv[1:]
n = int(args[0]); runs = int(args[1])
rest = args[2:]
parts = []
cur = []
for a in rest:
    if a == "--":
        parts.append(cur)
        cur = []
    else:
        cur.append(a)
parts.append(cur)
names, dicts, outs, times = parts
for b in range(n):
    xs = [float(times[b * runs + r]) for r in range(runs)]
    xs.sort()
    best = xs[0]
    mean = sum(xs) / len(xs)
    mid = len(xs) // 2
    med = xs[mid] if len(xs) % 2 else 0.5 * (xs[mid - 1] + xs[mid])
    print(f"{names[b]:<18} {dicts[b]:<6} {best:7.3f} {med:8.3f} {mean:8.3f} {int(outs[b]):12d} {'ok':>8}")
PY
)

emit ""
emit "Round 1 is cold for every binary. Median is the fair column."
