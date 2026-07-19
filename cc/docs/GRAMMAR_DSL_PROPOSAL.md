# `@grammar` — user-definable declaration kinds (proposal)

**Status:** Seam implemented (v0). Two builtin engines are live in
`cc/src/preprocess/grammar_rules.c`:
`@grammar(rules)` (Rebol/PARSE dialect → specialized matchers; match / parse
(tape DOM) / collect tiers, derived-restore backtracking, codec provenance) and
`@grammar(schema)` (typed direct-to-struct over a `use`d rules grammar: fields
bound at event sites via extract-mode keeps, key-length dispatch for unordered
members, unknown values skipped through the match tier, `items Schema` arena
arrays). Schema dialect: `use G`; **composition first** — `G.rule [ "k" term
... ]` narrows a member-list rule (delimiters, pads, key/kv shape, and the
unknown-member skip all DERIVED from the rule's decomposed structure) and
`f: G.rule of S` narrows a list rule to an arena array of schema S; plus
product forms `f: G.rule`, `f: int G.rule`, count-driven `f: bytes len` /
`f: items S count` (length-prefixed formats), and incremental `Name_read`.
Explicit `fields:`/`items:` directives remain as the fallback for grammars
not in narrowable shape. **Composition is optional, never a toll**: a schema
may carry an inline `rules [ ... ]` section instead of `use` (self-contained
single block, bare rule refs), and a rules grammar may `include Other` to
splice a previously declared grammar's rules verbatim (entry point stays the
includer's first own rule). All composition — use / inline / include /
narrowing — resolves at compile time to the same flat specialized C; nothing
parses through an intermediate.

**The factory model** (define → specialize → instantiate): a grammar is a
comptime factory — including a FILE artifact via `include "path.rules"`
(path relative to the including source; works in rules blocks and inline
schema sections alike, so one factory file serves many TUs). Schemas can
compose with a file factory directly: `use "path.rules" as Name` (shared —
keyed by resolved path, matchers emitted once per file no matter how many
schemas or aliases use it). The verbs are the semantics: **`include` =
copy** (the rules become yours; a block-level definition SHADOWS an
included one — that is how a factory is specialized without forking its
file; among includes the first wins, so diamonds are legal; overrides never
claim the entry point) and **`use` = share** (one matcher set, qualified
references). Every use is a compile-time specialization, and tiers are
**demand-gated**: a rules block
emits a projection (match / DOM+collect) only if the file references its
entry points (`Name_match`, `Name_parse`, `Name_collect`, `NameNode`) — the
declaration alone stamps nothing but the type and rule count. Runtime
instances are cursors: every schema emits `NameReader` +
`Name_reader(s,n,arena)` / `Name_next(&r,&out)` / `Name_at_end(&r)` — state
only; all behavior was specialized at compile time.
**Format (the write side)**: formatable schemas emit their inverse —
`Name_write(&v, dst, cap)` / `cc_write` / `Name.write(...)` returns bytes
written (0 = didn't fit). Length/count fields consumed by later
`bytes`/counted-`items` terms are DERIVED from the data (`data.len`,
`items_n`), so output is correct by construction; matcher terms emit
nothing (canonical). Narrowed schemas format too: the structural skeleton
(braces, quoted keys, delimiters) is derived from the SAME rules the parse
side narrows, and a `keep/decode(dec)/encode(enc)` codec declares its
inverse — contract `size_t enc(p, n, dst, cap)`: always returns the exact
encoded size; `dst == NULL` measures; with a dst, writes at most `cap`.
Three projections are emitted per schema: `__wmeasure` (exact size, no
stores — the skeleton folds to one constant, digits count by compare
chain), `__wput` (unchecked; the `to_str` back end: measure once, allocate
exactly, put), and `__wchk` (single-pass checked — `Name_write`; constant
runs check once per batch, encoders see the real remaining cap, so
encoder-heavy formats pay ONE encoding pass). Benchmarked at parity with
hand-rolled unchecked encoders on both RESP (~3.0 GB/s) and canonical-JSON
Feed formatting (~1.0 GB/s), gcc -O2. Round-trip tested on RESP and JSON
(parse -> write reproduces wire bytes; corrupted stored counts ignored;
write -> reparse preserves semantics). Directive-built `fields [...]`
schemas have no rule to invert and error clearly if write is demanded.
**The access model — keys separated from values** (one idea, three binding
times): a schema struct is packed C, and the key->field knowledge the
parser dispatches on is KEPT, not consumed. (1) COMPTIME: `cc_get(Tweet,
&v, "id", &out)` — a static `Tweet__fields[]` {name, kind, offset} table
(the comptime hidden class) plus a compiled length-switch+memcmp dispatch;
a literal key constant-folds to a member load. Presence is real where
absence is possible: dispatching schemas carry a bitmap set during parse
(`out.present`); product schemas report 1. (2) PARSE TIME: `cc_dom(Json,
s, n, reg, arena, &out)` — the SHAPED DOM (`<ccc/cc_shape.cch>`), V8-style
hidden classes discovered as data streams through: instances carry 16 B
value slots only; keys live in a persistent `CCShapeReg` trie shared
across parses (twitter.json: 147 shapes, 100% transition-cache hits after
warmup, shapes reused verbatim). Object-vs-array classification derives
from the SAME narrowing analysis the schema tier uses; construction is
FUSED into the tape build at container END markers (bottom-up, cache-hot,
no post-pass; completed containers hand values up an anchored stack so
derived restore unwinds them like keeps; dirty leaves decode at END, so a
codec failure is an ordinary branch failure). (3) DEGRADED: map-shaped
data (high-cardinality keys) must not explode the trie — depth/width caps
switch an INSTANCE to a per-object dictionary (known transitions at a
diverged site still ride the trie); the shape cap degrades, never fails.
Access: `cc_shape_get` = one probe + one memcmp against the shape's
shared table; prepared keys (`cc_shape_key`/`get_k`) hash once for loops.
Deliberately NO inline-cache tier: comptime `get` owns that niche.
**Streaming (the Result face)**: `cc_try_read` speaks the SAME contract as
channel recv — `bool !>(CCError)`: Ok(true) = frame parsed, pos advanced;
Ok(false) = clean end AT a frame boundary (pos == n), the graceful close;
Err(CC_ERR_WOULD_BLOCK) = partial frame, pos unmoved — refill and retry,
and the CALLER (who alone knows whether more bytes can exist) escalates to
truncation when the source is closed; Err(CC_ERR_PARSE) = malformed with
the evidence in hand. Bounds-vs-content is decided EXACTLY at product
terms (literals, fused int runs, `bytes`, counted items); matcher-tier
failures report Err conservatively. A parser drain loop reads identically
to a channel drain loop, and cancellation/timeout/closed pass through the
parse stage as ordinary CCError values in the same Result plumbing. The
generated entry is emitted as `!>` SURFACE syntax — the result-type
rewriter runs after the grammar seam, so generated code speaks the
language's own idiom.
**Zero-alloc hot paths**: `f: items S count cap N` gives the count field a
comptime capacity — the field becomes an INLINE array in the struct
(`RespArg args[4]`), the parse allocates NOTHING, and a larger count is a
protocol ERROR (the policy the real redis server itself applies); uncapped
items keep the arena path. Fused integer binds: an `int` bind whose rule
is `keep [opt '-' some digit]` (detected structurally, refs resolved)
emits ONE accumulate-while-scan loop with range compares — the hand
parser's inner loop, derived from the grammar.
**Tagged unions (`one of`)**: alternation narrowing, option B — a schema
whose body is `one of [ variant [ product-terms ] ... ]` lowers to
`typedef enum { Name_variant, ... } NameKind` + `struct Name { NameKind
kind; union { ... } u; }`. Dispatch is the first byte of each variant's
leading literal (comptime-verified distinct); at most one variant may
lack one — the DEFAULT arm (redis framed|inline commands). Variants are
product schemas; recursion (RESP reply arrays) uses the arena items form
with a depth guard — `cap` on a self-reference is a compile error, as is
ambiguous dispatch. try_read semantics carry through: unknown first byte
is PARSE, truncation anywhere inside a variant is WOULD_BLOCK. v1 is
read-side; write inversion (switch on kind) is the next increment, and
the `G.altrule [...]` qualified form (variant names = branch rule names,
dispatch from the grammar's FIRST sets) layers on this same lowering.
**Acceptance against real code**: `tests/redis_resp_gen_parity_smoke.ccs`
includes the real project's hand RESP reader
(`real_projects/redis/redis_resp.cch`, UNCHANGED) and proves a generated
wrapper behaviorally identical on identical streams at chunk sizes
1..4096 — pipelines, inline commands, binary payloads, malformed input,
over-cap, truncated tails. ~120 lines of hand protocol code vs a 12-line
schema + ~35-line adapter. Benchmarks (same-window interleaved, plain
-O2; window drifts +-10% between sessions so only interleaved ratios are
trusted): RESP parse gen ~3155 MB/s vs hand ~3740 (84%; the remainder is
the CCSlice provenance contract), RESP encode CHECKED gen at parity with
unchecked hand (~2.5 GB/s window-dependent); JSON write gen == hand ==
beats yyjson_mut_write on identical bytes (with a table-driven escape
codec); JSON parse: match/schema ~90% of the golden tape, shaped DOM ~80%
of tape (the delta IS the shaping work), tape DOM ~1.25x yyjson default.
Method note kept on purpose: one measured "+8%" turned out to be a
frame-pointer-handicapped control AND a detector that never fired —
re-measured honestly it was +17%, then +14% more from cap. Interleave or
don't believe.
**No magic names**: the call-site surface is the `cc_*` operations in
`<ccc/cc_grammar.cch>` (in the prelude) — `cc_match(Json, s, n)`,
`cc_parse(Tweet, s, n, arena, &out)`, `cc_reader`/`cc_next`/`cc_at_end` —
uniform across every grammar/schema and recognized by the demand gate. The
`Name_*` functions they expand to are the documented lowering contract, and
every generated splice begins with a MANIFEST comment listing exactly which
operations its declaration supports.
Smokes: `tests/grammar_rules_*.ccs` (incl. `_dom_shaped_smoke`: shaped DOM
vs tape cross-validation, registry persistence, dict fallback),
`tests/grammar_factory_smoke.ccs` (file factory, demand gating, Reader),
`tests/grammar_schema_twitter_smoke.ccs` (composed vs DOM cross-validation,
composed vs directive agreement, write round-trips),
`tests/grammar_schema_resp_smoke.ccs`,
`tests/grammar_schema_resp_stream_smoke.ccs` (byte-at-a-time try_read
contract, cap layout/policy), `tests/redis_resp_gen_parity_smoke.ccs`
(acceptance vs the real project's hand reader), plus 12 negative
`.ccs`/`.compile_err` pairs. Benchmarks: `examples/serdes/json/bench.sh`
(`-a` = full ladder: golden tape / yyjson / generated tiers / write /
shapes prototype / engine shaped DOM) and `examples/serdes/resp/bench.sh`
(gen vs hand, parse + encode).
The capture-and-route rewrite lives in `cc/src/preprocess/grammar_seam.c`
(first step of `cc_comptime_prepare_source`) and lowers
`@grammar(engine) Name {SENT … SENT}` to a synthesized `@comptime` block calling
the registered engine. **v0 engine contract:**
`@comptime void engine(CCSlice name, CCSlice body, const char* file, int line)`.
Body bytes are captured verbatim (heredoc fence; one sentinel-terminating
whitespace char consumed, CRLF-aware); origin honors `#line` directives; the
replaced span keeps its physical line count. Smokes:
`tests/grammar_seam_echo_smoke.ccs` (verbatim bytes + origin) and
`tests/grammar_seam_bad_fence_fail.ccs` (whitespace-after-`{` rejection).
**SERDES acceptance target:** the future `rules` engine must emit code that
matches or beats the hand-lowered golden output in
`examples/serdes/json/json.h` (benchmarked vs yyjson; keep that file as the
engine's golden test).
**Audience:** anyone designing the comptime surface or SERDES.
**Related:** [`spec/cc_serdes.md`](../../spec/cc_serdes.md) (the first consumer),
[`COMPTIME_INSTANTIATION_SEAM.md`](COMPTIME_INSTANTIATION_SEAM.md) (the
emission/factory machinery this reuses),
[`COMPTIME_CAPABILITY_MODEL.md`](COMPTIME_CAPABILITY_MODEL.md) (what any comptime
— including a grammar engine — is allowed to do).

---

## TL;DR

`@grammar(engine) Name { …raw DSL… }` is **one** blessed rewrite that turns a
fenced block of arbitrary text into a generated type. The compiler stays
ignorant of what the block *means*: it captures the opaque bytes and routes
`(name, bytes, origin)` to a named **`@comptime` engine** (a library function).
The engine emits a `Name` type plus specialized `Name_*` functions; use sites are
ordinary UFCS.

The thunderbolt: **`@grammar(engine)` makes the *kind* of a declaration
user-definable.** `struct`/`enum` are a closed set the compiler owns;
`@grammar(rules)`, `@grammar(schema)`, `@grammar(ccSerdes)` are all "a fenced
block processed by engine `X`," where `X` ships in a library. The compiler
blesses exactly one rewrite and gets an open-ended family of declaration kinds in
return. This **dissolves the builtin `@grammar` modes** in `cc_serdes.md`
(`fragments`/`rules`/`schema`) into three stdlib engines sitting in the same
parens.

---

## Grammar

```ebnf
grammar-decl   ::= "@grammar" "(" engine ")" Name raw-block
engine         ::= identifier        # a @comptime fn (library); NOT a builtin mode
Name           ::= identifier        # the declared name; becomes a generated *type*

# raw fenced body — heredoc-style; the only new lexing:
raw-block      ::= "{" sentinel  <bytes…>  sentinel "}"
sentinel       ::= token             # chars from '{' to first whitespace; must be non-empty
```

Body rules:
- The body is captured **verbatim** until the matching `sentinel}` — no interior
  tokenization, so it may contain `{`, `}`, quotes, anything. The user picks a
  sentinel that doesn't collide (heredoc/`<<EOF` discipline).
- `{` must be followed by a real sentinel token (disallow whitespace
  immediately after `{` in raw mode) so it can't be confused with a plain brace
  block.
- The body start `{file, line, col}` is recorded. Note the compiler only knows
  the **fence start** — mapping an error in generated `Name_*` C back to a
  specific *line inside the DSL body* requires the **engine** to thread
  byte-offset → source-position through its own parse and stamp each emitted
  fragment. That interior provenance is engine work, not a free property of the
  seam (see [Costs & caveats](#costs--caveats)).

Why this shape:
- **Engine first** — a forward-scanning highlighter must know the body's language
  before it reaches the body.
- **Name before body** — declaration convention: know *what* is declared before
  its definition (`@grammar(engine)` is the kind, like `struct`; `Name` is the
  declared identifier; `{ … }` is the body).

---

## Lowering

At the comptime pass, the declaration lowers to a single engine call — the entire
blessed rewrite:

```c
@grammar(ccSerdes) JSONReader {~~~~
   …raw DSL body…
~~~~}

// lowers to:
ccSerdes(/*name*/ "JSONReader", /*body*/ <raw bytes>, /*origin*/ {file,line,col});
```

The engine runs during the type's elaboration and emits the type plus its
methods (this is the existing comptime instantiation seam producing a named
type):

```c
typedef struct { /* … */ } JSONReader;
int JSONReader_read (JSONReader*, CCFile);
int JSONReader_write(JSONReader*, CCFile);
/* … */
```

---

## Engine contract

The engine is just a `@comptime` function; **its signature is the contract** —
nothing is builtin:

```c
@comptime void ccSerdes(CCSlice name, CCSlice body /*, origin */) {
    // parse `body` (raw fenced bytes of the DSL)
    // emit a `name` type + named, specialized functions:
    cc_emit_format(CC_EMIT_AFTER_PRELUDE,
        "typedef struct { /* … */ } %.*s;\n", (int)name.len, name.ptr);
    cc_emit_format(CC_EMIT_AFTER_PRELUDE,
        "int %.*s_read(%.*s* self, CCFile f) { /* generated */ }\n",
        (int)name.len, name.ptr, (int)name.len, name.ptr);
    // …_write, etc.
}
```

Any `@comptime` fn of that shape (name + raw body + origin → emits a `Name` type
and `Name_*` operations) is eligible to sit in the `@grammar(...)` slot.

---

## Use site

Ordinary, existing machinery — no new resolution, no dispatch table, no VM:

```c
JSONReader* p = @create();
p->read(f);     // static UFCS → JSONReader_read(p, f)
```

Because the engine emits *named, specialized* functions and UFCS binds
`p->read` statically by the type `JSONReader`, the call is a direct call to
specialized C — identical cost to a hand-written `struct + functions`.
Fn-pointers would only be needed if one variable had to switch grammars at
runtime, which never happens: **each grammar is its own type.**

> The `p->read(f)` shape above is illustrative. A given engine's **output
> contract** decides whether operations are UFCS methods (`p->read(f)`) or free
> functions over the generated type (`cc_parse(src, arena, Type)`); SERDES uses
> the latter. Either way they are static calls to specialized C — see
> [First consumer: SERDES](#first-consumer-serdes).

---

## First consumer: SERDES

[`spec/cc_serdes.md`](../../spec/cc_serdes.md) is the first and motivating
consumer of this seam, and the two docs are harmonized:

- **SERDES is a stdlib engine family, not compiler modes.** Its engines are
  spelled `fragments`, `rules`, and `schema` — i.e. `@grammar(rules) Name {…}`
  routes the fenced grammar to the SERDES `rules` engine. The generic
  `ccSerdes`/`ccJSONReader` names elsewhere in this doc are just placeholders for
  "some engine"; SERDES's real engine names are those three.
- **Bodies are raw-fenced.** SERDES grammar bodies (`charset [#'0'-#'9']`,
  `'\\'`, `"\r\n"`, …) are not C; they rely on this proposal's heredoc fence so
  the host lexer never tokenizes them. The plain `{ … }` in the SERDES spec's
  examples is shorthand for the fenced body.
- **Use site = the engine's contract.** SERDES exposes operations as free
  `cc_parse` / `cc_format` / `cc_match` / `cc_collect` over the generated types
  and entries, rather than UFCS methods. That is a per-engine output-contract
  choice (open item #1), not a property the seam dictates.

So: the seam (this doc) is one blessed rewrite; SERDES (the spec) is the first
engine that rides it. Pinning the **engine output contract** — UFCS methods vs
free `cc_*` operations, and how entries namespace under `Name` — is the shared
open item between the two.

---

## Design principles

- **Compiler stays ignorant.** Least the compiler must know = "capture an inline
  raw block, route `(name, bytes, origin)` to a named comptime engine."
  Everything else (what a grammar means, provenance-in-grammars, directionality)
  is library.
- **Locality is structural, from the name + fence — not the engine.** What
  restores locality is `@grammar(engine) Name {…}` being a *declaration kind*
  (peer of `struct`/`enum`); the engine is swappable plumbing behind it. And the
  locality restored is of the **binding**, not the **content**: a named, fenced,
  greppable decl replaces an ambient `@comptime { fopen(...) }`. The moment a
  body does `include "other.schema"`, non-local truth returns one indirection
  deeper (now gated behind an explicit `cc_depends`). So the honest claim is
  "config gets a name and a fence; staying local is engine discipline, not a
  mechanism guarantee."
- **Subtract the meaning, add a hair of earned signage.** The semantics fully
  reduce to a library; the only new surface is the `@grammar(engine) name {…}`
  form plus the package-and-route rewrite. The sigil earns its keep on **intent**
  (it announces "not ordinary CC here"), **greppability** (audit every generated
  region and every engine), and **coloring** (editors can treat the fenced block
  — even its interior DSL — as a distinct region).
- **REBOL-PARSE lineage, fenced.** Declarative structure in both directions
  (read/write); host code allowed only at **bounded leaves** (codecs must not
  redefine structural behavior). Same elegance as PARSE's paren-escape, with a
  guardrail.
- **Inline data, not an external file.** "Generate code from a data file" is
  already blessed but carries a hermeticity wart (external dependency). Inline
  fenced data has no such wart — the data travels with the source.

---

## Open items

These are the unglamorous parts that only real code retires:

1. **Engine output contract** — precisely what an engine may emit and how its
   symbols namespace under `Name` (the type-generator shape above is the
   leading answer; pin it).
2. **Cross-block composition** — one grammar referencing another
   (`include`/`use` across `@grammar` declarations).
3. **Emit-provenance offset bookkeeping** — mapping byte offsets inside the raw
   body back to `{file,line,col}` so generated-C diagnostics land on the right
   DSL line/column.
4. **Structure/leaf boundary under a TLV-shaped format** and **real protobuf
   semantics** (not just RESP) — the two IOUs that stress the declarative model.

---

## Costs & caveats

Honest accounting — the parts the "one rail, two inputs" framing tends to gloss:

1. **The dispatch/trust boundary differs from `CC_GENERIC_FACTORY`.** It is the
   same seam at the *output* end (`@emit` + UFCS), but the *input* end is
   materially different:

   | | `CC_GENERIC_FACTORY` | `@grammar(engine)` |
   |---|---|---|
   | Trigger | lazy, per use site | eager, once at the decl |
   | Dedup | once per mangled name | n/a (single instance) |
   | Args | compiler-canonicalized **types** | **opaque bytes** the compiler never inspects |

   The grammar engine therefore inherits the *entire* parse / validate /
   canonicalize burden the compiler normally shares with a generic factory.
   "Different config input" undersells this — it is where the new cost lives.

2. **Interior provenance is contingent on the engine.** As above: the compiler
   gives the engine only the fence-start origin. "A bug surfaces as *line 12 of
   your DSL block*" is true only if the engine maps offsets back itself. For
   C-shaped splices (e.g. a `CC_GENERIC_FACTORY` monomorph) the `#line` mapping
   is trivial and already works; for a non-C fenced body it is real engine work.

3. **Engine proliferation = sublanguage proliferation.** The capability unlocked
   is "host arbitrary, library-defined embedded sublanguages." Its failure mode
   is the cost-externalization trap: it looks minimal at the language boundary
   while shipping a learning + tooling cost to every reader who must now know
   `ccJSONReader` vs `ccProtobuf` vs `ccRESP`. The "one story" teachability holds
   at the **use** site; it does **not** hold at the engine-author or
   engine-reader sites. Treat a new engine as a new sublanguage and gate it
   accordingly.

The one-liner worth signing: **`@grammar(engine)` is the same comptime-factory
rail as `CC_GENERIC_FACTORY`, fed decl-site fenced config instead of use-site
type args — which buys back the *binding* locality that a bare
`@comptime { fopen(...) }` loses.** It does *not* by itself make dispatch
identical, DSL-interior diagnostics free, or engine count costless; those are
checks the design still has to earn in code.

---

## Relationship to existing seams

- This is **emission-class** (declaration → generated C via a comptime engine),
  the same class as `CC_GENERIC_FACTORY` / `cc_generic_register`
  (see `COMPTIME_INSTANTIATION_SEAM.md`). It is *not* a text-rewrite-class
  feature.
- Same **output** end (`@emit` → typed `Name_*` monomorph → UFCS), different
  **input** end (decl-site opaque bytes vs use-site canonical type args — see
  [Costs & caveats](#costs--caveats)).
- It reuses, rather than adds: raw-block lexing (one new lexer rule), the
  comptime engine call, `cc_emit_*` for emission, and UFCS for the use site.
- It is **orthogonal** to the capability model: `COMPTIME_CAPABILITY_MODEL.md`
  governs what any comptime — including a grammar engine — is permitted to do;
  this proposal only adds the capture-and-route surface.
