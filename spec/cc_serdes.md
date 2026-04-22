# Concurrent-C Grammar and SERDES Specification

**Version:** 0.2-draft  
**Date:** 2026-04-21  
**Status:** Concept-phase draft (inclusion-oriented language and lowering intent)

---

## Goals and Scope

Concurrent-C grammar and SERDES provides lightweight recognition grammars and
schema-driven parsing and formatting for structured text and binary formats. It
centers on:

1. **One unified declaration family:** `@grammar(fragments)`, `@grammar(rules)`,
  and `@grammar(schema)` (see [Surface: `@grammar(…)](#surface-grammar)`).
2. **Runtime operations:** `cc_parse` / `cc_format` for schema-driven I/O, plus
  companions such as `cc_match` / `cc_collect` for rule entry points when the
   language contract exposes them.

Conceptual names in prose — **fragments**, **rules**, **schema** — map to those
three `@grammar` modes. Older drafts used type-like spellings (`CCFragments`,
`CCRules`, `CCSchema`); this document treats them as the same three layers.

**Design intent:** keep the grammar declarative, keep ownership truthful, keep
lowering transparent, and keep the generated hot path competitive with (or
better than) handwritten C code.

**Scope:** This document defines the shared grammar model, semantic rules for
recognition and structured SERDES, the role of codecs, directionality,
provenance, error behavior, and the intended lowering model. Exact concrete
syntax is still evolving; semantics and layering are the primary contract.

**Related surface:** write-only string templating without a schema (for example
`@string` in earlier sketches) remains a separate convenience facility and is
not a `@grammar` kind.

---

## Design Principles

1. **One grammar family, two weights.** Under `@grammar`, **rules** are
  lightweight recognition and collection; **schema** is typed structural SERDES
   on the same conceptual foundation. **Fragments** are substitution-only and sit
   beneath both.
2. **One structural schema, two generated operations.** `@grammar(schema)`
  describes wire structure from which the compiler may generate both parse and
   format code.
3. **Structure over callbacks.** Grammar semantics belong to rules and schemas,
  not to codec callbacks.
4. **Leaf hooks only.** Codecs are lightweight bidirectional escape hatches for
  primitive or domain-specific behavior.
5. **Truthful provenance.** Parsed values must accurately reflect whether they
  borrow from source input or were materialized into an arena.
6. **Canonical formatting.** Formatting emits bytes from the schema's output
  structure; it is never defined as "running the parser backwards".
7. **Structured errors.** Parse and format failures are positioned, typed, and
  grammar-aware.
8. **Transparent lowering.** Implementations should generate direct, specialized
  ordinary C code with no parser VM requirement in the hot path.

---

## Surface: `@grammar(…)` {#surface-grammar}

Declarations use a single keyword `**@grammar`** with a **mode** that selects
semantics. `**@grammar` owns pattern and structure** (what to match, bind,
repeat, and emit). **Codecs are not `@grammar` blocks** — they remain leaf hooks
(primitive or domain behavior attached at fields or call sites), not a fourth
`@grammar` kind.


| Mode          | Surface                          | Role                                                                                                                                               |
| ------------- | -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| **fragments** | `@grammar(fragments) Name { … }` | Replacement-only snippets (CSS-custom-property spirit). Expanded before semantic analysis. No parse entry points, no match/rollback by themselves. |
| **rules**     | `@grammar(rules) Name { … }`     | Recognition and collection. Parse-first; `cc_match` / `cc_collect` (or equivalent) over named entries.                                             |
| **schema**    | `@grammar(schema) Name { … }`    | Typed wire model: named fields, directionality, `**cc_parse`** / `**cc_format**`, provenance-aware output.                                         |


**Rule:** `**@grammar` is for declarations.** `**cc_*` is for operations** over
the types and entry points those declarations introduce.

---

## Conceptual Stack

Read the system bottom-up:

1. **Fragments** (`@grammar(fragments)`) — compile-time substitution only. No
  match, commit, rollback, provenance, or entry-point semantics by themselves.
2. **Rules** (`@grammar(rules)`) — runtime recognition and collection on bytes
  (or a chosen input model). Parse-oriented; no generated format unless a
   future extension says otherwise.
3. **Schema** (`@grammar(schema)`) — runtime typed structural parse and
  canonical format over the same grammar vocabulary, plus named fields and
   directionality.
4. `**cc_parse` / `cc_format`** — stable call-site operations over schema types
  (and companions such as `cc_collect` / `cc_match` for rule entry points, if
   exposed by the language contract).

**Rule:** After fragment expansion, only **rules** and **schema** contribute
semantic meaning. Fragments must not be mistaken for named productions with
independent identity.

---

## Fragments (replacement-only)

**Fragments** (`@grammar(fragments)`) are **named replacement tokens**, analogous
in spirit to CSS custom properties: they exist to reduce repetition and keep
grammars readable. They are **not** a parallel grammar language and **not**
first-class rule definitions.

**Properties:**

- Fragment bodies expand **before** grammar analysis (exact pipeline stage is
implementation-defined, but expansion is strictly prior to match semantics).
- They **do not** introduce parse entry points, generated types, or standalone
productions.
- They **do not** perform input consumption, backtracking, or collection by
themselves; the surrounding `@grammar(rules)` or `@grammar(schema)` block does.
- **Provenance** and **errors** are attributed to the expanded site inside the
containing rule or schema, not to the fragment definition in isolation.

**Rule:** A `use FragmentName` (or equivalent import) brings names from that
fragment namespace into scope for substitution. References such as
`FragmentName.symbol` expand to the fragment's right-hand side at the use site.

**Rule:** If a design needs **runtime** reuse with full grammar semantics (e.g.
a leaf recognizer referenced from a schema), that is **rules**
(`@grammar(rules)`), not fragments.

Illustrative shape:

```c
@grammar(fragments) JsonCommon {
    ws:    any charset [#' ' #'\t' #'\r' #'\n']
    digit: charset [#'0' - #'9']
}

@grammar(rules) JsonLex {
    use JsonCommon
    number = [ opt #"-" some digit /* ... */ ];
}
```

**Open question (concept):** whether fragments may contain `keep` / `collect` or
must be restricted to character-level and literal patterns only. A strict
reading is: fragments are **syntactic** sugar for repeated pattern text;
collection semantics remain owned by **rules** and **schema**.

---

## Shared Grammar Model

**Rules** and **schema** (`@grammar(rules)` and `@grammar(schema)`) are two layers
of the **same** grammar family. They
should share core vocabulary wherever possible:

- literals
- named character sets
- grouping
- ordered choice
- repetition (`some`, `any`, `opt`)
- position/search operators such as `to` and `thru`
- named **rule** references (`Namespace.rule` for entries declared in
`@grammar(rules)`)
- collection helpers (`collect`, `keep`, `skip`)
- optional binary-oriented primitives such as `bit` and `bits(N)` (concept;
endian, alignment, and bit ordering require separate normative text)

**Rule:** Schema should feel like the typed structural layer built on the same
matching model as rules, not like a separate ad hoc DSL.

**Rule:** Rules are parse-only by default. Schema is the structured layer that
participates in both parse and format generation.

---

## Rules (`@grammar(rules)`)

`@grammar(rules)` declares lightweight recognition and collection grammars: tokenizers,
lexers, scanners, comment skipping, span extraction, and similar problems where
generated domain structs would be unnecessary weight.

Rules are:

- named
- composable
- recognition-first rather than type-first
- collect-driven rather than field-driven

Example:

```c
@grammar(rules) CssRules {
    nonascii: complement charset [0 - 177];
    hexa:     charset [#'a' - #'f' #'A' - #'F' #'0' - #'9'];
    nmstart:  charset [#'_' #'a' - #'z' #'A' - #'Z'];
    nmchar:   charset [#'_' #'a' - #'z' #'A' - #'Z' #'0' - #'9' #'-'];

    escape:   ['\\' [notesc | 1 6 hexa]];
    ident:    [opt '-' nmstart any nmchar];

    tokenize: collect any [
          "/*" thru "*/" skip
        | keep ident
        | keep skip
    ];
}
```

Typical rule-oriented entry points:

```c
char[:][:] tokens = cc_collect(src, arena, CssRules.tokenize) ?>(e) return cc_err(e);
bool ok = cc_match(src, CssRules.ident) ?>(e) return cc_err(e);
```

**Rule:** `@grammar(rules) Name { ... }` introduces a named rule namespace.
Individual rules are referenced as `Name.rule` in grammar and call sites.

**Rule:** The `Name.rule` form is **grammar-entry selection**, not ordinary field
access.

### Rules collection semantics

Rules are collection-driven:

- if a rule consumes input but is not kept, it does not appear in the output
- `skip` consumes without output
- `keep` emits the matched or derived value into the collected result
- `collect` defines a sequence-producing rule

**Rule:** Rules do not generate domain structs by default.

**Rule:** Output shape is determined by the kept values of the selected entry
rule, not by named fields.

By default, homogeneous kept output (e.g. `char[:][:]` spans) is sufficient for
many tokenizers.

**Rule:** Heterogeneous collected output is not required for rules v1.

**Future direction:** user-supplied emitted token types for collection (tagged
tokens) without promoting rules into full schema. Recursive structural domain
modeling remains the responsibility of schema.

---

## Schema (`@grammar(schema)`)

`@grammar(schema)` declares the structural wire model for a format. A schema may be:

- a product/sequence schema
- an alternation schema
- a forward declaration for recursive schemas

Example (length prefix is parse-visible; value borrows or materializes per
provenance rules):

```c
@grammar(schema) RespBulkString {
    '$' @parse_only len: int "\r\n"
    value: char[:len] "\r\n"
};
```

Schema adds:

- named typed fields
- generated output types
- directionality
- schema-driven formatting
- provenance-aware storage semantics

---

## `cc_parse` and `cc_format`

`cc_parse(src, arena, Type)` parses bytes from `src` into a value of schema type
`Type`.

Optional parse options may be supplied at the call site, for example borrowing
hints where legal:

```c
RespBulkString s = cc_parse(src, arena, RespBulkString, .borrow_from_src) ?>(e) return cc_err(e);
```

`cc_format(value, arena, Type)` formats a schema value into **canonical** bytes,
returning a string-like output type chosen by the language/library contract:

```c
CCString out = cc_format(v, arena, RespValue) ?>(e) return cc_err(e);
```

The concrete return type of `cc_parse` (pointer vs value vs arena-backed handle)
is a lowering contract; this document requires only that ownership and
provenance remain explicit in the generated API.

---

## Rules vs schema

- **Rules** (`@grammar(rules)`) — recognition, scanning, tokenization, collection
- **Schema** (`@grammar(schema)`) — typed structural parse and format generation

**Rule:** An implementation should not force tokenizer-style grammars through
struct generation when a rule grammar is sufficient.

**Rule:** Schema remains a structured superset in spirit; it need not expose
every rule-only collection convenience in identical form.

### Cross-referencing (not fragments)

The two layers compose, but composition is **directional** in v1.

**Rule:** Schema may reference a **rules** entry (`Namespace.rule`) as a
parse-side leaf recognizer or extractor when that rule's output shape is
schema-compatible (a real grammar invocation, not fragment expansion).

Example:

```c
@grammar(schema) CssRule {
    selector: CssRules.ident
    '{' props: keep(some) CssProperty '}'
};
```

**Rule:** Rules do not consume full schemas in v1.

**Rule:** Generalized schema parsing over arbitrary token streams is not defined
in v1. Byte-oriented grammar is the normative input model for this draft.

**Future direction:** multi-phase pipelines (bytes → rule tokens → later parse)
require an explicit stream-type grammar layer; they are not implied here.

---

## Grammar Semantics vs Codec Semantics

**Hard rule:** grammar semantics belong to **rules** and **schema**
(`@grammar(rules)` and `@grammar(schema)`), not to the codec.

Rules and schemas own:

- sequencing, literals, ordered choice, repetition, recursion
- collection shape, field binding, structural output layout

The codec owns only **leaf-level** domain behavior, for example:

- primitive numeric parse/format
- string escaping and unescaping
- whitespace policy
- domain-specific diagnostics

**Rule:** A codec may refine how primitive byte sequences are interpreted or
emitted, but it must not redefine structural behavior: branching, rollback,
repetition, recursion, or ownership.

**Rule of thumb:** local meaning of bytes → codec; match structure and storage
→ grammar and runtime semantics.

---

## Codec Model

A codec is the bidirectional domain hook for schema-driven SERDES, analogous in
spirit to policy arguments used by lightweight templating APIs. Codecs attach
at **leaves** (fields, primitives, call-site options), not as `@grammar(…)`
blocks — see [Surface: `@grammar(…)](#surface-grammar)`.

Codec hooks must not become a second grammar language.

**Rule:** Implementations should reject or warn on codec patterns that attempt to
control general grammar flow instead of leaf behavior.

---

## Directionality

Only `**@grammar(schema)`** participates in generated formatting.
`**@grammar(rules)**` is parse-only unless a future extension states otherwise.

Schema elements participate in one or both directions:

- **bidirectional** — parse and format
- **parse_only** — read only
- **format_only** — write only

**Rule:** Directionality is part of schema semantics. A schema used with
`cc_format` must have a complete format path; ambiguous cases are compile-time
errors unless explicitly recoverable.

### Legality table


| Schema element                           | Parse                 | Format                   | Default direction             |
| ---------------------------------------- | --------------------- | ------------------------ | ----------------------------- |
| Literal char/string/bytes                | match                 | emit                     | bidirectional                 |
| Named primitive field                    | parse                 | format                   | bidirectional                 |
| Named nested-schema field                | parse                 | format                   | bidirectional                 |
| Ordered sequence                         | in order              | in order                 | bidirectional                 |
| Ordered choice (PEG alternation)         | try branches in order | branch from tag/value    | bidirectional                 |
| Repetition with explicit count           | repeat                | emit                     | bidirectional                 |
| Collection `keep(...)`                   | collect               | emit if shape defined    | bidirectional when defined    |
| Hidden driver field                      | guide parse           | omit unless mapped       | parse_only                    |
| `to ...` / `thru ...` / delimiter search | advance/locate        | no implicit inverse      | parse_only                    |
| Whitespace skipping                      | consume               | emit only if defined     | parse_only by default         |
| Guarded `opt (expr)`                     | conditional           | conditional if evaluable | bidirectional if well-defined |
| Derived emitted field                    | skip read             | emit from value          | format_only                   |


### Directionality rules

1. Literals and named value fields are symmetric by default.
2. Hidden fields (e.g. length prefixes) are parse-only by default unless the
  format path re-derives or maps them.
3. Search operators do not imply inverse emission; `**cc_format` is structural,
  not inverse parse.**
4. **Choice must be format-resolvable** (tag, discriminant, or distinct output
  types).
5. **Reject ambiguous format schemas** when `cc_format` cannot emit a unique
  byte sequence from the value.

### Suggested surface markers

```c
@parse_only len: int
@format_only checksum: uint32
```

---

## Provenance and Materialization

When `cc_parse` produces slices or nested values, the implementation must preserve
**truthful provenance**: borrow from input or materialize in the arena.

**Rule:** Provenance is observable storage truth, not a hint.

### Borrow vs materialize (summary)


| Construct                                     | Default                                     |
| --------------------------------------------- | ------------------------------------------- |
| Fixed-length or delimiter slice, no transform | borrow if legal                             |
| Escaped / decoded / normalized text           | materialize                                 |
| Primitives / binary integers                  | stored as values                            |
| Recursive containers                          | may allocate nodes; leaves may still borrow |


### Provenance rules

1. Borrowing only when the value is a **direct contiguous view** of source bytes
  without transformation.
2. Any byte rewrite → materialize in `arena`.
3. Codecs do not invent borrowed provenance for transformed bytes.
4. Borrowed slices reference **input** provenance; materialized slices reference
  **arena** provenance.
5. Call-site hints such as `.borrow_from_src` are **advisory**: they permit
  borrowing where legal; they must not override materialization requirements.
6. Containers preserve per-field provenance independently.

---

## Speculative Parsing and Rollback

Ordered choice and repetition require explicit speculative semantics.

### Rollback rules

1. **Input position** restores on failed speculative branch.
2. **Partial output** from a failed branch is not observable afterward.
3. **Arena checkpoint/rewind** at speculative entry is normative on failure.
4. **No dangling references** after a failed branch.
5. **Ordered choice is PEG-style:** `a | b` tries `a` first from the same entry
  cursor.
6. **Repetition** commits successful items; terminating non-match for `any`/`opt`
  is not an error unless the surrounding schema requires it.
7. **Fatal failure** propagates when no alternative remains.

Implementations may lower differently if observable behavior matches the above.

---

## Error Model

Parse, format, and collection produce **structured, positioned, typed** errors
(rule/schema-aware).

Diagnostics should include where possible:

- failure offset
- expected construct
- observed context
- relevant rule or schema production

`cc_collect`, `cc_parse`, and `cc_format` integrate with the ordinary CC result
and error model.

### Error rules

1. Generated entry points return `T!>(E)`-style results per contract.
2. Parse failure must not silently advance beyond the failing production's
  committed frontier.
3. Alternation diagnostics should preserve useful context (e.g. farthest offset
  and expected construct).

---

## Lowering Model

Grammar and SERDES lower to specialized ordinary C: direct helpers, specialized
generated functions, no parser VM in the hot path.

1. `@grammar(fragments)` → compile-time expansion only (no standalone runtime
  entry point; no parser VM).
2. `@grammar(rules)` → recognition/collection helpers per named entry.
3. `@grammar(schema)` → generated C types and parse/format helpers.
4. Non-recursive shapes may fully specialize; recursive shapes may use mutually
  recursive functions.
5. `cc_format` → append/emit over the chosen output buffer plus codec leaf
  encoders.
6. Write-only templating remains separate from `@grammar(schema)` unless an
  implementation chooses internal lowering.

---

## Protocol Buffers as a Pressure Test

Protocol Buffers are a useful **benchmark** for whether `@grammar(schema)` plus
codecs describe real wire formats without becoming a parser library. This section is
**normative for design intent**, not a promise of full protobuf compatibility in
v1.

### What protobuf stresses (tier 1)


| Protobuf concern                                    | What it tests in this model                                      |
| --------------------------------------------------- | ---------------------------------------------------------------- |
| Tag + wire type + payload                           | Structural sequencing vs leaf encoding                           |
| Varints and zigzag                                  | Variable-width **primitive** behavior (codec / typed leaf)       |
| Length-delimited submessages and `bytes` / `string` | **Hidden length** + **provenance** (borrow vs copy)              |
| `oneof` / discriminated unions                      | **Format-resolvable** ordered choice                             |
| Repeated fields                                     | Repetition termination and emission count                        |
| Recursion                                           | Forward declarations, arena rollback, container allocation       |
| Presence / defaults (proto2 vs proto3)              | Whether **optional emission** is a schema/type concern or policy |


### Core schema vs codec / primitive (guidance)

**Likely schema (`@grammar(schema)`) — structure, binding, directionality:**

- Field order and nesting as sequencing on the wire.
- Discriminated unions with a deterministic format branch from the in-memory
value.
- Repetition shape (how many times a production runs; what terminates it).
- Recursive and forward-declared message types.
- Parse-only vs format-only vs bidirectional annotations on fields.

**Likely codec or fixed primitive family (local byte meaning):**

- Varint encoding, zigzag, fixed-width integers, IEEE floats (unless elevated
everywhere as first-class schema syntax).
- UTF-8 validation / string vs raw bytes policy (ties to materialization).

**Gray zone (explicit split recommended in a normative follow-on):**

- Length-delimited framing is often modeled as **schema** (`len` + payload
slice) for provenance and `cc_format`, while the **varint that encodes the tag
or length** stays a **leaf** codec concern.

### Minimal protobuf-shaped example (documentation sketch)

A single small message family in the spec should touch each axis once: scalar
varint leaf, length-delimited bytes, nested message, `oneof`-style union,
optional presence, repeated field. Exact syntax is illustrative:

```c
// Illustrative only — not final surface syntax.
@grammar(schema) ProtoExample {
    // tag/wire framing: structural sequencing; varint encoding at leaf
    field_a: sint32_codec   // zigzag + varint as leaf
    field_b: len_prefixed_bytes_codec  // length + slice provenance
    field_c: ProtoExample?               // optional nested message
    union_field: ProtoOneof              // discriminated; format from tag
    repeated_d: int32_codec[]            // repetition + leaf encoding
};
```

The point of the sketch is **separation of concerns**, not literal protobuf
field declaration syntax.

### Likely v1 deferrals (protobuf-adjacent)

Out of scope or explicitly deferred for an early `@grammar(schema)` /
`cc_parse` / `cc_format` generation unless later specified:

- **Unknown field preservation** and full round-trip compatibility bags
- **Extensions**, `**Any`**, and descriptor-driven dynamic messages as first-class
- `**map**` as a dedicated schema feature (wire shape is repeated pairs; sugar
can wait)
- **Packed vs unpacked** compatibility matrix for all scalar repeats (policy
can be "generated code picks one encoding" until specified)
- **Full proto2/proto3/editions presence matrix** — document "presence model TBD"
and prefer explicit `opt` + unions in examples
- **Services / RPC**

---

## MVP Guidance

v1 should prioritize:

- shared vocabulary between `@grammar(rules)` and `@grammar(schema)`
- clear `@grammar(fragments)` vs `@grammar(rules)` boundary (substitution vs runtime grammar)
- unified `@grammar(fragments|rules|schema)` declaration family
- explicit directional semantics and format completeness checks
- codec-as-leaf-hook
- truthful provenance and canonical `cc_format`
- structured errors and transparent lowering

v1 should avoid:

- codecs as general grammar callbacks
- hidden ownership changes
- claiming parse and `cc_format` are exact inverses
- forcing tokenizer grammars through struct generation
- parser VMs on the hot path

---

## Summary

- `**@grammar(fragments)`** — replacement-only reuse (CSS-variable spirit), no
standalone grammar semantics.
- `**@grammar(rules)**` — lightweight recognition and collection (parse-first).
- `**@grammar(schema)**` — typed wire structure with `**cc_parse**` and
`**cc_format**`.
- `**@grammar` vs codecs** — pattern and structure live in `@grammar`; codecs are
leaf hooks only, not a `@grammar` mode.
- **Provenance** — truthful; **errors** — structured; **lowering** — direct C.

---

## Note on earlier naming (v0.1)

Earlier drafts used `@rules`, `@schema`, `@parse`, `@format`, and `@string`.
Conceptually: `@rules` maps to `@grammar(rules)`; `@schema` maps to
`@grammar(schema)`; `@parse` maps to `cc_parse`; `@format` maps to `cc_format`.
Fragment reuse was partially conflated with `@schema` referencing `@rules`; that
split is now explicit via `**@grammar(fragments)`** vs `**@grammar(rules)**`.

The intermediate spellings `CCFragments`, `CCRules`, and `CCSchema` in design
notes map to the same three `@grammar` modes.