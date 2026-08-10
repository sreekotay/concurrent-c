# python-Levenshtein, as a CC extension module

The "Python imports CC" door on a real package: `cclev` re-implements the
core of [python-Levenshtein](https://pypi.org/project/Levenshtein/) 0.27
(rapidfuzz-backed) as one `.ccs` file, built by one command, judged by the
real package.

```bash
ccc build -O real_projects/levenshtein/levenshtein_cc.ccs   # → bin/cclev.abi3.so
PYTHONPATH=bin python3 -c "import cclev; print(cclev.distance('kitten','sitting'))"
```

Harness smoke (`tests/py_levenshtein_smoke.shcc`) builds
`levenshtein_cc_smoke.ccs` — same exports, DP only — so ILP32/QEMU does not
cold-compile the bit-parallel specimen.

## Exports

`distance`, `ratio`, `hamming`, `jaro`, `jaro_winkler` — all over
codepoints (`CCPyStr` → `s.codepoints(arena)`), matching upstream:

- `ratio` = `(lensum - indel) / lensum`, indel = substitution-cost-2 DP
- `hamming(..., pad=1)` by default; `pad=0` raises on unequal lengths
  (keyword-only `*` still unspelled — see FRICTION.md)
- `jaro`: both-empty = 1.0, one-empty = 0.0, transpositions halved with
  integer division
- `jaro_winkler`: prefix bonus 0.1 capped at 4, only above the 0.7 boost
  threshold

Module state is one lazily-created scratch arena, reset per call; `destroy`
tears it down as the module's `m_free`.

Three tiers inside, same answers by construction: a textbook two-row DP is
the readable spec and carries small inputs; above an 8-codepoint threshold,
`distance` runs Myers/Hyyrö bit-parallel edit distance and `ratio` runs
Allison–Dix bit-parallel LCS (indel = n + m − 2·LCS), both blocked to
arbitrary lengths over 64-bit words; and where rapidfuzz reaches for C++
templates on the block count, one `@comptime` block in the same file emits
`lev__dist_fx2`..`fx8` and `lev__lcs_fx2`..`fx8` — each kernel unrolled to
a fixed block count, every state word in a register. The generated
specialization is ordinary C, spliced where the block sits, readable in the
lowered output (`--emit-c-only`).

## Referees

```bash
pip install Levenshtein
PYTHONPATH=bin python3 real_projects/levenshtein/parity.py   # 5060 checks, ASCII → astral
PYTHONPATH=bin python3 real_projects/levenshtein/bench.py
```

Parity is the bar: every function against the pip-installed upstream on
fixed edge cases plus 1200 random pairs over widening alphabets and
lengths to 250 — both sides of the scalar/bit dispatch threshold and the
64-codepoint block boundary. `tests/py_levenshtein_smoke.shcc` builds and
imports the module in the main suite without needing pip.

## Cost, honestly (release build, one machine)

Snapshot from `ccc build -O` + `bench.py` on Darwin arm64 (raw output under
`benchmarks/`). Ratios below 1 mean cclev is faster.

| workload | cclev | upstream | cclev/upstream |
|---|---|---|---|
| distance, words 3–12 | 90 ns | 166 ns | 0.55× — cclev faster |
| distance, 200 chars | 1.6 µs | 2.2 µs | 0.71× — cclev faster |
| ratio, words 3–12 | 103 ns | 199 ns | 0.52× — cclev faster |
| ratio, 200 chars | 532 ns | 634 ns | 0.84× — cclev faster |
| jaro_winkler, words 3–12 | 117 ns | 213 ns | 0.55× — cclev faster |
| hamming, equal-len words | 47 ns | 82 ns | 0.57× — cclev faster |

Short strings: cclev wins — the abi3 crossing is cheaper than upstream's
binding layer. Long rows sit at or ahead of rapidfuzz after the bit-parallel
kernels and `@comptime` fixed-block specializations; codepoints arrive via
`CCPyStr.codepoints` (Stable-ABI UCS4) rather than a UTF-8 round-trip.
With the scalar DP alone the long rows read 13× and 35×. (Refresh the table
from a bench run on your machine — the crossing-dominated short rows
especially move with the CPU.)
