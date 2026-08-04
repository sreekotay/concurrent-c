#!/usr/bin/env python3
"""Parity: cclev vs the installed python-Levenshtein, judged by upstream.

Every exported function runs against upstream on the same inputs — fixed
edge cases first, then random pairs drawn from alphabets that widen from
ASCII to astral-plane codepoints (so codepoint-vs-byte confusion cannot
hide).  Floats compare within 1e-12: both sides compute in doubles, but
not in the same order.

    ccc build real_projects/levenshtein/levenshtein_cc.ccs
    PYTHONPATH=bin python3 real_projects/levenshtein/parity.py
"""
import random
import sys

import cclev
import Levenshtein as upstream

ALPHABETS = [
    "ab",                        # tiny: forces repeats, transpositions
    "abcdefghij",                # ASCII
    "abcdé çñüö",                # Latin-1-ish two-byte UTF-8
    "αβγδε漢字かな",              # BMP multi-byte
    "a𝕊𝕋𝕌😀😃b",                 # astral plane (surrogate-pair territory)
]

FIXED = [
    ("", ""), ("", "abc"), ("abc", ""), ("abc", "abc"),
    ("kitten", "sitting"), ("flaw", "lawn"),
    ("héllo", "hello"), ("𝕊tring", "string"),
    ("martha", "marhta"), ("dixon", "dicksonx"),
    ("aaaa", "aa"), ("ab" * 50, "ba" * 50),
]

FUNCS = [
    ("distance", 0),
    ("ratio", 1e-12),
    ("hamming", 0),
    ("jaro", 1e-12),
    ("jaro_winkler", 1e-12),
]


def upstream_call(name, a, b):
    if name == "hamming":
        # cclev always pads; upstream 0.27 raises on unequal lengths
        # unless asked.  Padding is the keyword-only extra parameter's
        # behavior, which cclev cannot spell (see FRICTION.md).
        return upstream.hamming(a, b, pad=True)
    return getattr(upstream, name)(a, b)


def check(name, tol, a, b):
    got = getattr(cclev, name)(a, b)
    want = upstream_call(name, a, b)
    ok = got == want if tol == 0 else abs(got - want) <= tol
    if not ok:
        print(f"MISMATCH {name}({a!r}, {b!r}): cclev={got!r} upstream={want!r}")
    return ok


def main():
    rng = random.Random(20260804)
    failures = 0
    total = 0

    pairs = list(FIXED)
    for alphabet in ALPHABETS:
        for _ in range(200):
            n = rng.randrange(0, 24)
            m = rng.randrange(0, 24)
            a = "".join(rng.choice(alphabet) for _ in range(n))
            b = "".join(rng.choice(alphabet) for _ in range(m))
            pairs.append((a, b))
        # Long pairs: cross the bit kernels' 64-codepoint block boundary
        # (and the scalar/bit dispatch threshold) in every alphabet.
        for _ in range(40):
            n = rng.randrange(0, 250)
            m = rng.randrange(0, 250)
            a = "".join(rng.choice(alphabet) for _ in range(n))
            b = "".join(rng.choice(alphabet) for _ in range(m))
            pairs.append((a, b))

    for a, b in pairs:
        for name, tol in FUNCS:
            total += 1
            failures += 0 if check(name, tol, a, b) else 1

    print(f"parity: {total - failures}/{total} ok "
          f"({len(pairs)} pairs x {len(FUNCS)} functions)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
