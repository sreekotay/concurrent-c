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

The built-in engines are `rules` and `schema`. Any other engine name is a
`@comptime` function `void engine(CCSlice name, CCSlice body, const char *file, int line)`.
The seam synthesizes a comptime call; if that function is not in scope,
compilation fails.

`@grammar(cli)` is that comptime path (argv → typed struct; not a byte-wire
dialect). Its fenced body is the `CliSyntax` rules factory in
`<ccc/std/cli_decl.rules>`. `.ccs` units include `<ccc/std/cli.cch>` before
the declaration so `cli` is harvested. `.shcc` units receive that header from
`<ccc/script/prelude.cch>`.

```c
#include <ccc/std/json.cch>
@grammar(rules) Json {~~~~
    include JsonRfc
~~~~}

@grammar(schema) RespArg {~~~~
    rules [
        digit:  charset [#'0' - #'9']
        number: keep [some digit]
    ]
    '$' len: int number "\r\n"
    data: bytes len "\r\n"
~~~~}

#include <ccc/std/cli.cch>
@grammar(cli) Opts {~~~~
    help: flag -h, --help desc "Show help"
    jobs: opt i64 -j, --jobs attach as N default 4 desc "Workers"
    file: rest string desc "Inputs"
~~~~}
```

Grammar files are compile-time factories. In a rules block, `include "path"`
copies rules from a file (relative to the including source, then the compiler
include path), `include <ccc/…>` searches the include path only, and
`include Name` copies a rules grammar declared earlier in the translation
unit or a stdlib factory (`JsonRfc`, `JsonKeep`, `JsonDom`). In a schema,
`rules [ include "path" ]` creates a private copy, while `use Name`,
`use "path" as Name`, and `use <ccc/…> as Name` share a rules factory and
require qualified references such as `Name.rule`. The JSON factory in
`<ccc/std/json.rules>` (`include JsonRfc`) recognizes RFC 8259 JSON-text
(`ws value ws`, the seven value types, no leading zeros or trailing dots
on numbers). Unescaped U+0000–U+001F in a string is a miss. `JsonKeep`
adds `keep/decode(jstr)` on strings and `keep` on numbers (needs
`<ccc/std/json.cch>`). `JsonDom` adds `collect` on containers. Closed
products (`JsonVal`, Tweet, TmGrammar) stay in the TU. JSON is not in
the std prelude.

A fenced body may contain `depth N`, where `N` is an integer from 1 through
65535. The default nest limit is 128. The directive is valid only in the
`@grammar` block itself, not in an included file. A second `depth` in the same
block is rejected. Crossing the limit is a parse failure: `cc_match` /
`cc_parse` / `cc_collect` / `cc_dom` return zero, and the schema streaming face
reports `Err(CC_ERR_PARSE)`. For `@grammar(rules)` the limit counts nested
generated-rule calls. For `@grammar(schema)` it counts nested `__fill` calls
(self-typed or mutually recursive items).

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
ordered choice, references, `some` / `any` / `opt` repetition, and
`and` / `not` peeks (try the child and restore the cursor). Character and
string literals accept `\\n`, `\\t`, `\\r`, `\\0`, `\\\\`, `\\'`, `\\"`,
and `\\xHH` (two hex digits). Whitespace is consumed only where the
grammar declares it. The engine grows its IR from a stack-rooted arena;
grammar size is not a fixed cap.

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
valid. A successful keep does not imply Unicode well-formedness of a
borrowed span; that check belongs at the use site that treats the bytes as
text.

Rules do not generate domain structs. Tape DOM and shaped DOM are projections
over the rule grammar, not separate grammar dialects.

## `@grammar(schema)`

`@grammar(schema)` declares a typed wire structure and emits direct-to-struct
parse and write projections. The schema IR (terms, keys, body, variants)
grows from the same stack-rooted arena as the used rules grammar; schema
size is not a fixed cap. Identifier and field spellings stay bounded.
`one of` still shares the `@variant` arm table (32 arms). A product schema lowers named primitive, slice,
byte, nested-schema, and repeated-item fields into a generated C struct.
A `bytes` field is a `CCSliceHdr` (`{ptr,len}` wire borrow). A kept string /
line field remains a `CCSlice` (provenance and optional codec materialize).

The stable schema operations are:

```c
int parsed = cc_parse(Name, src, len, arena, out);
int read = cc_read(Name, src, len, pos, arena, out);
bool !>(CCError) streamed = cc_try_read(Name, src, len, pos, arena, out);
NameReader reader = cc_reader(Name, src, len, arena);
int next = cc_next(Name, &reader, out);
int ended = cc_at_end(Name, &reader);
size_t written = cc_write(Name, value, dst, capacity);
size_t need = cc_measure(Name, value);
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

`field: Schema` binds one nested value as an arena-allocated `Schema*`. The
named type may be declared later in the translation unit, including a cycle
(`Member` holds `Val*`, `Val` holds `object of Member`). A typo is a C
compile failure on the generated pointer type.

`field: G.rule of Schema` narrows a list-shaped rule (open, optional
delimited elements, close) and fills an arena array of `Schema`. The same
form applies to member lists (`members: Json.object of JsonMember`) and
value lists (`elems: Json.array of JsonVal`). The rule supplies delimiters
and pads; the schema supplies the element type.

`items Elem count cap N` emits an inline `Elem[N]` field and its generated
count field; parsing does not allocate the item array. A negative count or a
count greater than `N` is a parse failure. The streaming face reports an
over-cap count as `Err(CC_ERR_PARSE)`, not `Err(CC_ERR_WOULD_BLOCK)`. The
emitter also publishes `enum { Name_field_cap = N }` so call sites can size
buffers from the schema instead of restating the literal.

A schema `int` field stores a signed 64-bit integer. A numeric bind whose
value is outside that range is a parse failure. The streaming face reports it
as `Err(CC_ERR_PARSE)`, not `Err(CC_ERR_WOULD_BLOCK)`. The generated
accumulator does not wrap.

`cc_get` reflects a field by name into `CCGramValue`; `cc_field` returns its
static `CCGramField` descriptor. Schemas with conditional members carry a
presence bitmap, so a parsed field can be present even when its value is zero,
and an absent field remains distinguishable from that value. Product schemas
report their fields present.

`cc_write` emits the schema's canonical byte structure. It returns the byte
count on success and zero when the value cannot be emitted or the destination
capacity is insufficient. Exact capacity is sufficient. `cc_measure` returns
that same success size with no destination buffer (and zero when the value
cannot be emitted), so callers can allocate once before `cc_write`.
`cc_format` uses the same writer and returns an arena-backed `CCString`.
`cc_format(Name, value, arena)` / `value.to_str(arena)`, `cc_measure(Name,
value)` / `value.measure()`, and `cc_write(Name, value, dst, cap)` /
`value.write(dst, cap)` resolve to the generated faces; the grammar engine
registers the schema type for UFCS, so no user registration is required.

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

### Access model

A schema `one of` is a tagged sum with the same **protected consumption
surface** as a default-layout `@variant`:

- construction names exactly one arm (`Reply r = { .bulk = { .data = s } };`);
- arm projection requires kind/switch/`!>`/`?>` domination;
- user source cannot write `.kind` or reach into `.u`.

Generated parse fill and write helpers retain raw layout access. Product-level
fields outside the union (field-discriminated `one of`) remain ordinary struct
members. Designators `{ .kind = … }` / `{ .u.… = … }` are an interop escape for
compound literals only.

Default-layout `@variant` and schema `one of` share the `NameKind` + `union u`
shape and the access rules above. They are not the same type family: schemas
add wire parse/write/measure faces; variants add destructor/transition and
optional packing. Packed `@variant` representations are not layout-compatible
with schema `one of`.

Alternatives dispatch by disjoint leading literals. At most one alternative may
omit a leading literal; that alternative is the default. Parse stores the
selected tag and fields in its union member. Write switches on `kind`, emits the
selected product, derives lengths and counts, and recurses through self-typed
items. An out-of-range tag emits zero bytes. If any alternative contains a term
without a write form, the declaration is rejected rather than generating a
partial writer.

A product schema may share a prefix with a field-discriminated `one of`:

```c
@grammar(schema) Bulk {~~~~
    rules [
        digit:  charset [#'0' - #'9']
        number: keep [opt #'-' some digit]
    ]
    #'$' len: int number
    one of [
        nil  [= -1] [ "\r\n" ]
        data [>= 0] [ "\r\n" payload: bytes len "\r\n" ]
    ] by len
~~~~}
```

`by <int-field>` selects the arm from an earlier product-level int bind. Each
arm carries `[= N]` or `[>= N]`; the schema needs at least one equality arm and
one lower-bound arm. Parse binds the int, then dispatches; unknown values are
parse failures. Write switches on `kind` and emits the discriminant from the
arm (equality arms emit `N`; lower-bound arms derive the int from a later
`bytes`/`items` count) rather than from a stored field. Unit arms (literals
only) are allowed. Prefix `one of` remains the entire schema body; field mode
may follow shared product terms. One `one of` per schema.

Prefix dispatch is the first distinct byte of each arm: a leading
literal, a narrowed list's open byte, or the singleton FIRST set of a
bound rule (`Json.string` → `"`). An arm whose first term has several
possible first bytes (JSON number) is the default arm. At most one
default arm is allowed; first bytes must be distinct.

## Errors and source origin

Malformed grammar declarations are compile-time errors attributed to the
captured source origin. This includes unknown rules or grammars, invalid
references, ambiguous alternatives, unsupported field shapes, and schemas
without a complete requested write projection.

Runtime recognition, collection, shaped DOM, and schema parse operations report
failure with zero. Rules tape parsing reports failure with a null pointer. They
do not expose partially successful output as success. Write reports failure
with a zero byte count, and format returns an empty string. After a failed
`cc_match` or `cc_parse` in the same thread, `cc_fail_pos()` is the
high-water byte offset of the miss (zero if the last call succeeded or
never ran). Recognition stays boolean; the cursor is not a second result
channel. Grammar operations compose with the language's ordinary result
handling at call sites.

`cc_dom(Name, src, len, registry, arena, out)` is the rules engine's shaped-DOM
projection. It builds `CCShapeVal` objects and arrays using the caller's
`CCShapeReg`; map-shaped data may use per-instance dictionaries.

## `@grammar(cli)`

`@grammar(cli)` declares argv options and emits a typed C struct plus
`Name_parse_args` / `Name_prepare` / `Name_print_usage`. The fenced body is
the `CliSyntax` rules factory (`<ccc/std/cli_decl.rules>`). Include
`<ccc/std/cli.cch>` before the declaration in `.ccs` so the `cli` comptime
engine is harvested; omit it and the unit fails to compile. `.shcc` units
already have that header from the script prelude. Call sites use
`cc_parse_args` / `cc_prepare_args` / `cc_print_usage` from that header
(same faces for `.ccs` and `.shcc`).

Field kinds:

- `flag` / `count` — bool or `int64_t` increment
- `opt i64` — `int64_t` + `name_present`
- `opt string` — `char[:0]` + `name_present` (NUL-terminated borrow of argv
  or of a `default "..."` literal)
- `rest string` — `char[:0] *name` + `name_len`
- `alias SPELL... -> field[=value]` — fixed or passthrough map onto another field
  (top-level row; optional when using the inline `alias` attr below)

Trailing attrs (order-free):

- `desc "..."` — usage text
- `as NAME` — usage metavariable (`--jobs N`)
- `attach` — allow glued shorts (`-p4`); omit ⇒ space-required
- `default LIT` — applied before argv overlay; `_present` stays false until
  the user sets the option. `LIT` is an integer for `opt i64`, or `"..."` for
  `opt string`. Shown in usage as `[default: …]`.
- `alias SPELL=VALUE[, SPELL=VALUE...]` — on `opt` only; synthesizes the same
  alias rows as top-level `alias: … -> field=VALUE` (e.g.
  `alias --fast=1, --best=9` on a `level` opt)

Accepted spellings of the same attrs: `attached` for `attach`, `value_name`
for `as`, `space` for space-required (inverse of `attach`), and
`many positional string` for `rest string`.

Spellings are unquoted (`-h`, `--help`, `-11`, `-0..-9`). Long `--name=value`
is accepted. `--` ends options. A digit range `-LO..-HI` expands to each
inclusive short (`-0..-9` → `-0`…`-9`); multi-digit shorts such as `-11` stay
listed separately. Digit shorts bind when listed (or expanded) on an
`opt i64` field.

`cc_cli_overlay_i64` / `cc_cli_overlay_cstr` copy an opt into a destination
only when `*_present` is true (user-set), so apply sites need not write the
`if (opts->field_present)` ladder by hand.

```c
Opts opts = {0};
bool go = cc_prepare_args(Opts, argc, argv, &arena, &opts, stderr) !>;
if (!go) return 0; /* help flag named `help` → Ok(false); usage printed */
```

`cc_prepare_args` is a `bool !>(CCError)` face: `Ok(true)` proceed,
`Ok(false)` help (usage printed), `Err` bad argv (usage printed).
`cc_parse_args` is the pure fill (applies defaults, then overlays argv).
A `flag` field named `help` is the help marker. `cc_print_usage` prints the
generated usage text.

Usage is generated from the same field table: basename of `argv[0]`,
letter shorts preferred over digit-only shorts, `as` metavariables,
`[default: …]`, and `alias` rows listed under their target.

## Lowering

Engines emit specialized ordinary C. Generated entry points have
deterministic `Name_operation` names. The stable macro surface is `cc_match`,
`cc_parse`, `cc_collect`, `cc_dom`, `cc_read`, `cc_try_read`, `cc_reader`,
`cc_next`, `cc_at_end`, `cc_write`, `cc_measure`, `cc_format`, `cc_get`,
`cc_field`, `cc_parse_args`, `cc_prepare_args`, and `cc_print_usage`.

The rules engine computes nullability and FIRST sets, dispatches disjoint
alternatives by lookahead, fuses eligible character-set loops into SWAR scans,
and emits no-leading-pad helpers so adjacent reusable padding is scanned once.
These transformations preserve ordered-choice and byte-consumption semantics.
The generated hot path uses direct functions and control flow rather than a
parser virtual machine.
