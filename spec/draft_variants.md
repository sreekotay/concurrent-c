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

## 4. Construction and accessors

Generated per-arm constructors. The idiomatic surface is the TYPE-SCOPED
DOT form, riding the existing type-scoped UFCS convention (same lowering
contract as `Tweet.parse(...)` -> `Tweet_parse(...)`):

```c
RedisValue v = RedisValue.num(42);      /* surface: type-scoped UFCS      */
RedisValue s = RedisValue.str(cc_string_from(...));
Signal    hup = Signal.hup();           /* void arm: no payload           */
/* RedisValue_num(42) etc. remain valid — the lowered/C-interop spelling. */
```

Case labels keep the underscored enum constants in v1
(`case RedisValue_num:`) — constant-expression position, deliberately the
raw-C surface; teaching the rewriter that a non-call `RedisValue.num` in
case position denotes the tag is a purely additive fast-follow if the
asymmetry grates (recorded in §11).

Raw `v.kind == RedisValue_num` and `v.u.num` remain the C-interop truth
(as with Result, they're interop detail, not preferred surface style).

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
switch (v.kind) {
    case RedisValue_num: use_int(v.num);        break;
    case RedisValue_str: use_string(v.str);     break;
}   /* missing an arm and no default => compile error */
```

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
- Whole-variant assignment (`*cell = RedisValue_num(x)`) first drops the
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
2. **`@fmt(buf, `...template...`)` → `char[:]`** — `@string`'s
   stack/borrowed-destination sibling: formats into a caller buffer,
   returns the written slice, never allocates. Overflow contract is FAIL
   LOUDLY: debug trap; not silent truncation (§10.3).
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
        int64_t base = cell->num !> {          /* not already a number */
            int64_t parsed = cell->str.as_slice().to_i64()
                !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                     "ERR value is not an integer or out of range")); };
            parsed;                             /* handler yields the parse */
        };
        value = cc_add_i64_checked(base, delta)
            !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                 "ERR increment or decrement would overflow")); };
        *cell = RedisValue_num(value);       /* str arm dropped, int installed */
    } else {
        db_set(db, key, RedisValue_num(value), 0) !>;
    }
    return cc_ok(reply_integer(value));
}
```

The hot path (existing int cell) does no parsing and no text encoding;
rendering moves to the read boundary (GET on a `.num` cell formats at
reply time via `@fmt`). Wire behavior is unchanged — this is a storage
representation change carried by the type system.

## 11. Open questions

1. **(Dissolved.)** Earlier drafts debated `@match` case spellings; the
   construct was dropped entirely (§11.5), so no case grammar exists to
   spell.
2. **Multi-field arms.** Schema variants have them (`bulk [len, data]`).
   `@variant` v1 says one type per arm (use a struct); revisit at §8
   unification.
3. **`@fmt` overflow.** Draft says debug-trap. Alternative: return
   `char[:] !>(CCError)`; rejected for v1 — formatting an int into a
   sized-right buffer failing is a programmer error, not an environment
   condition.
4. **Value-yielding handlers.** Today every `!>` handler DIVERGES
   (return/goto). The db_incrby demo uses a handler whose last expression
   yields a substitute value for the projection (`unwrap-or-else`). That
   is a semantic extension to `!>` and must be specified with the variant
   work (or the demo rewritten with an early-return shape). It applies to
   Results identically — arguably a gap the error side already has.
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
7. **Dot-form tags in case labels.** `case RedisValue.num:` (non-call,
   constant position) as sugar for the enum constant — additive
   fast-follow; v1 keeps underscored tags in switch.
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
