# Concurrent-C Grammar and SERDES

Status: implemented

`cc/include/ccc/cc_grammar.cch` is authoritative for the generated macro
surface and C signatures described here.

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

Grammar files are compile-time factories. In a rules block, `include "path"`
copies rules from a file and `include Name` copies a rules grammar declared
earlier in the translation unit. In a schema, `rules [ include "path" ]`
creates a private copy, while `use Name` and `use "path" as Name` share a rules
factory and require qualified references such as `Name.rule`.

A block-level rule may override a rule copied by an include. Two block-level
rules with the same name are rejected. Among sibling includes, the first
definition wins. A nested rules file may override only rules introduced by its
own includes; it cannot override a rule inherited from a sibling or ancestor
include.

## `@grammar(rules)`

`@grammar(rules)` declares named byte recognizers. The default entry is the
first new, non-shadow rule declared at include depth zero in the block. A local
override of an included rule does not claim the entry. If the block declares no
new depth-zero rule, the first entry copied from its includes remains the
default. Rules compose literals, character sets and complements, sequences,
ordered choice, references, and `some`, `any`, and `opt` repetition.
Whitespace is consumed only where the grammar declares it.

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

The stable schema operations are:

```c
int parsed = cc_parse(Name, src, len, arena, out);
int read = cc_read(Name, src, len, pos, arena, out);
bool !>(CCError) streamed = cc_try_read(Name, src, len, pos, arena, out);
NameReader reader = cc_reader(Name, src, len, arena);
int next = cc_next(Name, &reader, out);
int ended = cc_at_end(Name, &reader);
size_t written = cc_write(Name, value, dst, capacity);
CCString text = cc_format(Name, value, arena);
int found = cc_get(Name, value, field_name, out_value);
const CCGramField *field = cc_field(Name, field_name, field_name_len);
```

`cc_parse` succeeds only when the schema consumes the required input and fills
the output. `cc_read` parses one value at `*pos`, advances `*pos` on success,
and returns zero on failure. A reader is a cursor over a source, position, and
arena. `cc_next` parses and advances one value; it returns zero both at clean
end and on parse failure. `cc_at_end` is true only when the cursor position
equals the source length, so it distinguishes clean end from a mid-input
failure.

`cc_try_read` has streaming Result semantics:

- `Ok(true)` means one value was parsed and `*pos` advanced.
- `Ok(false)` means clean end at a frame boundary, with `*pos == len`.
- `Err(CC_ERR_WOULD_BLOCK)` means the available bytes end within a frame;
  `*pos` is unchanged so the caller can refill and retry.
- `Err(CC_ERR_PARSE)` means the available bytes establish malformed input.

Primitive fields are stored as values. `float` and `double` fields parse their
matched numeric spans into floating values. Direct contiguous byte fields
borrow from the source; decoded, normalized, or otherwise transformed fields
materialize in the arena. Repeated and recursive structures allocate their
containers in the arena while preserving each leaf's actual provenance.

`items Elem count cap N` emits an inline `Elem[N]` field and its generated
count field; parsing does not allocate the item array. A negative count or a
count greater than `N` is a parse failure. The streaming face reports an
over-cap count as `Err(CC_ERR_PARSE)`, not `Err(CC_ERR_WOULD_BLOCK)`.

`cc_get` reflects a field by name into `CCGramValue`; `cc_field` returns its
static `CCGramField` descriptor. Schemas with conditional members carry a
presence bitmap, so a parsed field can be present even when its value is zero,
and an absent field remains distinguishable from that value. Product schemas
report their fields present.

`cc_write` emits the schema's canonical byte structure. It returns the byte
count on success and zero when the value cannot be emitted or the destination
capacity is insufficient. Exact capacity is sufficient. `cc_format` uses the
same writer and returns an arena-backed `CCString`. Both `cc_format(Name,
value, arena)` and `value.to_str(arena)` resolve to the generated
`Name_to_str`; the grammar engine registers the schema type for UFCS, so no
user registration is required.

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

`cc_dom(Name, src, len, registry, arena, out)` is the rules engine's shaped-DOM
projection. It builds `CCShapeVal` objects and arrays using the caller's
`CCShapeReg`; map-shaped data may use per-instance dictionaries.

## Lowering

Both engines emit specialized ordinary C. Generated entry points have
deterministic `Name_operation` names. The stable macro surface is `cc_match`,
`cc_parse`, `cc_collect`, `cc_dom`, `cc_read`, `cc_try_read`, `cc_reader`,
`cc_next`, `cc_at_end`, `cc_write`, `cc_format`, `cc_get`, and `cc_field`.

The rules engine computes nullability and FIRST sets, dispatches disjoint
alternatives by lookahead, fuses eligible character-set loops into SWAR scans,
and emits no-leading-pad helpers so adjacent reusable padding is scanned once.
These transformations preserve ordered-choice and byte-consumption semantics.
The generated hot path uses direct functions and control flow rather than a
parser virtual machine.
