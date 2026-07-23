# Draft: `@variant` — first-class tagged unions

Status: HISTORICAL DRAFT. Nothing here is implemented; where this
document disagrees with the shipped compiler, the compiler wins until this
graduates into the spec proper.

> **Note (2026-07):** the `@match` statement referenced throughout this
> draft has since been **removed from the language** (the keyword stays
> reserved). Sections that extend `@match` are superseded: the current
> variants direction uses **protected projection + a checker-enforced
> `switch`** over the variant tag — ordinary control flow consuming a
> value, never a statement form. Channel multiplexing goes through
> `cc_chan_match_select(...)` directly (spec §8.5.1).

## 1. Motivation: the language already has three tagged unions

1. **Results.** `T !>(E)` lowers (normatively, spec §"Result") to
   `struct { bool ok; union { T value; E error; } u; }` with active-arm drop
   semantics and move-out on `!>`.
2. **Schema one-of.** `@grammar(schema) ... one of [...]` lowers to
   `typedef enum { Reply_simple, ... } ReplyKind;
   struct Reply { ReplyKind kind; union { ... } u; }` — now with both parse
   and write projections.
3. **`@match`.** Exists today for channel multiplexing with typed-binding
   cases (`case int x = @await ch:`) and an implicit cancellation arm.

What's missing is the plain **data-model** sum type. The cost shows up in
real code — `real_projects/redis/redis_idiomatic.ccs` `db_incrby` stores
integers as decimal text because `RedisValue` can only hold a string, so
every INCR pays parse → checked add → re-encode → copy, with two hand
helpers and a four-line stack-buffer dance. Upstream redis solves this with
`robj` int-encoding flags and manual discipline; the type system should
carry it instead.

Design pressure comes from two deliberately different shapes in the same
program:

- **RESP reply** (wire boundary): closed set, dispatch byte, recursive,
  needs parse/write projections → already served by schema one-of.
- **RedisValue** (data model): evolving set, no dispatch byte, needs cheap
  in-place transitions and destructor discipline → served by nothing today.

## 2. Principles

- **Lowered C is first class.** The lowering is normative, ABI-stable, and
  identical to the schema one-of shape. C interop reads `v.kind` /
  `v.u.<arm>` directly; the CC surface is sugar over exactly that struct.
- **Converge, don't multiply.** One lowering for schema one-of and
  `@variant`; schema one-of becomes "a variant with wire projections
  attached" (§8). Result keeps its `ok` spelling for ABI stability but is
  conceptually the two-arm instance.
- **No hidden allocation.** A variant is a value type: `sizeof = tag +
  max(arm)`. Construction, match, and transition never allocate.
- **Layout-agnostic surface.** The designator dialect (construction,
  brace-assignment, `case .arm:`, projection) plus READ-ONLY `.kind` is
  the portable contract: `v.kind` always reads the tag (field read on
  unpacked, compiler-emitted decode on packed) and is never assignable —
  tags change only via construction/transition. Raw `v.u` access (and
  writing `kind`) is an interop privilege of the UNPACKED layout. This is what makes §12
  packing a representation choice instead of a semantic one.
- **FAIL LOUDLY.** Non-exhaustive `@match` without `default` is a compile
  error. Reading an inactive arm is UB in C but a compile error where
  statically provable (same rule as Result).

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

- Arm names are C identifiers (they become enum and union member names, so
  C keywords are rejected at parse: `int:` is an error — spell it `num:`).
- Recursive arms are allowed through pointers/slices only
  (`items: RespReply[:]`), same rule as C.

**Lowering (normative):**

```c
typedef enum { RedisValue_str, RedisValue_num } RedisValueKind;
struct RedisValue {
    RedisValueKind kind;
    union { CCString str; int64_t num; } u;   /* void arms omitted */
};
```

Identical to the schema one-of lowering (`Name_<arm>` enum spelling), so a
schema-generated union and a hand-declared variant with the same arms are
layout-compatible by construction.

## 4. Construction: designated initializers, tag auto-filled

No generated constructor functions. (They cannot exist: C's enum constants
and functions share one identifier namespace, so `RedisValue_num` the tag
and `RedisValue_num(...)` the constructor would collide. Earlier drafts
proposing constructors — underscored or type-scoped dot form — are
superseded.) Construction is the standard C designated initializer naming
exactly ONE arm; the compiler fills the tag and the `u.` path:

```c
RedisValue v = { .num = 42 };
RedisValue s = { .str = cc_string_from(...) };
*cell = (RedisValue){ .num = value };   /* transition: old arm dropped */
*cell = { .num = value };               /* RATIFIED: braced assignment — a C
                                           syntax hole (braces are init-only
                                           in C), trapped and rewritten; the
                                           LHS type resolves the arm */

/* lowers to exactly the by-hand form: */
(RedisValue){ .kind = RedisValue_num, .u.num = 42 }
```

- Naming two arms in one initializer is a compile error citing both arms.
- Void arms: `Signal s = { .hup = {} };` (empty-braces designator, lowered
  to tag-only init). The raw `{ .kind = Signal_hup }` also remains valid.
- `RedisValue_num` (the tag) is thereby the ONLY generated name, with one
  job: `kind` comparisons and case labels. Raw `v.kind` / `v.u.num` remain
  the C-interop truth.

## 5. Consuming a variant: protected projection + checked switch

No new statement. (`@match` is REMOVED from the language entirely — see
§11.5 — and variants deliberately do not resurrect it.) Consumption uses
the two patterns the language already owns:

**5a. Protected arm projection (the workhorse, expression position).**
Arm access is member-spelled — `v.num`, `cell->str` — and the checker
enforces that every projection is *protected*: legal only when

- it is **dominated by a kind check** the checker can see (v1 scope:
  an enclosing `case Name_arm:` of a `switch (v.kind)`, or a directly
  enclosing `if (v.kind == Name_arm)` in the same block), or
- it carries a **`!>` handler**, which runs iff the arm is not active:

```c
int64_t n = cell->num !> {                 /* not a num — the "else"     */
    int64_t p = cell->str.to_i64()         /* rides as a suffix handler  */
                 !> { return cc_err(...); };
    ...
};
```

This is the existing Result rule generalized: `T !>(E)` already lowers to
a two-arm tagged union whose ok-arm projection is `!>` — Result is the
blessed two-arm variant, and projection-with-handler is the SAME operator
doing the same job on N arms. An unprotected projection is a compile
error ("projection of arm 'num' is not dominated by a kind check and has
no !> handler").

**5b. Checker-blessed `switch` (the symmetric N-way case).** No new form:
`switch (v.kind)` over a variant kind gets compile-time exhaustiveness
(error naming the missing arms, unless `default:` is present), and each
`case Name_arm:` counts as the dominating check that legalizes that arm's
projections in the case body:

```c
switch (v) {                    /* RATIFIED: variant subject — a C constraint
                                   violation (non-integer switch), trapped */
    case .num: use_int(v.num);        break;   /* designator label: also a
    case .str: use_string(v.str);     break;      C syntax hole, trapped */
}   /* missing an arm and no default => compile error */
```

`switch (v.kind)` with `case Name_arm:` remains valid for UNPACKED
variants (C-interop spelling) and lowers identically — but the designator
form is the PORTABLE one: packed variants (§12) have no `kind` field, so
subject-switch + `case .arm:` is the only spelling that works across
representations. Same rule for if-domination: `if (v.kind == Name_arm)`
is unpacked-only; the portable guard is the projection itself.

Pointer subjects need nothing special — `cell->num += delta;` inside the
dominating case is plain C, mutation in place.

**Why not a match statement** (recorded so we don't re-litigate): channel
`@match` was temporal (which event fires first), variant branching is
spatial (which arm a value holds) — one keyword for both was false
economy; most real variant consumption is asymmetric, which projection
serves with zero ceremony; and the brand is "the compiler makes plain C
forms safe", not "replace C forms". The full alternatives analysis lives
in §11.5.

## 6. Transitions, drop, and moves

Same active-arm discipline as Result (spec §"Rule (drop for T!>(E))"),
generalized:

- On scope exit, the destructor of the **active arm** runs, if that arm's
  type has one.
- Whole-variant assignment (`*cell = (RedisValue){ .num = x }`) first drops the
  old active arm (if destructor-bearing), then installs the new arm and
  tag. This is the INCR string→int transition in one line.
- An arm moved out through a projection follows the existing move rules:
  reading a destructor-bearing arm by value is a **copy** unless wrapped in
  `cc_move(v.str)`, in which case the arm is dead and the variant may only
  be re-assigned, not read (checker-enforced where provable).

## 7. Interplay with `!>` / `@errhandler`

Orthogonal by design: `!>` stays the *error* channel, `@variant` the *data*
channel. A function returning `RedisValue !>(CCError)` composes both with
no new rules — `v = f() !>;` then project or switch on `v`. There is no
separate variant-propagation operator; `!>` on a projection IS the
propagation-or-handle form, uniformly for Results and variants.

## 8. Schema convergence (later phase, recorded now)

`@grammar(schema) X { one of [...] }` should eventually *declare* the same
type `@variant X` would, with parse/write projections attached — one type,
optionally wire-aware. Until then the shapes are layout-identical by §3,
and a hand-written `@variant` can be cast-bridged to its schema twin.
Blocking detail: schema arms today are anonymous product structs
(`u.bulk.data`), while variant arms are single types; unification needs
either named payload structs in schema lowering or multi-field arms in
`@variant`. Deferred.

## 9. Stdlib riders (land with, or before, the variant work)

Two idiom gaps that stay painful even with variants:

1. **`char[:].to_i64()` → `int64_t !>(CCError)`** (and `to_u64`, `to_f64`)
   — replaces per-project `parse_i64`. Strict: full-slice consumption,
   overflow → `CC_ERR_INVALID_ARG`-class error.
2. **Arena-less `@string`: the bounded-template stack form.** Omitting
   the arena is legal iff the template's maximum width is statically
   boundable (`${int}`/`${i64}`/`${bool}`/`${char}` + literal text).
   Lowering emits the exact block-scoped `char buf[N]` you'd write by
   hand, sized by the compiler, and yields a **`char[:]` borrow** with
   stack provenance (escape flagged where provable). An unbounded
   interpolation (`${slice}`, ...) without an arena is a COMPILE ERROR
   naming the interpolation and suggesting an arena. The arena form is
   unchanged and yields an owned `CCString`; unbounded-but-stack cases
   use `cc_arena_fixed_buffer`. (This supersedes both the earlier `@fmt`
   proposal and its withdrawal: same single surface, ergonomics by
   omission, guarded by boundedness.) Surviving pin regardless:
   `@string` on FIXED-arena exhaustion must be loud and defined.
3. **`cc_add_i64_checked(a, b)` → `int64_t !>(CCError)`** (family) —
   overflow-checked arithmetic that composes with `!>` handlers instead of
   hand-rolled `INT64_MAX - delta` guards.

## 10. Acceptance demo: `db_incrby`

Before (today, abridged — parse/encode helpers + buffer dance elided):
~20 lines, 2 hand helpers, text round-trip on every increment.

After:

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
rendering moves to the read boundary (GET on a `.num` cell formats at
reply time via `@string` into the reply arena). Wire behavior is unchanged — this is a storage
representation change carried by the type system.

## 11. Open questions

1. **(Dissolved.)** Earlier drafts debated `@match` case spellings; the
   construct was dropped entirely (§11.5), so no case grammar exists to
   spell.
2. **Multi-field arms.** Schema variants have them (`bulk [len, data]`).
   `@variant` v1 says one type per arm (use a struct); revisit at §8
   unification.
3. **Arena-less `@string` escape checking.** The borrowed slice's stack
   provenance makes escape detection best-effort (same class as existing
   slice provenance) — accepted; no worse than the hand-written buffer
   dance it replaces. Fixed-arena exhaustion in `@string` still needs its
   loud, defined, tested contract.
4. **(Resolved: no value-yielding `!>`.)** The language already has the
   expression alternative: `?>` (default-value unwrap, `x ?> fallback`).
   `!>` takes a handler and DIVERGES; `?>` takes a value and SUBSTITUTES —
   one meaning each. Variant projections extend BOTH uniformly:
   `v.num !> { diverge }` and `v.num ?> fallback`. Computed fallbacks
   with their own error exits nest `!>` inside the `?>` fallback
   expression. (Impl note for phases 1-3: confirm `?>` accepts a
   statement-expression fallback.)
5. **Domination analysis scope.** v1 accepts only syntactic domination
   (switch case; directly enclosing `if (v.kind == K)`). Anything cleverer
   (early-return elimination, `&&` chains) waits for evidence from real
   code that the `!>`-handler fallback is too noisy.
6. **`@match` removal record.** `@match` (channel multiplexing) was
   removed in the same wave that produced this draft: zero real-project
   usage, a ~250-line rewrite over `cc_chan_match_select` (which remains
   the library escape hatch), an implicit-cancellation story weaker than
   spec'd (pre-block check only), and thread-blocking select emitted even
   in fiber contexts. Fiber-per-source is the primary idiom; if
   completion-select ever earns re-admission from real code, it returns as
   a value-producing EXPRESSION (comptime-synthesized event variant,
   consumed by §5's checked switch) — never as a statement. The keyword
   stays reserved.
7. **(Superseded.)** Dot-form constructors and dot-form tags were dropped
   with the constructor functions themselves (C namespace collision +
   magic-namespacing objection); construction is designated-init (§4),
   tags are the sole generated names.
8. **Nil arms in wire types.** RESP taught us dispatch-byte collision makes
   `nil` a boundary case there; data-model variants have no dispatch byte,
   so `nil: void;` arms are fine. No rule needed — recording the asymmetry.

## 12. Implementation sketch (phases)

1. **Parse + lower `@variant`** (L2 rewriter/grammar seam): declaration →
   enum + struct + constructors. Reuse the schema one-of emitter's naming.
2. **Checker: protected projection + switch exhaustiveness**: arm
   registry populated by the declaration; projection legality (domination
   or `!>` handler) and switch exhaustiveness enforced in the checker;
   `!>`-on-projection lowering rides the existing result_unwrap machinery
   (a projection is an unwrap whose "err" arm set is every other arm).
3. **Drop/transition semantics** (unwrap-destroy/lifetime passes): active-
   arm drop on scope exit and on whole-variant assignment; move interplay.
4. **Stdlib riders** (§9): independent; can land first.
5. **Redis adoption**: `RedisValue` → variant; INCR/GET paths; bench gate
   (INCR should *improve*; SET/GET must hold).
6. **Schema unification** (§8): later wave.

Each phase gates as usual: failing-first tests, full + strict suites,
lint, and for phase 5 the redis smoke + bench parity.

## 12. Packed representation — first-class, not future

`@variant(packed) Name { ... }` opts a variant into NICHE PACKING: the
discriminant lives in invalid representations donated by the arms, not in
a tag field. RATIFIED as part of v1 precisely because it forces the
surface to be layout-agnostic from day one (§2).

**Mechanism (multi-word niches).** An arm with unusable encodings donates
them. Canonical example — packed RedisValue in 24 bytes, tag word gone:

```
word 0 == valid 8-aligned ptr → kind=str, words 0..2 are the CCString
word 0 == 0x1 (impossible ptr) → kind=num, word 1 holds the FULL int64
```

No bit theft from payloads: `num` keeps all 64 bits. Pointer-bearing arms
(strings, slices, boxed types) almost always donate a niche.

**Rules:**
- Opt-in only; default layout stays the honest C struct of §3.
- Compiler-PROVED or refused: no lossless packing exists → compile error
  stating which arms need which bits ("both arms require all of word 0").
  Never a silent fallback to unpacked.
- No `v.kind`, no `v.u` on packed variants — raw access is a compile
  error; construction/brace-assign/projection/subject-switch all work,
  with encode/decode emitted by the compiler and visible in --keep-c.
- Packed layout is a private representation: no layout-compat promise
  with schema one-of; wire types stay unpacked.
- Drop/transition semantics identical (dispatch on the decoded kind).

**Adoption gate (house rule: measure first).** Phase 5 lands RedisValue
UNPACKED with MEMLOG before/after; `packed` (or the container-level
alternative below) is justified by a number, not a vibe.

**Recorded alternative:** for variants that sit by the millions in one
collection (the redis DB cell), container-level out-of-band tags (a tag
byte in the map's own cell metadata, untagged payload union) can beat
type-level packing at zero type-system cost. Type-level packing wins for
variants that TRAVEL (channels, arrays, arenas).
