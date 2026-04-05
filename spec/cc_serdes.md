# Concurrent-C Grammar and SERDES Specification

**Version:** 0.1-draft  
**Date:** 2026-04-03  
**Status:** Draft inclusion-ready language and lowering specification

---

## Goals and Scope

Concurrent-C grammar and SERDES provides lightweight recognition grammars and
schema-driven parsing and formatting for structured text and binary formats. It
introduces five related surface forms:

- `@rules` for lightweight recognition and collection grammars
- `@schema` for structural wire-format declarations
- `@parse` for schema-driven reading
- `@format` for schema-driven writing
- `@string` for lightweight write-only templating without a schema

**Design intent:** keep the schema declarative, keep ownership truthful, keep
lowering transparent, and keep the generated hot path competitive with
handwritten code.

**Scope:** This document defines the shared grammar model, the semantic model
for lightweight rule collection and schema-driven parse and format operations,
the role of codecs, directionality rules, provenance rules, error behavior, and
the intended lowering model.

---

## Design Principles

1. **One grammar family, two weights.** `@rules` provides lightweight
   recognition and collection. `@schema` provides typed structural SERDES on
   the same conceptual foundation.
2. **One structural schema, two generated operations.** `@schema` describes
   wire structure from which the compiler may generate both parse and format
   operations.
3. **Structure over callbacks.** Grammar semantics belong to rules and schemas,
   not to codec callbacks.
4. **Leaf hooks only.** Codecs are lightweight bidirectional escape hatches for
   primitive/domain details that are easier to express imperatively.
5. **Truthful provenance.** Parsed values must accurately reflect whether they
   borrow from source input or were materialized into an arena.
6. **Canonical formatting.** Formatting emits bytes from the schema's output
   structure; it is not defined as "running the parser backwards".
7. **Structured errors.** Parse and format failures are positioned, typed, and
   grammar-aware.
8. **Transparent lowering.** Implementations should generate direct,
   specialized ordinary C code with no parser VM requirement in the hot path.

---

## Shared Grammar Model

`@rules` and `@schema` are not separate parsing worlds. They are two layers of
the same grammar family.

Both forms should share the same core vocabulary wherever possible, including:

- literals
- named character sets
- grouping
- ordered choice
- repetition (`some`, `any`, `opt`)
- position/search operators such as `to` and `thru`
- named rule references
- collection helpers such as `keep`

**Rule:** `@schema` should feel like the typed structural layer built on the
same matching model as `@rules`, not like a separate ad hoc DSL.

**Rule:** `@rules` is parse-only by default. `@schema` is the structured layer
that may participate in both parse and format generation.

---

## Surface Overview

### `@rules`

`@rules` declares lightweight recognition and collection grammars. It is
intended for tokenizers, lexers, scanners, comment skipping, span extraction,
and similar parse-only problems where generated structs would be unnecessary
weight.

Rules are:

- named
- composable
- recognition-first rather than type-first
- collect-driven rather than field-driven

Example:

```c
@rules CssRules {
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

Typical rule-oriented entry points may include:

```c
char[:][:] tokens = @collect(src, arena, CssRules.tokenize) @err;
bool ok = @match(src, CssRules.ident) @err;
```

**Rule:** `@rules Name { ... }` introduces a named rule namespace. Individual
rules within that declaration are referenced as `Name.rule` in grammar and call
sites.

**Rule:** The `Name.rule` form in this document is not ordinary field access. It
is grammar-entry selection syntax for named rules declared inside an `@rules`
block.

### `@rules` Collection Semantics

`@rules` is collection-driven:

- if a rule consumes input but is not kept, it does not appear in the output
- `skip` consumes without output
- `keep` emits the matched or derived value into the collected result
- `collect` defines a sequence-producing rule

**Rule:** `@rules` does not generate domain structs by default.

**Rule:** `@rules` output shape is determined by the kept values of the selected
entry rule rather than by named fields.

By default, the most natural output for lightweight tokenization is a
homogeneous kept sequence such as `char[:][:]`.

**Rule:** Homogeneous kept output is sufficient for span-oriented tokenizers,
lexeme-preserving minifiers, and similar recognition tasks where each emitted
item is naturally represented as the same value family.

**Rule:** Heterogeneous collected output is not required for `@rules` v1.

**Future direction:** a later extension may permit user-supplied emitted token
types for `@rules` collection, such as a tagged token struct containing token
kind plus matched span, without promoting `@rules` into full `@schema`.

Example future shape:

```c
typedef struct {
    CssTokenKind kind;
    char[:] text;
} CssToken;

CssToken[:] toks = @collect(src, arena, CssRules.tokenize, CssToken) @err;
```

This future direction is intended for typed token streams. Recursive structural
domain modeling remains the responsibility of `@schema`.

### `@schema`

`@schema` declares the structural wire model for a format. A schema may be:

- a product/sequence schema
- an alternation schema
- a forward declaration for recursive schemas

Example:

```c
@schema RespBulkString {
    '$' @parse_only len: int "\r\n"
    value: char[:len] "\r\n"
};
```

`@schema` is the heavier structured layer that adds:

- named typed fields
- generated output types
- directionality
- schema-driven formatting
- provenance-aware storage semantics

### `@parse`

`@parse(src, arena, Type)` parses bytes from `src` into a value of schema type
`Type`.

Optional parse options may be supplied at the call site. For example:

```c
RespBulkString s = @parse(src, arena, RespBulkString, .zero_copy) @err;
```

### `@format`

`@format(value, arena, Type)` formats a schema value into canonical bytes,
returning a string-like output object chosen by the language/library contract.

Example:

```c
CCString out = @format(v, arena, RespValue) @err;
```

### `@string`

`@string` remains the easy write-only convenience form. It does not require a
schema and is intentionally simpler than full SERDES.

Example:

```c
CCString msg = @string(json_codec, `{"name": ${name}}`, arena);
```

---

## `@rules` vs `@schema`

The intended split is:

- `@rules` for recognition, scanning, tokenization, and collection
- `@schema` for typed structural parse and format generation

`@rules` is appropriate when the output is naturally a flat or lightly-typed
collection of kept values. `@schema` is appropriate when the output is a
structured record, union, recursive value, or other domain model.

**Rule:** An implementation should not force tokenizer-style grammars through
struct generation when a rule grammar is sufficient.

**Rule:** `@schema` should remain a structured superset in spirit, but it is not
required to expose every rule-only collection convenience in identical form.

### Cross-Referencing

The two layers should compose, but composition is directional in v1.

**Rule:** `@schema` may reference `@rules` fragments as parse-side leaf
recognizers or extractors when the referenced rule has a schema-compatible
output shape.

Example:

```c
@schema CssRule {
    selector: CssRules.ident
    '{' props: keep(some) CssProperty '}'
};
```

This allows a schema to reuse lightweight lexical recognition without forcing
those leaf patterns to become full schemas.

**Rule:** `@rules` does not consume full schemas in v1.

**Rule:** Generalized schema parsing over arbitrary token streams is not defined
in v1. Byte-oriented grammar is the normative input model for this draft.

**Future direction:** multi-phase parsing is expected to be valuable, including
pipelines such as:

- source bytes -> `@rules` token collection
- token stream -> later structured parse

but token-stream grammar is a separate semantic layer and should not be implied
without explicit stream-type rules.

---

## Grammar Semantics vs Codec Semantics

**Hard rule:** grammar semantics belong to `@rules` and `@schema`, not to the
codec.

Rules and schemas own:

- sequencing
- literals
- ordered choice
- repetition
- recursion
- collection shape
- field binding
- structural output layout

The codec owns only leaf-level domain behavior, such as:

- primitive number parsing and formatting
- string escaping and unescaping
- whitespace style or skipping policy
- domain-specific diagnostics

**Rule:** A codec may refine how primitive values are interpreted or emitted,
but it must not redefine structural parser semantics such as branching,
rollback, repetition, recursion, or ownership.

**Rule of thumb:**

- if it changes what a primitive byte sequence means locally, it may belong in
  the codec
- if it changes how the grammar matches or how values are stored, it belongs in
  grammar/runtime semantics instead

---

## Codec Model

A codec is the bidirectional domain hook for schema-driven SERDES, analogous in
spirit to the policy argument used by `@string`.

**Intent:** the codec is a lightweight imperative escape hatch for domain
details, while the grammar remains the declarative center of the feature.

Typical codec responsibilities include:

- parse primitive numeric forms (`int`, `float`, domain integers)
- format primitive numeric forms
- escape or unescape domain strings
- skip or emit canonical whitespace
- construct domain-specific errors

Codec hooks must not become a second grammar language.

**Rule:** An implementation should reject or warn on codec usage patterns that
attempt to control general grammar flow instead of leaf behavior.

---

## Directionality

Only `@schema` participates in generated formatting. `@rules` is parse-only
unless a future extension explicitly states otherwise.

Schema elements participate in one or both generated directions:

- `bidirectional` means the element contributes to both parsing and formatting
- `parse_only` means the element is used only while reading input
- `format_only` means the element is used only while producing output

**Rule:** Directionality is part of schema semantics. An implementation must
reject a schema that requires write-side behavior for an element that has only
parse meaning, unless the schema explicitly supplies a format-side rule.

### Legality Table

| Schema element | Parse | Format | Default direction |
|--------|--------|--------|--------|
| Literal char/string/bytes | match exact input | emit exact bytes | bidirectional |
| Named primitive field | parse field value | format field value | bidirectional |
| Named nested-schema field | parse nested value | format nested value | bidirectional |
| Ordered sequence | parse in order | emit in order | bidirectional |
| Ordered choice `a \| b` | try branches in order | select branch from value/tag | bidirectional |
| Repetition with explicit count | parse repeated items | emit repeated items | bidirectional |
| Collection `keep(...)` | collect parsed items | emit contained items | bidirectional if output shape is defined |
| Hidden driver field | may guide later parse | not emitted unless explicitly mapped | parse_only |
| `to ...` search | advance scan position | no implicit inverse | parse_only |
| `thru ...` search | advance through delimiter | no implicit inverse | parse_only |
| Delimiter-driven extraction | parse by scanning delimiter | no implicit inverse unless format rule is defined | parse_only by default |
| Whitespace skipping | consume allowed input whitespace | emit only if schema/codec defines it | parse_only by default |
| Guarded branch `opt (expr)` | controls parse presence | may control emission if evaluable from output value | bidirectional if well-defined |
| Derived emitted field | not read from input | emitted from value or formatter helper | format_only |

### Directionality Rules

1. **Literals are symmetric by default.** A literal token matched during parse
   is emitted unchanged during format.
2. **Named value fields are symmetric by default.** A bound field of primitive
   or schema type participates in both parse and format unless marked
   otherwise.
3. **Hidden fields are parse-only by default.** A field used only to drive
   later parsing, such as a length prefix, is not part of the formatted output
   value unless explicitly retained or re-derived.
4. **Search operators are parse-only unless given explicit format meaning.**
   `to`, `thru`, and delimiter-search extraction describe how to locate input
   boundaries and do not imply how bytes should be emitted.
5. **Formatting is structural, not inverse search.** `@format` emits bytes from
   the schema's output structure and field values. It does not "run parse
   backwards".
6. **Choice must be format-resolvable.** A bidirectional alternation must
   provide a deterministic way to choose the emitted branch, typically from a
   generated tag or concrete output type.
7. **Implementations must reject ambiguous format schemas.** If a schema
   contains parse-only constructs with no format-side meaning but is used with
   `@format`, the compiler must emit a diagnostic unless the remaining output
   path is still complete.

### Suggested Surface Markers

Minimal explicit surface markers are sufficient:

```c
@parse_only len: int
@format_only checksum: uint32
```

---

## Provenance and Materialization

When `@parse` produces slices or nested values, the implementation must
preserve truthful provenance. Parsed output may either:

- borrow from the input slice
- materialize into the provided arena

**Rule:** provenance is not a hint or optimization detail. It is part of the
observable storage truth of the produced value.

### Borrow vs Materialize Table

| Parsed construct | Borrow input | Materialize in arena | Default |
|--------|--------|--------|--------|
| Fixed-length slice `char[:len]` with no transform | yes | yes | borrow if legal |
| Delimiter slice `char[:to x]` with no transform | yes | yes | borrow if legal |
| Literal token | not applicable | not applicable | matched only |
| Primitive numeric field | not applicable | value stored directly | value |
| Charset-captured slice with no transform | yes | yes | borrow if legal |
| Escaped string requiring unescape | no | yes | materialize |
| Transcoded or normalized text | no | yes | materialize |
| Binary integer fields | not applicable | value stored directly | value |
| Repeated borrowed slices | yes | yes | each element follows own rule |
| Nested schema with only borrowed leaves | yes | yes | borrow leaves if legal |
| Recursive structure needing owned node allocation | limited | yes | materialize container as needed |

### Provenance Rules

1. **Borrowing is allowed only when the parsed value is a direct view of source
   bytes.** If the output slice can be represented as a contiguous sub-slice of
   `src` without transformation, it may borrow.
2. **Transformation requires materialization.** If parsing performs unescaping,
   decoding, transcoding, normalization, or any byte rewrite, the resulting
   slice must be materialized in `arena`.
3. **Codecs do not invent provenance policy.** A codec may request or require
   transformation for a leaf value, but it may not claim a borrowed slice when
   the bytes were rewritten.
4. **Borrowed slices must point into the original input provenance.** If a
   field borrows, its slice provenance must reflect the input source rather than
   the arena.
5. **Materialized slices must reflect arena provenance.** If a field allocates
   and copies or synthesizes bytes, its provenance must identify destination
   arena storage.
6. **`.zero_copy` is advisory only.** A call-site hint such as `.zero_copy`
   permits borrowing where legal, but must not force borrowing when
   transformation or ownership rules require materialization.
7. **Containers do not erase leaf truth.** A parsed struct, array, or union may
   contain a mix of borrowed and materialized fields. Each field's provenance
   remains independently truthful.
8. **Recursive outputs may allocate structure even when leaves borrow.** A
   recursive parse may require arena allocation for container nodes while still
   allowing leaf byte views to borrow from input.

---

## Speculative Parsing and Rollback

Ordered choice and repetition require speculative parse behavior. The semantics
must be explicit.

### Rollback Rules

1. **Input position rollback.** If a speculative branch fails, the input slice
   position must be restored to the position held at branch entry.
2. **Partial output is invalid on failure.** Values written into an output slot
   during a failed speculative branch must not be observed after that branch
   fails.
3. **Arena checkpoint/rewind is normative.** Speculative parsing must take an
   arena checkpoint at speculative branch entry and must rewind to that
   checkpoint if the branch fails.
4. **No escaping partial references.** A failed branch must not leave behind
   borrowed or materialized references that appear valid to later branches.
5. **Ordered choice is PEG-style.** `a | b` tries `a` first. If `a` succeeds,
   `b` is not attempted. If `a` fails, `b` runs from the original entry
   position.
6. **Repetition commits item-by-item.** Repetition forms may commit each
   successfully parsed element in sequence; the terminating non-match for
   `any`/`opt` is not an error unless required by surrounding structure.
7. **Fatal failure propagates.** If the active branch fails in a non-recoverable
   way and no alternative remains, the generated parse returns an error result.

**Implementation note:** These rules constrain observable behavior, not the
exact lowering strategy. Implementations remain free to generate specialized
parsers using arena checkpoints, temporary locals, or equivalent direct C
rewrites, provided the observable semantics remain identical to checkpoint and
rewind at speculative boundaries.

---

## Error Model

Grammar and SERDES operations produce structured, positioned, typed errors.

Rule-oriented and schema-oriented diagnostics should include enough information
to report:

- failure offset
- expected construct
- observed input or output context
- relevant rule or schema production

`@collect ... @err`, `@parse ... @err`, and `@format ... @err` integrate with
the ordinary CC result and error model.

### Error Rules

1. Generated grammar and SERDES functions should return `T!>(E)`-style results
   using the domain error type named by the grammar/library contract.
2. A parse failure must not silently consume input beyond the committed parse
   frontier for the failing production.
3. Alternation failures should preserve the most useful diagnostic context
   available, typically the farthest offset reached together with the expected
   construct at that point.
4. Local handlers such as `@err(e) { ... }` and scoped defaults such as
   `@errhandler(E e) { ... }` are surface error-consumption features; they do
   not change core parser semantics.

---

## Lowering Model

Grammar and SERDES are compile-time features that lower to specialized ordinary
C code.

Implementations should favor:

- direct helper calls
- specialized generated functions
- no virtual dispatch in the hot path
- no hidden parser VM

### Lowering Rules

1. `@rules` lowers to reusable recognition and collection helpers for named
   entry rules.
2. `@schema` lowers to concrete generated C types and helper functions.
3. Non-recursive grammars and schemas may lower to fully specialized direct
   functions with no runtime dispatch.
4. Recursive grammars and schemas may lower to mutually recursive C functions.
5. `@format` lowers to direct append or emit operations over the chosen output
   buffer type together with codec leaf encoders.
6. `@string` remains a separate write-only facility and is not defined as sugar
   over `@schema` unless an implementation explicitly chooses to do so
   internally.

---

## MVP Guidance

Grammar and SERDES v1 should prioritize:

- shared primitive vocabulary between `@rules` and `@schema`
- lightweight rule composition
- clear structural schemas
- explicit directional semantics
- codec-as-leaf-hook
- truthful provenance
- canonical formatting
- structured errors
- transparent lowering

Grammar and SERDES v1 should avoid:

- codecs acting as general grammar callbacks
- hidden ownership changes
- claims that parse and format are exact inverses
- forcing tokenizer-style grammars through struct generation
- parser-runtime indirection where direct lowering is practical

---

## Summary

The intended mental model is:

- `@rules` declares lightweight recognition and collection grammars
- `@schema` declares typed structure
- `@parse` reads according to schema structure
- `@format` writes canonically according to schema structure
- codecs customize leaf behavior, not grammar behavior
- provenance tells the truth
- errors are structured
- lowering stays visible and direct
