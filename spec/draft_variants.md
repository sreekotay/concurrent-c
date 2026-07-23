# Draft: `@variant` — first-class tagged unions

Status: draft — not implemented; where this document disagrees with the
shipped compiler, the compiler wins.

## 1. Motivation

The language already owns two tagged-union shapes:

1. **Results.** `T !>(E)` lowers to
   `struct { bool ok; union { T value; E error; } u; }` with active-arm
   drop semantics and move-out on `!>`.
2. **Schema one-of.** `@grammar(schema) … one of […]` lowers to
   `typedef enum { Reply_simple, … } ReplyKind;
   struct Reply { ReplyKind kind; union { … } u; }`, with parse and
   write projections.

What is missing is the plain **data-model** sum type. Two deliberately
different shapes coexist in the same program:

- **Wire boundary** (e.g. a RESP reply): closed set, dispatch byte,
  recursive, needs parse/write projections — served by schema one-of.
- **Data model** (e.g. a value cell that holds a string or an integer):
  evolving set, no dispatch byte, needs cheap in-place transitions and
  destructor discipline — served by `@variant`.

Without a data-model sum type, code stores integers as decimal text and
pays parse → checked add → re-encode → copy on every increment; the type
system should carry the representation instead.

## 2. Principles

- **Lowered C is first class.** The default lowering is normative,
  ABI-stable, and identical to the schema one-of shape. C interop reads
  `v.kind` / `v.u.<arm>` directly; the CC surface is sugar over exactly
  that struct.
- **Converge, don't multiply.** One lowering serves schema one-of and
  `@variant`; schema one-of is a variant with wire projections attached
  (§8). Result keeps its `ok` spelling for ABI stability but is
  conceptually the two-arm instance.
- **No hidden allocation.** A variant is a value type: `sizeof = tag +
  max(arm)`. Construction, projection, and transition never allocate.
- **Layout-agnostic surface.** The designator dialect (construction,
  brace-assignment, `case .arm:`, projection) plus the read-only `.kind`
  pseudo-member is the portable contract: `v.kind` always reads the tag
  (a field read on the unpacked layout, a compiler-emitted decode on the
  packed layout) and is never assignable — tags change only via
  construction and transition. Raw `v.u` access (and writing `kind`) is
  an interop privilege of the unpacked layout. This is what makes §11
  packing a representation choice instead of a semantic one.
- **FAIL LOUDLY.** A non-exhaustive variant `switch` without `default`
  is a compile error. Reading an inactive arm is UB in C but a compile
  error where statically provable (same rule as Result).

## 3. Declaration and lowering

```c
@variant RedisValue {
    str: CCString;
    num: int64_t;
};

@variant Signal {          /* payload-less arms allowed */
    hup:  void;
    term: void;
    info: int;
};
```

- Arm names are C identifiers (they become enum and union member names,
  so C keywords are rejected at parse: `int:` is an error — spell it
  `num:`).
- Each arm has exactly one type; multi-field payloads use a struct.
- Recursive arms are allowed through pointers/slices only
  (`items: RespReply[:]`), same rule as C.
- `void` arms are legal in any variant; they carry no payload.

**Lowering (normative, default layout):**

```c
typedef enum { RedisValue_str, RedisValue_num } RedisValueKind;
struct RedisValue {
    RedisValueKind kind;
    union { CCString str; int64_t num; } u;   /* void arms omitted */
};
```

Identical to the schema one-of lowering (`Name_<arm>` enum spelling), so
a schema-generated union and a hand-declared variant with the same arms
are layout-compatible by construction.

## 4. Construction: designated initializers, tag auto-filled

There are no generated constructor functions; `RedisValue_num` (the tag
constant) is the only generated name. Construction is the standard C
designated initializer naming exactly **one** arm; the compiler fills
the tag and the `u.` path:

```c
RedisValue v = { .num = 42 };
RedisValue s = { .str = cc_string_from(...) };
*cell = (RedisValue){ .num = value };   /* transition: old arm dropped */
*cell = { .num = value };               /* braced assignment: the LHS type
                                           resolves the arm */

/* lowers to exactly the by-hand form: */
(RedisValue){ .kind = RedisValue_num, .u.num = 42 }
```

- Naming two arms in one initializer is a compile error citing both
  arms.
- Braced assignment (`lvalue = { .arm = expr };`) is legal on variant
  lvalues: braces are init-only in C, so the form is unambiguous and the
  LHS type resolves the arm.
- Void arms: `Signal s = { .hup = {} };` (empty-braces designator,
  lowered to tag-only init). The raw `{ .kind = Signal_hup }` also
  remains valid on the unpacked layout.

## 5. `.kind` and designators in expression position

**`.kind` (read-only pseudo-member, portable).** `v.kind` reads the
variant's tag on every layout: a plain field read on unpacked, a
compiler-emitted decode on packed (§11). It is never assignable; tags
change only via construction and transition (§6). Its type is the
generated `NameKind` enum.

**Designators in expression position.** A bare `.arm` designator is
legal wherever an expected variant-kind type resolves it — braces are
init-only and designators are member-access-only in C, so both forms are
syntax holes with no ambiguity:

```c
if (v.kind == .num) { … }        /* comparison partner resolves the arm */
RedisValueKind k = .num;         /* declared type resolves the arm      */
switch (v) { case .num: … }      /* subject type resolves the label     */
```

`v.kind == .arm` is the **portable** comparison. The lowered `Name_arm`
spelling remains valid for context-free and interop uses on the unpacked
layout.

## 6. Consuming a variant: protected projection + checked switch

There is no dedicated match statement (see Appendix A). Consumption uses
two patterns the language already owns:

**6a. Protected arm projection (the workhorse, expression position).**
Arm access is member-spelled — `v.num`, `cell->str` — and the checker
enforces that every projection is *protected*: legal only when

- it is **dominated by a kind check** the checker can see (v1 scope: an
  enclosing case of a variant switch, or a directly enclosing
  `if (v.kind == .arm)` in the same block), or
- it carries a protecting suffix — a **`!>` handler** (diverges) or a
  **`?>` fallback** (substitutes a value of the arm's type). Either
  suffix protects the projection; the suffix runs iff the arm is not
  active:

```c
int64_t port = cell->num ?> 6379;          /* fallback value form        */
int64_t n = cell->num !> {                 /* handler form — diverges    */
    int64_t p = cell->str.to_i64()         /* rides as a suffix handler  */
                 !> { return cc_err(...); };
    ...
};
```

This is the Result rule generalized: `T !>(E)` lowers to a two-arm
tagged union whose ok-arm projection is `!>` — Result is the blessed
two-arm variant, and projection-with-suffix is the same pair of
operators doing the same job on N arms (`!>` diverges, `?>`
substitutes; one meaning each, exactly as for Results). An unprotected
projection is a compile error ("projection of arm 'num' is not
dominated by a kind check and has no !> handler").

Domination analysis is syntactic in v1: an enclosing variant-switch
case, or a directly enclosing `if (v.kind == .arm)` /
`if (v.kind == Name_arm)` in the same block. No flow analysis
(early-return elimination, `&&` chains).

**6b. Checker-blessed `switch` (the symmetric N-way case).**
`switch` over a variant gets compile-time exhaustiveness (an error
naming the missing arms, unless `default:` is present), and each case
counts as the dominating check that legalizes that arm's projections in
the case body:

```c
switch (v) {                    /* variant subject: non-integer switch is
                                   a C constraint violation, so the form
                                   is unambiguous */
    case .num: use_int(v.num);        break;
    case .str: use_string(v.str);     break;
}   /* missing an arm and no default => compile error */
```

The subject-switch with `case .arm:` labels is the portable spelling and
works on every layout. `switch (v.kind)` with `case Name_arm:` labels is
the C-interop spelling for unpacked variants and lowers identically.

Pointer subjects need nothing special — `cell->num += delta;` inside the
dominating case is plain C, mutation in place.

## 7. Transitions, drop, and moves

Same active-arm discipline as Result, generalized:

- On scope exit, the destructor of the **active arm** runs, if that
  arm's type has one.
- Whole-variant assignment (`*cell = (RedisValue){ .num = x }`) first
  drops the old active arm (if destructor-bearing), then installs the
  new arm and tag.
- An arm moved out through a projection follows the existing move
  rules: reading a destructor-bearing arm by value is a **copy** unless
  wrapped in `cc_move(v.str)`, in which case the arm is dead and the
  variant may only be re-assigned, not read (checker-enforced where
  provable).

## 8. Interplay with `!>` / `@errhandler`

Orthogonal by design: `!>` stays the *error* channel, `@variant` the
*data* channel. A function returning `RedisValue !>(CCError)` composes
both with no new rules — `v = f() !>;` then project or switch on `v`.
There is no separate variant-propagation operator; `!>` on a projection
is the propagation-or-handle form, uniformly for Results and variants.

## 9. Companion stdlib surface

The stdlib surface that pairs with variants is specified normatively in
`spec/concurrent-c-stdlib-spec.md` and `spec/concurrent-c-spec-complete.md`;
the summaries here are cross-references.

### 9.1 `char[:].to_i64()` / `to_u64()` / `to_f64()`

Strict slice-to-number conversion returning `T !>(CCError)`:
full-slice consumption, base 10, no whitespace or `'+'`; malformed or
empty input is `CC_ERR_PARSE`, out-of-range is `CC_ERR_INVALID_ARG`.
See stdlib spec §1.3.

### 9.2 Arena-less `@string` — bounded-template stack form

`@string(`…`)` with no arena is legal iff every interpolation has a
statically bounded formatted width (integer types, `bool`, `char`).
Lowering emits a block-scoped stack buffer sized exactly from the bound
and yields a `char[:]` borrow (stack provenance, block lifetime). An
unbounded interpolation without an arena is a compile error naming the
interpolation and suggesting an arena. The arena form is unchanged and
yields an owned `CCString`; fixed-arena exhaustion poisons the result
rather than truncating it. See main spec §9.1.2–§9.1.3.

### 9.3 `cc_add_i64_checked` / `cc_sub_i64_checked` / `cc_mul_i64_checked`

Overflow-checked `int64_t` arithmetic returning `int64_t !>(CCError)`;
overflow is `CC_ERR_INVALID_ARG`. See stdlib spec §1.5.

## 10. Example: `db_incrby`

```c
@variant RedisValue { str: CCString; num: int64_t; };

static RedisReply !>(CCError) db_incrby(RedisDb* db, char[:] key,
                                        int64_t delta, int64_t* now_cache) {
    @errhandler(CCError e) { return cc_err(e); }
    RedisValue* cell = db_lookup_live(db, key, now_cache);
    int64_t value = delta;
    if (cell) {
        int64_t base = cell->num ?> ({      /* fallback: parse str arm */
            cell->str.as_slice().to_i64()
                !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                     "ERR value is not an integer or out of range")); };
        });
        value = cc_add_i64_checked(base, delta)
            !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                 "ERR increment or decrement would overflow")); };
        *cell = (RedisValue){ .num = value };  /* str arm dropped, int installed */
    } else {
        db_set(db, key, (RedisValue){ .num = value }, 0) !>;
    }
    return cc_ok(reply_integer(value));
}
```

The hot path (existing int cell) does no parsing and no text encoding;
rendering moves to the read boundary (a `.num` cell formats at reply
time via `@string` into the reply arena). Wire behavior is unchanged —
this is a storage representation change carried by the type system.

## 11. Packed representation

`@variant(packed) Name { … }` opts a variant into **niche packing**: the
discriminant lives in invalid representations donated by the arms, not
in a tag field. Packing is part of v1 precisely because it forces the
surface to be layout-agnostic from day one (§2).

**Mechanism (niche packing).** The discriminant lives in a bit pattern
that one arm's type can never hold. Niches come from two sources:

- **Inferred** (compiler knows the invalid representations): a raw
  pointer arm donates the null pattern and its low alignment bits; a
  `void` arm carries no payload and needs none.
- **Declared** (the type states its own invalid representations): a type
  registers a *niche descriptor* — an (offset, width, sentinel-value)
  the type guarantees a valid instance never exhibits — through the
  `cc_type_register` comptime hook, the registry the checker already
  consults. The packing engine reads it there.

A type's first word is **not** assumed to be a pointer: `CCString` is
SSO (its first word holds inline bytes or a pointer), so no sentinel
over word 0 is sound. `CCString` instead **declares** a niche in its
`cap` field — it already reserves `len == UINT32_MAX` as the poison
sentinel, and reserving a `cap` sentinel for "these bytes are a
niche-packed non-string" is the same kind of reservation.

Example — packed `RedisValue` in **16 bytes** (`sizeof(CCString)`), tag
word gone:

```
cap field != sentinel → kind=str, all 16 bytes are the CCString
cap field == sentinel → kind=num, bytes 0..7 hold the FULL int64
```

No bit theft from payloads: `num` keeps all 64 bits, `str` keeps all 16
bytes. A variant packs only if the compiler can prove a lossless niche
from inferred or declared sources; otherwise it refuses (below).

**Rules:**

- Opt-in only; the default layout stays the honest C struct of §3.
- Compiler-proved or refused: if no lossless packing exists, the
  declaration is a compile error stating which arms need which bits
  ("both arms require all of word 0"). Never a silent fallback to
  unpacked.
- The portable dialect works unchanged: construction, braced
  assignment, projection, subject-switch with `case .arm:`, and
  reading `v.kind` (compiler-emitted decode, visible in `--keep-c`).
- Raw `v.u` access and writing `kind` are compile errors on packed
  variants — there is no tag field and no exposed union.
- Packed layout is a private representation: no layout-compat promise
  with schema one-of; wire types stay unpacked.
- Drop/transition semantics are identical (dispatch on the decoded
  kind).

**Adoption gate (house rule: measure first).** A variant adopts `packed`
on a measured number (footprint or throughput), not a vibe. For variants
that sit by the millions in one collection, container-level out-of-band
tags (a tag byte in the container's own cell metadata, untagged payload
union) can beat type-level packing at zero type-system cost; type-level
packing wins for variants that travel (channels, arrays, arenas).

## 12. Schema convergence (deferred)

`@grammar(schema) X { one of […] }` should eventually *declare* the same
type `@variant X` would, with parse/write projections attached — one
type, optionally wire-aware. Until then the shapes are layout-identical
by §3, and a hand-written `@variant` can be cast-bridged to its schema
twin. Blocking detail: schema arms are anonymous product structs
(`u.bulk.data`), while variant arms are single types; unification needs
either named payload structs in schema lowering or multi-field arms in
`@variant`.

## Appendix A: Alternatives

**A match statement is not provided.** Channel multiplexing is temporal
(which event fires first); variant branching is spatial (which arm a
value holds) — one construct for both is false economy. Most real
variant consumption is asymmetric ("give me the int arm or handle the
other case"), which projection-with-suffix serves with zero ceremony;
the symmetric N-way case is served by the checked `switch`, keeping the
brand "the compiler makes plain C forms safe" rather than replacing C
forms. Channel multiplexing goes through `cc_chan_match_select(...)`
directly (main spec §8.5.1); the `@match` keyword is reserved. If
completion-select ever earns admission, it would be a value-producing
expression consumed by §6b's checked switch — not a statement.

**Constructor functions are not generated.** C's enum constants and
functions share one identifier namespace, so `RedisValue_num` the tag
and `RedisValue_num(...)` the constructor cannot coexist; any renaming
scheme (underscores, type-scoped dot forms) trades that collision for a
magic namespace. Designated initializers already name exactly one arm,
give the compiler the tag for free, and keep the tag constants as the
sole generated names.
