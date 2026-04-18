#!/bin/bash
# compare_contention_stability.sh - Repeat channel isolation comparison and
# summarize min/mean/max for baseline, contention, and interference.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNS="${1:-5}"

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

echo "================================================================="
echo "CHANNEL ISOLATION STABILITY"
echo "================================================================="
echo "Runs: $RUNS"
echo ""

for run in $(seq 1 "$RUNS"); do
    echo "[$run/$RUNS] Running compare_contention.sh..."
    "$SCRIPT_DIR/compare_contention.sh" > "$TMP_DIR/run_$run.txt"
done

python3 - "$TMP_DIR" "$RUNS" <<'PY'
import pathlib
import re
import statistics
import sys

tmp_dir = pathlib.Path(sys.argv[1])
runs = int(sys.argv[2])

metrics = {
    "Pthread": {
        "baseline": "DATA_PTHREAD_BASELINE_MS",
        "contention": "DATA_PTHREAD_CONTENTION_MS",
        "interference": "DATA_PTHREAD_INTERFERENCE",
    },
    "Concurrent-C": {
        "baseline": "DATA_CC_BASELINE_MS",
        "contention": "DATA_CC_CONTENTION_MS",
        "interference": "DATA_CC_INTERFERENCE",
    },
    "Go": {
        "baseline": "DATA_GO_BASELINE_MS",
        "contention": "DATA_GO_CONTENTION_MS",
        "interference": "DATA_GO_INTERFERENCE",
    },
    "Zig": {
        "baseline": "DATA_ZIG_BASELINE_MS",
        "contention": "DATA_ZIG_CONTENTION_MS",
        "interference": "DATA_ZIG_INTERFERENCE",
    },
}

values = {name: {k: [] for k in cols} for name, cols in metrics.items()}

for run in range(1, runs + 1):
    text = (tmp_dir / f"run_{run}.txt").read_text()
    for name, cols in metrics.items():
        for col, key in cols.items():
            m = re.search(rf"^{re.escape(key)}:\s*([^\n]+)$", text, re.M)
            if m:
                raw = m.group(1).strip().rstrip("%")
                values[name][col].append(float(raw))

def fmt_triplet(xs):
    if not xs:
        return "N/A"
    return f"{min(xs):.2f} / {statistics.mean(xs):.2f} / {max(xs):.2f}"

print("min / mean / max")
print("--------------------------------------------------------------------------")
print(f"{'Implementation':<20} {'Baseline (ms)':<22} {'Contention (ms)':<22} {'Interference %':<22}")
for name in ("Pthread", "Concurrent-C", "Go", "Zig"):
    print(
        f"{name:<20} "
        f"{fmt_triplet(values[name]['baseline']):<22} "
        f"{fmt_triplet(values[name]['contention']):<22} "
        f"{fmt_triplet(values[name]['interference']):<22}"
    )
PY
