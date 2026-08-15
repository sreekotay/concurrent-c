# JSON serdes (engine + golden)

**Example path:** `@grammar(rules)` / `@grammar(schema)` over the shared
factory in this directory (`json.rules` recognition, `json_dom.rules` DOM
specialization, `json_codec.cch` for `jstr` / `jstr_enc`). Benches:
`./bench.sh -a` (full ladder), or `-g` / `-y` / `-d` / `-w` / `-s`
individually.

## RFC 8259

`json.rules` is an RFC 8259 JSON-text recognizer: `ws value ws`, the
seven value types, numbers without a leading `+` / leading zero /
trailing dot, and unescaped `U+0000`–`U+001F` rejected in strings.
Escapes (`\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, `\uXXXX`) are
accepted. Clean string spans stay borrowed; UTF-8 well-formedness of a
borrow is a use-site check, not a parse-time copy. `jstr` runs only on
dirty (escaped) spans.

The product schema for that text is `JsonVal` (tagged sum of the seven
types) plus `JsonText` (`ws` + `JsonVal` + `ws`). Closed products such
as `Tweet` / `Feed` are projections of the same factory, not a second
grammar. After a failed `cc_match` / `cc_parse`, `cc_fail_pos()` is the
high-water byte of the miss.

**Golden reference:** `json.h` is the hand-lowered C a rules engine would emit
for a JSON DOM — kept legible so the lowering stays inspectable. It is the
oracle for borrow/materialize splits. Its string scan still accepts raw
controls; `json.rules` does not.

## What it demonstrates

- **Zero-copy borrow with real provenance.** A string with no escapes borrows the
  source bytes; a string with escapes (or `\uXXXX`) is materialized into the arena.
  The borrow-vs-own distinction is a genuine `CCSlice` provenance bit (the Cow bit),
  not a demo flag. On the `twitter.json` corpus, **98.3% of strings borrow** — copies
  happen only at the escape boundary.
- **Lazy numbers.** A number borrows its text and is parsed on demand
  (`JsonNode_as_f64` via [ffc.h](https://github.com/kolemannix/ffc.h)) — a second
  zero-copy that eager parsers give up.
- **Compact 24-byte node** (yyjson-inspired). `meta` packs tag + Cow bit + length in
  one word; object members are stored as `(key, value)` pairs so a value/array
  element carries no key field. The full `CCSlice` — with provenance id and
  sub-slice bound — is **reconstructed on demand** by `JsonNode_slice()`:
  pay-per-use provenance, the same demand-driven idea as lazy numbers.
- **Request-scoped arena.** Nodes and materialized strings share one `CCArena`,
  allocated with the single-owner local tier and reset wholesale between parses.
- **UFCS-shaped API.** `JsonParser_parse(&p, &out)` is `p.parse(&out)`;
  `JsonNode_tag/len/slice/as_f64/get(n, …)` are `n.tag()`, `n.slice()`, etc.

The `// @await` markers show where the `@async` variant would suspend on I/O — the
whole-buffer version here gets resumption for free from the state-machine lowering.

## Run

```bash
./bench.sh              # ours, on twitter.json + numbers.json
./bench.sh 400          # more iterations
./bench.sh -y           # also run yyjson (see below)
./bench.sh -c           # also print a correctness checksum
```

`bench` reports MB/s, µs/parse, zero-copy rate, and node size (parse only —
no post-walk in the timed loop). `-c` adds a deterministic DOM digest computed
by one extra parse+walk *after* the clock stops, so it verifies correctness
without skewing the timing (expected: twitter.json `chk=406072`, numbers.json
`chk=356701`).

## Corpora

- `twitter.json` — the standard ~630 KB nested-object benchmark file (string-heavy).
- `numbers.json` — 30k floats (array-heavy). Regenerate a larger one with
  `python3 tools/gen_numbers.py 300000 > numbers.json`.
- `python3 tools/minify.py < twitter.json > twitter_min.json` strips structural
  whitespace *without* re-encoding strings — useful because pretty-printing
  (twitter.json is 26.6% whitespace) otherwise dominates the parse.

## Comparing against yyjson (optional)

`yyjson.c` / `yyjson.h` (v0.12.0 from <https://github.com/ibireme/yyjson>, MIT)
are checked in so `./bench.sh -y` works out of the box.

Latest full ladder (`./bench.sh -a`, K=200) is
[`benchmark_baseline_2026_08_15.txt`](benchmark_baseline_2026_08_15.txt)
(Darwin arm64). A Linux x86-64 container receipt from July is
[`benchmark_baseline_2026_07_20.txt`](benchmark_baseline_2026_07_20.txt).
Treat ratios within a run; boxes are not interchangeable.

On that Darwin snapshot, generated match is about **0.68×** yyjson-default
on twitter.json (3204 vs 4716 MB/s) and **even** with it on numbers.json
(2066 vs 2062 MB/s). Schema parse of twitter sits between match and
collect; generated write matches the hand unchecked encoder. yyjson's
remaining edge on string-heavy input is a denser 16-byte tape and years
of micro-tuning — both of which trade away the provenance this example
keeps. `insitu` yyjson is faster still on twitter but destructively
rewrites the input and eager-parses numbers. Platform changes who wins:
the July Linux receipt has generated match ahead of yyjson-default.

## Files

| file | what |
|------|------|
| `json.rules` | RFC 8259 recognition factory (`include` this) |
| `json_dom.rules` | DOM specialization (`keep` / `collect` overrides) |
| `json_codec.cch` | `jstr` decode + `jstr_enc` encode |
| `bench_grammar.ccs` | engine match / collect / DOM / schema bench |
| `json.h` | hand golden DOM (lowering oracle; not RFC-strict on controls) |
| `bench.c` | golden throughput + zero-copy harness |
| `bench.sh` | build + run driver (`-a` / `-g` / `-y` / `-d` / `-w` / `-s`) |
| `yy.c` | yyjson comparison harness |
| `yyjson.c`, `yyjson.h` | vendored yyjson 0.12.0 |
| `benchmark_baseline_2026_08_15.txt` | latest full-ladder receipt |
| `tools/gen_numbers.py`, `tools/minify.py` | corpus helpers |
