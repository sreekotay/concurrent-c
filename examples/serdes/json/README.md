# Hand-lowered zero-copy JSON DOM

`json.h` is the C that a `@grammar(rules) Json` engine would emit for a JSON DOM
parser — written by hand so the lowering is legible. It exists to demonstrate how
Concurrent-C's building blocks (arenas, `CCSlice` provenance, borrow-vs-own) turn
into a real, competitive serde without giving up the safety story.

## What it demonstrates

- **Zero-copy borrow with real provenance.** A string with no escapes borrows the
  source bytes; a string with escapes (or `\uXXXX`) is materialized into the arena.
  The borrow-vs-own distinction is a genuine `CCSlice` provenance bit (the Cow bit),
  not a demo flag. On the `twitter.json` corpus, **98.3% of strings borrow** — copies
  happen only at the escape boundary.
- **Lazy numbers.** A number borrows its text and is parsed on demand
  (`JsonNode_as_f64`) — a second zero-copy that eager parsers give up.
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
```

`bench` reports MB/s, µs/parse, zero-copy rate, node size, and a correctness
checksum (stable per parse, independent of iteration count).

## Corpora

- `twitter.json` — the standard ~630 KB nested-object benchmark file (string-heavy).
- `numbers.json` — 30k floats (array-heavy). Regenerate a larger one with
  `python3 tools/gen_numbers.py 300000 > numbers.json`.
- `python3 tools/minify.py < twitter.json > twitter_min.json` strips structural
  whitespace *without* re-encoding strings — useful because pretty-printing
  (twitter.json is 26.6% whitespace) otherwise dominates the parse.

## Comparing against yyjson (optional)

The comparison isn't vendored. To enable `./bench.sh -y`, drop `yyjson.c` and
`yyjson.h` (from <https://github.com/ibireme/yyjson>) into this directory.

Rough standing (best-of, this parser vs yyjson-default, minified inputs so
whitespace-skipping doesn't mask either side): competitive on string-heavy input,
~within 15% on number-heavy. yyjson's remaining edge is a denser 16-byte tape and
years of micro-tuning — both of which trade away the provenance and simplicity this
example keeps. `insitu` yyjson is faster still but destructively rewrites the input
buffer and eager-parses numbers.

## Files

| file | what |
|------|------|
| `json.h` | the DOM: parser, compact node, accessors |
| `bench.c` | throughput + zero-copy + checksum harness |
| `bench.sh` | build + run driver |
| `yy.c` | optional yyjson comparison harness |
| `tools/gen_numbers.py`, `tools/minify.py` | corpus helpers |
