# Concurrent-C Grammar and SERDES

Status: implemented

## Grammar engine seam

`@grammar(engine) Name {sentinel ... sentinel}` captures the fenced body
verbatim and routes its name, bytes, and source origin to the named grammar
engine. The host lexer does not tokenize the body. The compiler owns this
capture-and-route seam; each engine owns its grammar syntax, validation,
generated types, and generated operations.

The built-in SERDES engines are `rules` and `schema`:

```c
@grammar(rules) Json {~~~~
    include "json.rules"
~~~~}

@grammar(schema) RespArg {~~~~
    rules [
        digit:  charset [#'0' - #'9']
        number: keep [some digit]
    ]
    '$' len: int number "\r\n"
    data: bytes len "\r\n"
~~~~}
```

Grammar files are compile-time factories. `include "path"` copies rules from a
file, `include Name` copies a rules grammar declared earlier in the translation
unit, and a local rule overrides an included rule. Among sibling includes, the
first definition wins. A schema may instead `use Name` or
`use "path" as Name` and refer to shared rules as `Name.rule`.

## `@grammar(rules)`

`@grammar(rules)` declares named byte recognizers. The first rule declared at
the outermost include depth is the default entry. Rules compose literals,
character sets and complements, sequences, ordered choice, references, and
`some`, `any`, and `opt` repetition. Whitespace is consumed only where the
grammar declares it.

`keep` records a matched span, `skip` consumes without output, and `collect`
forms an interior collection. The generated operations are used as follows:

```c
int matched = cc_match(Name, src, len);
NameNode *tape = cc_parse(Name, src, len, arena);
int collected = cc_collect(Name, src, len, arena, callback, env);
int shaped = cc_dom(Name, src, len, registry, arena, out);
```

`cc_match` requires a full-input match. The rules form of `cc_parse` builds the
generated tape DOM. `cc_collect` replays kept leaves through the callback after
a successful match. Callback failure makes collection fail.

A plain kept span is a borrowed `CCSlice` into the source. A
`keep/decode(codec)` leaf invokes the codec only when decoding is required; the
codec may preserve a legal borrow or materialize transformed bytes in the
arena. `cc_slice_is_unique` distinguishes materialized slices from borrowed
views. Borrowed slices remain valid only while their source storage remains
valid.

Rules do not generate domain structs. Tape DOM and shaped DOM are projections
over the rule grammar, not separate grammar dialects.

## `@grammar(schema)`

`@grammar(schema)` declares a typed wire structure and emits direct-to-struct
parse and write projections. A product schema lowers named primitive, slice,
byte, nested-schema, and repeated-item fields into a generated C struct.

The stable operations are used as follows:

```c
int parsed = cc_parse(Name, src, len, arena, out);
size_t written = cc_write(Name, value, dst, capacity);
CCString text = cc_format(Name, value, arena);
```

`cc_parse` succeeds only when the schema consumes the required input and fills
the output. Primitive fields are stored as values. Direct contiguous byte
fields borrow from the source; decoded, normalized, or otherwise transformed
fields materialize in the arena. Repeated and recursive structures allocate
their containers in the arena while preserving each leaf's actual provenance.

`cc_write` emits the schema's canonical byte structure. It returns the byte
count on success and zero when the value cannot be emitted or the destination
capacity is insufficient. Exact capacity is sufficient. `cc_format` uses the
same writer and returns an arena-backed `CCString`; `value.to_str(arena)` is its
UFCS form.

Length and count fields that drive `bytes` or `items` parsing are derived from
the corresponding value on write. The stored parse-time count does not override
the data length or item count. Nested and self-typed items invoke their
generated writers recursively.

Schema write generation rejects a term without a defined write projection.
Codecs remain leaf operations: they may parse or encode a primitive value, but
they do not control sequencing, choice, repetition, rollback, or ownership.

## Tagged schema alternatives

`one of` declares a tagged union:

```c
@grammar(schema) Reply {~~~~
    rules [
        digit:  charset [#'0' - #'9']
        number: keep [opt #'-' some digit]
        lchar:  complement charset [#'\r' #'\n']
        line:   keep [any lchar]
    ]
    one of [
        simple  [ #'+' text: line "\r\n" ]
        integer [ #':' value: int number "\r\n" ]
        bulk    [ #'$' len: int number "\r\n" data: bytes len "\r\n" ]
        array   [ #'*' n: int number "\r\n" items: items Reply n ]
    ]
~~~~}
```

The generated default layout is:

```c
typedef enum { Reply_simple, Reply_integer, Reply_bulk, Reply_array } ReplyKind;
typedef struct Reply {
    ReplyKind kind;
    union {
        /* one product struct per alternative */
    } u;
} Reply;
```

Alternatives dispatch by disjoint leading literals. At most one alternative may
omit a leading literal; that alternative is the default. Parse stores the
selected tag and fields in its union member. Write switches on `kind`, emits the
selected product, derives lengths and counts, and recurses through self-typed
items. An out-of-range tag emits zero bytes. If any alternative contains a term
without a write form, the declaration is rejected rather than generating a
partial writer.

## Errors and source origin

Malformed grammar declarations are compile-time errors attributed to the
captured source origin. This includes unknown rules or grammars, invalid
references, ambiguous alternatives, unsupported field shapes, and schemas
without a complete requested write projection.

Runtime recognition, collection, shaped DOM, and schema parse operations report
failure with zero. Rules tape parsing reports failure with a null pointer. They
do not expose partially successful output as success. Write reports failure
with a zero byte count, and format returns an empty string. Grammar operations
compose with the language's ordinary result handling at call sites.

## Lowering

Both engines emit specialized ordinary C. Generated entry points have
deterministic `Name_operation` names and are also available through the
`cc_match`, `cc_parse`, `cc_collect`, `cc_dom`, `cc_write`, and `cc_format`
macros.

The rules engine computes nullability and FIRST sets, dispatches disjoint
alternatives by lookahead, fuses eligible character-set loops into SWAR scans,
and emits no-leading-pad helpers so adjacent reusable padding is scanned once.
These transformations preserve ordered-choice and byte-consumption semantics.
The generated hot path uses direct functions and control flow rather than a
parser virtual machine.
