#!/bin/bash
# compare_contention_matrix.sh - Run shared-channel contention across common N x M shapes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

: "${CC_CONTENTION_ITERATIONS:=200000}"
: "${CC_CONTENTION_TRIALS:=2}"
export CC_CONTENTION_ITERATIONS
export CC_CONTENTION_TRIALS

CONFIGS=(
    "1 1"
    "1 8"
    "8 1"
    "8 8"
)

echo "================================================================="
echo "SHARED CHANNEL CONTENTION MATRIX"
echo "================================================================="
echo "Messages: ${CC_CONTENTION_ITERATIONS}"
echo "Trials: ${CC_CONTENTION_TRIALS}"
echo ""

for cfg in "${CONFIGS[@]}"; do
    read -r producers consumers <<<"$cfg"
    shape="${producers}x${consumers}"
    echo "[${shape}] Running compare_contention.sh..."
    CC_CONTENTION_PRODUCERS="$producers" \
    CC_CONTENTION_CONSUMERS="$consumers" \
    "$SCRIPT_DIR/compare_contention.sh" > "$TMP_DIR/${shape}.txt"
done

python3 - "$TMP_DIR" <<'PY'
import pathlib
import re
import sys

tmp_dir = pathlib.Path(sys.argv[1])
shapes = ["1x1", "1x8", "8x1", "8x8"]
metrics = [
    ("Pthread", "PTHREAD"),
    ("Concurrent-C", "CC"),
    ("Go", "GO"),
]

def extract(text: str, key: str):
    m = re.search(rf"^{re.escape(key)}:\s*([^\n]+)$", text, re.M)
    return m.group(1).strip().rstrip("%") if m else "N/A"

print("--------------------------------------------------------------------------")
print(f"{'Shape':<8} {'Implementation':<16} {'Baseline (ms)':<14} {'Contention (ms)':<16} {'Interference %':<16}")
print("--------------------------------------------------------------------------")
for shape in shapes:
    text = (tmp_dir / f"{shape}.txt").read_text()
    for idx, (name, prefix) in enumerate(metrics):
        baseline = extract(text, f"DATA_{prefix}_BASELINE_MS")
        contention = extract(text, f"DATA_{prefix}_CONTENTION_MS")
        interference = extract(text, f"DATA_{prefix}_INTERFERENCE")
        shape_label = shape if idx == 0 else ""
        print(f"{shape_label:<8} {name:<16} {baseline:<14} {contention:<16} {interference:<16}")
    print("--------------------------------------------------------------------------")
PY
