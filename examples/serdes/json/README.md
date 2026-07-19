# JSON serdes (engine + golden)

**Production path:** `@grammar(rules)` / `@grammar(schema)` over the shared
factory in this directory (`json.rules` recognition, `json_dom.rules` DOM
specialization, `json_codec.cch` for `jstr` / `jstr_enc`). Benches:
`./bench.sh -g` (engine tiers), `-d` (shaped DOM), `-w` (write).

**Golden reference:** `json.h` is the hand-lowered C a rules engine would emit
for a JSON DOM — kept legible so the lowering stays inspectable. It is the
oracle for borrow/materialize splits, not the product parser.

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

Rough standing (best-of, parse-only vs yyjson-default, minified inputs so
whitespace-skipping doesn't mask either side): within ~2× on string-heavy input,
closer on number-heavy. yyjson's remaining edge is a denser 16-byte tape and
years of micro-tuning — both of which trade away the provenance and simplicity this
example keeps. `insitu` yyjson is faster still but destructively rewrites the input
buffer and eager-parses numbers.

## Files

| file | what |
|------|------|
| `json.rules` | shared recognition factory (`include` this) |
| `json_dom.rules` | DOM specialization (`keep` / `collect` overrides) |
| `json_codec.cch` | `jstr` decode + `jstr_enc` encode |
| `bench_grammar.ccs` | engine match / collect / DOM / schema bench |
| `json.h` | hand golden DOM (lowering oracle) |
| `bench.c` | golden throughput + zero-copy harness |
| `bench.sh` | build + run driver (`-g`/`-y`/`-d`/`-w`) |
| `yy.c` | yyjson comparison harness |
| `yyjson.c`, `yyjson.h` | vendored yyjson 0.12.0 |
| `tools/gen_numbers.py`, `tools/minify.py` | corpus helpers |
