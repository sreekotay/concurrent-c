#!/usr/bin/env python3
"""Race cclev against the installed python-Levenshtein.

Same inputs, same loop, wall time per call.  Upstream is rapidfuzz-backed
C++ with bitparallel cores; cclev is a plain two-row DP behind the same
Python-facing door — the interesting number is the crossing cost plus how
far a straightforward CC port lands from a hand-tuned library.

    ccc build -O real_projects/levenshtein/levenshtein_cc.ccs
    PYTHONPATH=bin python3 real_projects/levenshtein/bench.py
"""
import random
import time

import cclev
import Levenshtein as upstream

rng = random.Random(20260804)
WORDS = ["".join(rng.choice("abcdefghijklmnop") for _ in range(rng.randrange(3, 12)))
         for _ in range(1000)]
LONG = ["".join(rng.choice("abcdefghijklmnop") for _ in range(200)) for _ in range(20)]


def race(label, fn_name, pairs, reps):
    mine = getattr(cclev, fn_name)
    theirs = getattr(upstream, fn_name)
    for f in (mine, theirs):          # touch both before timing
        f(pairs[0][0], pairs[0][1])
    t0 = time.perf_counter()
    for _ in range(reps):
        for a, b in pairs:
            mine(a, b)
    t_mine = time.perf_counter() - t0
    t0 = time.perf_counter()
    for _ in range(reps):
        for a, b in pairs:
            theirs(a, b)
    t_theirs = time.perf_counter() - t0
    n = reps * len(pairs)
    print(f"{label:28} cclev {t_mine/n*1e9:8.0f} ns  "
          f"upstream {t_theirs/n*1e9:8.0f} ns  "
          f"ratio {t_mine/t_theirs:5.2f}x")


def main():
    word_pairs = [(WORDS[i], WORDS[(i * 7 + 1) % len(WORDS)]) for i in range(len(WORDS))]
    long_pairs = [(LONG[i], LONG[(i * 3 + 1) % len(LONG)]) for i in range(len(LONG))]
    race("distance/words(3-12)", "distance", word_pairs, 30)
    race("distance/long(200)", "distance", long_pairs, 30)
    race("ratio/words(3-12)", "ratio", word_pairs, 30)
    race("ratio/long(200)", "ratio", long_pairs, 30)
    race("jaro_winkler/words(3-12)", "jaro_winkler", word_pairs, 30)
    race("hamming/words equal-len", "hamming",
         [(a, "".join(reversed(a))) for a, _ in word_pairs], 30)


if __name__ == "__main__":
    main()
