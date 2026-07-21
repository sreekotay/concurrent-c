# Draft: `@variant` — first-class tagged unions

Status: DRAFT for discussion. Nothing here is implemented; where this
document disagrees with the shipped compiler, the compiler wins until this
graduates into the spec proper.

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

Generated per-arm constructors, mirroring `cc_ok`/`cc_err`:

```c
RedisValue v = RedisValue_num(42);
RedisValue s = RedisValue_str(cc_string_from(...));
Signal    hup = Signal_hup();          /* void arm: no payload */
```

Raw `v.kind == RedisValue_num` and `v.u.num` remain the C-interop truth
(as with Result, they're interop detail, not preferred surface style).

## 5. `@match` over variants

Extends the existing `@match` statement. Subject in parens selects variant
mode (channel mode keeps its current no-subject form):

```c
@match (v) {
    case .str s:  use_string(s);      /* s: CCString  (copy of the arm)   */
    case .num n:  use_int(n);         /* n: int64_t                       */
}

@match (cell) {                        /* pointer subject: arms bind as    */
    case .str s:  s->len;              /* pointers — in-place access       */
    case .num n:  *n += delta;         /* and mutation                     */
}
```

Rules:

- **Exhaustiveness:** every arm present, or a `default:` arm. Missing arms
  without `default` = compile error naming the missing arms.
- **Binding:** value subject → arm binds by value (copy); pointer subject →
  arm binds as pointer to the live arm. No binder needed for void arms
  (`case .hup:`).
- **Lowering:** a `switch (subject->kind)` with scoped binder declarations;
  each case body is the user's text. `default:` lowers to `default:`.
- The leading-dot spelling `case .arm binder:` is deliberate — it reads as
  "this subject's arm" and cannot collide with the channel-mode grammar
  (`case T x = @await ch:`). Open question §10.1 records the alternative
  spelling that echoes the channel form.

## 6. Transitions, drop, and moves

Same active-arm discipline as Result (spec §"Rule (drop for T!>(E))"),
generalized:

- On scope exit, the destructor of the **active arm** runs, if that arm's
  type has one.
- Whole-variant assignment (`*cell = RedisValue_num(x)`) first drops the
  old active arm (if destructor-bearing), then installs the new arm and
  tag. This is the INCR string→int transition in one line.
- An arm moved out by a `@match` binding follows the existing move rules:
  by-value binding of a destructor-bearing arm is a **copy** unless wrapped
  in `cc_move`, in which case the variant's arm is dead and the variant may
  only be re-assigned, not read (checker-enforced where provable).

## 7. Interplay with `!>` / `@errhandler`

Orthogonal by design: `!>` stays the *error* channel, `@variant` the *data*
channel. A function returning `RedisValue !>(CCError)` composes both with
no new rules — `v = f() !>;` then `@match (v) { ... }`. There is no
variant-propagation operator; sum-typed control flow is what `@match` is
for.

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
        @match (cell) {
            case .num n: value = cc_add_i64_checked(*n, delta)
                             !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                                  "ERR increment or decrement would overflow")); };
            case .str s: {
                int64_t parsed = s->as_slice().to_i64()
                             !> { return cc_err(CC_ERROR(CC_ERR_INVALID_ARG,
                                  "ERR value is not an integer or out of range")); };
                value = cc_add_i64_checked(parsed, delta) !> { ... };
            }
        }
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

1. **Case spelling.** `case .num n:` vs echoing the channel form
   (`case int64_t n = .num:`). The short form wins on noise; the long form
   wins on uniformity with channel `@match`. Draft picks short.
2. **Multi-field arms.** Schema variants have them (`bulk [len, data]`).
   `@variant` v1 says one type per arm (use a struct); revisit at §8
   unification.
3. **`@fmt` overflow.** Draft says debug-trap. Alternative: return
   `char[:] !>(CCError)`; rejected for v1 — formatting an int into a
   sized-right buffer failing is a programmer error, not an environment
   condition.
4. **Nil arms in wire types.** RESP taught us dispatch-byte collision makes
   `nil` a boundary case there; data-model variants have no dispatch byte,
   so `nil: void;` arms are fine. No rule needed — recording the asymmetry.

## 12. Implementation sketch (phases)

1. **Parse + lower `@variant`** (L2 rewriter/grammar seam): declaration →
   enum + struct + constructors. Reuse the schema one-of emitter's naming.
2. **`@match` variant mode** (statement pass): subject detection, case
   rewrite to switch + binders, exhaustiveness check in the checker (arms
   known from the registry the declaration populates).
3. **Drop/transition semantics** (unwrap-destroy/lifetime passes): active-
   arm drop on scope exit and on whole-variant assignment; move interplay.
4. **Stdlib riders** (§9): independent; can land first.
5. **Redis adoption**: `RedisValue` → variant; INCR/GET paths; bench gate
   (INCR should *improve*; SET/GET must hold).
6. **Schema unification** (§8): later wave.

Each phase gates as usual: failing-first tests, full + strict suites,
lint, and for phase 5 the redis smoke + bench parity.
