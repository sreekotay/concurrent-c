#!/bin/bash
# contention_workers4_stability.sh - Diagnose CC channel-isolation stability at 4 workers
#
# Reuses the existing channel contention benchmark but reports how often trials
# look serial-like instead of only showing the single best run.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="$REPO_ROOT/out/cc/bin/ccc"
WORKERS="${CC_WORKERS:-4}"
ROUNDS="${ROUNDS:-8}"
SERIAL_RATIO="${SERIAL_RATIO:-1.80}"
ELEVATED_RATIO="${ELEVATED_RATIO:-1.35}"

echo "================================================================="
echo "CHANNEL CONTENTION STABILITY DIAGNOSTIC"
echo "================================================================="
echo "Workers: $WORKERS | Benchmark rounds: $ROUNDS"
echo "Serial-like threshold: ratio >= $SERIAL_RATIO"
echo "Elevated threshold:    ratio >= $ELEVATED_RATIO"
echo ""

mkdir -p "$SCRIPT_DIR/out"
"$CCC" build --release "$SCRIPT_DIR/channel_contention.ccs" -o "$SCRIPT_DIR/out/channel_contention"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
aggregate_csv="$tmpdir/trials.csv"
: > "$aggregate_csv"

for round in $(seq 1 "$ROUNDS"); do
    round_out="$tmpdir/round_${round}.txt"
    echo "--- Round $round/$ROUNDS ---"
    CC_WORKERS="$WORKERS" "$SCRIPT_DIR/out/channel_contention" | tee "$round_out"
    python3 - "$round_out" "$aggregate_csv" "$round" "$SERIAL_RATIO" "$ELEVATED_RATIO" <<'PY'
import csv
import re
import statistics
import sys

round_out, aggregate_csv, round_id, serial_ratio, elevated_ratio = sys.argv[1:]
round_id = int(round_id)
serial_ratio = float(serial_ratio)
elevated_ratio = float(elevated_ratio)
trial_re = re.compile(r"Trial\s+(\d+):\s+baseline=\s*([0-9.]+)\s+ms\s+contention=\s*([0-9.]+)\s+ms")

rows = []
with open(round_out, "r", encoding="utf-8") as fh:
    for line in fh:
        m = trial_re.search(line)
        if not m:
            continue
        trial = int(m.group(1))
        baseline = float(m.group(2))
        contention = float(m.group(3))
        ratio = contention / baseline if baseline > 0 else 0.0
        rows.append((round_id, trial, baseline, contention, ratio))

if not rows:
    print("  [WARN] no trial rows parsed")
    sys.exit(0)

with open(aggregate_csv, "a", encoding="utf-8", newline="") as fh:
    writer = csv.writer(fh)
    writer.writerows(rows)

ratios = [r[4] for r in rows]
serial_like = sum(1 for ratio in ratios if ratio >= serial_ratio)
elevated = sum(1 for ratio in ratios if ratio >= elevated_ratio)
print(f"  Round summary: median_ratio={statistics.median(ratios):.2f} "
      f"serial_like={serial_like}/{len(ratios)} elevated={elevated}/{len(ratios)} "
      f"worst_ratio={max(ratios):.2f}")
PY
    echo ""
done

python3 - "$aggregate_csv" "$SERIAL_RATIO" "$ELEVATED_RATIO" "$WORKERS" "$ROUNDS" <<'PY'
import csv
import math
import statistics
import sys

aggregate_csv, serial_ratio, elevated_ratio, workers, rounds = sys.argv[1:]
serial_ratio = float(serial_ratio)
elevated_ratio = float(elevated_ratio)

rows = []
with open(aggregate_csv, "r", encoding="utf-8", newline="") as fh:
    reader = csv.reader(fh)
    for row in reader:
        if not row:
            continue
        round_id, trial, baseline, contention, ratio = row
        rows.append({
            "round": int(round_id),
            "trial": int(trial),
            "baseline": float(baseline),
            "contention": float(contention),
            "ratio": float(ratio),
        })

if not rows:
    print("No trials recorded.")
    sys.exit(1)

ratios = [row["ratio"] for row in rows]
serial_like = [row for row in rows if row["ratio"] >= serial_ratio]
elevated = [row for row in rows if row["ratio"] >= elevated_ratio]
good = [row for row in rows if row["ratio"] <= 1.20]
rounds_with_serial = sorted({row["round"] for row in serial_like})
worst = max(rows, key=lambda row: row["ratio"])
best = min(rows, key=lambda row: row["ratio"])

def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0

print("=================================================================")
print("STABILITY SUMMARY")
print("=================================================================")
print(f"Total trials:            {len(rows)} ({rounds} rounds x 15 trials)")
print(f"Median ratio:            {statistics.median(ratios):.2f}")
print(f"P90 ratio:               {statistics.quantiles(ratios, n=10)[8]:.2f}")
print(f"Best ratio:              {best['ratio']:.2f} "
      f"(round {best['round']} trial {best['trial']})")
print(f"Worst ratio:             {worst['ratio']:.2f} "
      f"(round {worst['round']} trial {worst['trial']})")
print(f"Good trials (<= 1.20x):  {len(good)}/{len(rows)} ({pct(len(good), len(rows)):.1f}%)")
print(f"Elevated (>= {elevated_ratio:.2f}x): {len(elevated)}/{len(rows)} "
      f"({pct(len(elevated), len(rows)):.1f}%)")
print(f"Serial-like (>= {serial_ratio:.2f}x): {len(serial_like)}/{len(rows)} "
      f"({pct(len(serial_like), len(rows)):.1f}%)")
print(f"Rounds with serial-like trials: {len(rounds_with_serial)}/{rounds}")
if rounds_with_serial:
    print("  " + ", ".join(str(r) for r in rounds_with_serial))
print("")
print(f"DATA_CC_W{workers}_MEDIAN_RATIO: {statistics.median(ratios):.2f}")
print(f"DATA_CC_W{workers}_P90_RATIO: {statistics.quantiles(ratios, n=10)[8]:.2f}")
print(f"DATA_CC_W{workers}_GOOD_RATE: {pct(len(good), len(rows)):.2f}")
print(f"DATA_CC_W{workers}_ELEVATED_RATE: {pct(len(elevated), len(rows)):.2f}")
print(f"DATA_CC_W{workers}_SERIAL_LIKE_RATE: {pct(len(serial_like), len(rows)):.2f}")
print("=================================================================")
PY
