#!/usr/bin/env python3
"""
Interleaved A/B runner for perf/perf_buffered_base.ccs binaries.

Why this exists:
- Avoid drift bias by alternating A then B, B then A each round.
- Report stable p50/p90 deltas across CC_WORKERS matrix.
- Keep results comparable between optimization attempts.
"""

import argparse
import math
import os
import re
import statistics
import subprocess
import sys
from typing import Dict, List


MEDIAN_RE = re.compile(r"median:\s+([0-9]+)\s+ops/sec")


def run_once(binary: str, workers: int) -> int:
    env = dict(os.environ)
    env["CC_WORKERS"] = str(workers)
    out = subprocess.check_output([binary], text=True, env=env)
    match = MEDIAN_RE.search(out)
    if not match:
        raise RuntimeError(f"Could not parse median ops/sec from output of {binary}")
    return int(match.group(1))


def pct_delta(new_value: int, old_value: int) -> float:
    if old_value == 0:
        return math.nan
    return ((new_value - old_value) / old_value) * 100.0


def percentile(values: List[int], p: float) -> int:
    idx = max(0, min(len(values) - 1, int(len(values) * p) - 1))
    return sorted(values)[idx]


def summarize(values: List[int]) -> Dict[str, int]:
    s = sorted(values)
    return {
        "p50": s[len(s) // 2],
        "p90": percentile(s, 0.90),
        "min": s[0],
        "max": s[-1],
        "mean": int(statistics.mean(s)),
    }


def run_ab(bin_a: str, bin_b: str, workers: int, rounds: int) -> Dict[str, Dict[str, int]]:
    vals_a: List[int] = []
    vals_b: List[int] = []

    for i in range(rounds):
        if i % 2 == 0:
            vals_a.append(run_once(bin_a, workers))
            vals_b.append(run_once(bin_b, workers))
        else:
            vals_b.append(run_once(bin_b, workers))
            vals_a.append(run_once(bin_a, workers))

    return {"A": summarize(vals_a), "B": summarize(vals_b)}


def parse_workers(spec: str) -> List[int]:
    out = []
    for piece in spec.split(","):
        piece = piece.strip()
        if not piece:
            continue
        out.append(int(piece))
    if not out:
        raise ValueError("workers list is empty")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Interleaved A/B for buffered benchmark binaries.")
    parser.add_argument("--bin-a", required=True, help="Path to binary A")
    parser.add_argument("--bin-b", required=True, help="Path to binary B")
    parser.add_argument("--name-a", default="A", help="Label for binary A")
    parser.add_argument("--name-b", default="B", help="Label for binary B")
    parser.add_argument("--workers", default="1,2,8", help="Comma-separated worker counts")
    parser.add_argument("--rounds", type=int, default=30, help="Number of interleaved rounds per worker")
    args = parser.parse_args()

    if args.rounds < 2:
        print("rounds must be >= 2", file=sys.stderr)
        return 2

    workers = parse_workers(args.workers)
    print(f"Buffered A/B: {args.name_a} vs {args.name_b} | rounds={args.rounds} | workers={workers}")
    print("-" * 96)
    print(
        f"{'W':>3}  "
        f"{args.name_a + ' p50':>14}  {args.name_b + ' p50':>14}  {'delta p50':>10}  "
        f"{args.name_a + ' p90':>14}  {args.name_b + ' p90':>14}  {'delta p90':>10}"
    )
    print("-" * 96)

    for w in workers:
        stats = run_ab(args.bin_a, args.bin_b, w, args.rounds)
        a = stats["A"]
        b = stats["B"]
        d50 = pct_delta(a["p50"], b["p50"])
        d90 = pct_delta(a["p90"], b["p90"])
        print(
            f"{w:>3}  "
            f"{a['p50']:>14,}  {b['p50']:>14,}  {d50:>+9.2f}%  "
            f"{a['p90']:>14,}  {b['p90']:>14,}  {d90:>+9.2f}%"
        )

    print("-" * 96)
    print("delta is (A - B) / B; positive means A is faster.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
