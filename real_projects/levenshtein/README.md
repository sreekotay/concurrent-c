# python-Levenshtein, as a CC extension module

The "Python imports CC" door on a real package: `cclev` re-implements the
core of [python-Levenshtein](https://pypi.org/project/Levenshtein/) 0.27
(rapidfuzz-backed) as one `.ccs` file, built by one command, judged by the
real package.

```bash
ccc build -O real_projects/levenshtein/levenshtein_cc.ccs   # → bin/cclev.abi3.so
PYTHONPATH=bin python3 -c "import cclev; print(cclev.distance('kitten','sitting'))"
```

## Exports

`distance`, `ratio`, `hamming`, `jaro`, `jaro_winkler` — all over
codepoints (a hand-rolled UTF-8 decode), matching upstream:

- `ratio` = `(lensum - indel) / lensum`, indel = substitution-cost-2 DP
- `hamming` pads unequal lengths (upstream's `pad=True`; cclev cannot spell
  the keyword-only parameter — see FRICTION.md)
- `jaro`: both-empty = 1.0, one-empty = 0.0, transpositions halved with
  integer division
- `jaro_winkler`: prefix bonus 0.1 capped at 4, only above the 0.7 boost
  threshold

Module state is one lazily-created scratch arena, reset per call; `destroy`
tears it down as the module's `m_free`.

## Referees

```bash
pip install Levenshtein
PYTHONPATH=bin python3 real_projects/levenshtein/parity.py   # 5060 checks, ASCII → astral
PYTHONPATH=bin python3 real_projects/levenshtein/bench.py
```

Parity is the bar: every function against the pip-installed upstream on
fixed edge cases plus 1000 random pairs over widening alphabets.
`tests/py_levenshtein_smoke.shcc` builds and imports the module in the main
suite without needing pip.

## Cost, honestly (release build, one machine)

| workload | cclev | upstream | ratio |
|---|---|---|---|
| distance, words 3–12 | 265 ns | 457 ns | 0.58× |
| distance, 200 chars | 74.7 µs | 5.7 µs | 13× |
| ratio, words 3–12 | 270 ns | 352 ns | 0.77× |
| ratio, 200 chars | 68.3 µs | 2.0 µs | 35× |
| jaro_winkler, words 3–12 | 298 ns | 417 ns | 0.71× |

Short strings: cclev wins — the abi3 crossing is cheaper than upstream's.
Long strings: upstream's bitparallel cores beat a readable two-row DP by an
order of magnitude and more. Both numbers are the point: the boundary is
cheap, and a straightforward port is not a hand-tuned library.
