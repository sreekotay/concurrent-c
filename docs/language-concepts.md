# Concurrent-C Language Concepts

Spec: [concurrent-c-spec-complete.md](../spec/concurrent-c-spec-complete.md).
Recipes: [examples/README.md](../examples/README.md#learning-path-recommended-order).

---

## 1. Cleanup binds to a place

[recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs) · Spec §5.1 / §4.2.2 / §2.2

`@destroy` attaches cleanup to **successful declaration construction** — `@defer`
sugar on the binding (same scope-exit LIFO ledger). After `!>`, construction
succeeded only if the unwrap did (no binding → no destroy).

The destroy chain is: registered pre-destroy → `@destroy { body }` if present
→ registered destroy → each **value** field whose type has a hook,
last-declared to first, transitively. Pointer, array, and function-pointer
fields are omitted. Bodyless `@destroy` emits that list (without a
call-site body). An empty chain is a compile error; a block body alone is
enough to make it non-empty. `x.destroy()` is UFCS (`Type_destroy` when
that function exists).

| Bind | Meaning |
|------|---------|
| `@defer` | A **statement**: run this when the **scope** exits (LIFO) |
| `@destroy` | Same defer, on **successful declaration construction** |

```c
CCFile f = cc_file_open(path) !> @destroy;

/* !> unwraps (or routes E); @destroy runs if construction succeeded */
CCNursery n = cc_nursery_create() !> @destroy;
```

Named `@defer` can be disarmed with `@cancel_defer`; `@defer(ok)` / `@defer(err)` gate on result returns.
Registration is visible in source; discharge sites (soft-return epilogue,
cancelled-resume, never-entered `env_drop`) are defined by the spec emit.

---

## 2. Errors map to a value or to control flow

[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs) · [hello.ccs](../examples/hello.ccs)

Fallible work returns `T!>(E)` or `T?>(E)`. Both share the same ABI and consume
operators; the marker sets consumption policy:

| Return       | Bare `f();` |
| ------------ | ----------- |
| `T!>(E)`     | ill-formed  |
| `T?>(E)`     | well-formed (optional ignore) |

Two operators; three modifiers:

| | Maps |
|--|--|
| `?>` | `E → T` — stay a value (`x ?> default`) |
| `!>` | `E →` control flow — leave (`x !> { … }` / `x !>;`) |

| Modifier | Does |
|----------|------|
| `(e)` | Exposes `E` (`x !>(e) { … }` / `x ?>(e) …`) |
| bare `!>` | Routes `E` to the scope's `@errhandler` (`x !>;`) |
| `@destroy` | Cleanup on **successful declaration construction** |

```c
@errhandler(CCError e) cc_error_exit(e);   // bare !> routes here

int a = read() ?> 30;
int b = read() !>;
int c = read() !>(e) { /* local */ @err(e); };
log_line(msg);                           // T?>(CCPrintError) — bare ok
```

**Anti-pattern — custom `E` to force handling:** Do not pick `T!>(MyError)`
instead of `T!>(CCError)` hoping bare `!>` becomes unusable without “real”
handling. Bare `!>` routes to an in-scope `@errhandler` whose parameter type
matches `E`; without one, the call is ill-formed — but
`@errhandler(MyError e) cc_error_exit(e);` restores the same log-and-exit
policy. Callers can still propagate with `!>(e) return cc_err(e);` without
reading `e`. Prefer `T!>(CCError)` for cross-cutting fallible work. Use a
domain error type when the **payload** matters at the boundary; register
`@typeview on MyError { as: base; }` when CCError dispatch must participate
(script default handler, `@for (…, … in …) !>;`, shared `@errhandler`).

---

## 2a. Data alternatives are `@variant`

[Cheatsheet](cheatsheet.md#variant--data-alternatives) ·
[recipe_variant.ccs](../examples/recipe_variant.ccs) ·
[spec/draft_variants.md](../spec/draft_variants.md)

`T!>(E)` is the function error channel. `@variant` is ordinary tagged data:
one named arm is always active. Construct with exactly one designator — bare
`{0}` is a compile error (it zero-initializes to the first arm tag, not an
empty value). Project only when a `switch` / `kind ==` / `?>` / `!>` protects
that arm. The variant is data; reading an inactive arm is fallible, so `?>` /
`!>` apply to the projection.

Checked `@switch` accepts a value, pointer, or field-path subject (`h.cell`,
`r->del`). Every arm must appear (`default:` forfeits the check). Use
`case .arm(bind):` to bind the dominated payload in the case body; void arms
cannot bind.

```c
@variant Cell { txt: CCString; num: int64_t; };

Cell c = { .num = 42 };
int64_t n = c.num ?> 0;
c = { .txt = cc_string_from("hi", a) };

@switch (c) {
    case .num(v): use_i64(v); break;
    case .txt(s): use(s.as_slice()); break;
}
```

---

## 3. Methods are ordinary functions

[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs) · [recipe_user_generics.ccs](../examples/recipe_user_generics.ccs)

`recv.method(args)` calls the function the receiver's type names.
**Declaring that function is installing the method** — no trait, no header edit,
and (for this ordinary path) no `@typehooks` step. Method and free forms
are the same API (`v.push(10)` ↔ `CCVec_int_push(&v, 10)`); Concurrent-C examples
prefer the method form. Stdlib families may also register a `.ufcs` lowerer for
naming/rewrite policy; that is optional library machinery, not required to add
a method — see [getting started](getting-started.md#ufcs--methods-are-ordinary-functions)
and the [@typehooks / @typeview tutorial](typehooks-typeviews.md).

```c
/* extend: one declaration */
static double CCVec_double_median(CCVec_double* v) {
    return v->len ? v->data[v->len / 2] : 0.0;
}

v.median();           // that function
v.push(10);           // CCVec_int_push(&v, 10)
v.truncate(n);        // shrink len; n >= len is a no-op
mean(u, 6.0);         // plain C
u.mean(6.0);          // same call
```

Parameter order for methods: **receiver first, arena last** (when an arena is needed).
That is what makes `s.clone_into(a)` and `clone_into(s, a)` the same shape.

Generics: `Name::[args]` instantiates a factory (`CC_GENERIC_FACTORY`).
`Vec::[T]`, `Map::[K,V]`, `ArrayMap::[K,V]`, and non-char `T[:]` are that
rule — Vec is the struct (`v.push`), Map/ArrayMap sugar is `Name*`
(`m->insert`). File-scope `Vec::[T]`, a header `typedef Vec::[T] Alias`,
and a Vec field on a header struct keep that UFCS — including `@typehooks`
owners (`d->runs.truncate(n)` is the Vec, not the outer type). Arguments
may be types or non-negative decimal integers
(`SmallVec::[int, 8]`). Recipe:
[recipe_user_generics.ccs](../examples/recipe_user_generics.ccs).

Fallible chain: unwrap (`!>` / `?>`), then the next method sees the value.

**Print** (include `<ccc/stdio.cch>`): prefer **`io.println`** when a `CCStdio` handle is in scope. Naked `println` / data-first `.println()` remain valid. Console print returns `void ?>(CCPrintError)` — bare `println` is well-formed; use `!>` only when failure must propagate. Templates need an arena — prefer `@scratch` for throwaways.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(a);
io.println("hi");
io.println(@string(`n=${n}`, @scratch));
/* also fine: println("hi");  /  @string(`…`, @scratch).println(); */
```

---

## 4. Slices remember where bytes live

[recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs) · [recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) · [recipe_long_lived_store.ccs](../examples/recipe_long_lived_store.ccs) · [Allocator strategy](../spec/draft_alloc_strategy.md) · [Restricted access](../spec/draft_facets.md)

**An arena names a lifetime. Its allocation strategy is an implementation
policy for storage belonging to that lifetime.**

The `CCArena` binding *is* that lifetime (the epoch). Size the root for the
typical live set of that lifetime; choose heap/stack/fixed/overflow as
**policy** for how its storage is obtained — not a separate allocator identity.
Slices are views — `T[:]` is `{ptr, len, id}`; storing it does not take the
bytes. Provenance (stack, arena, static, unique, …) is what the compiler uses
to reject views that outlive their storage. `id == 0` is untracked.
`char[:0]` is the NUL-terminated refinement — prefer `char[:0] s = "hi";` for
string literals (`len` is the payload; `ptr[len]` is `0`). `is_cstr` on the
slice id survives erase to `char[:]`; `s.to_c(arena) !>` returns `char[:0]`
and copies only when the bit is clear. `s.to_cstr(arena) !>` is
`s.to_c(arena) !>.ptr`. How those facts compose into an object
(constructors assume dead, epochs as fields, failure is unchanged):
[getting started — locality](getting-started.md#locality-owned-or-view) ·
[recipe_owned_view.ccs](../examples/recipe_owned_view.ccs).

Ordinary slice sites may read `.ptr`, `.len`, and `.id`; they may not
store fields. Typed `T[:]` carries those fields on `.base`. The walk is `@for (v in s)` (also enumerate /
zip / range): the compiler pays `i < live .len` and loads. `v` is a copy —
`v =` and `&v` are ill-formed. `@for (&v in s) { … } !>;` is the mut walk:
`v = x` stores through the same `.access` peel as the load (slice, vec,
string, `T[n]`). `.len` / `.access` return the naked bound and slot —
the hook is not Result. A slice / `T[n]` snapshots `.len` and the data
pointer at entry; a grower does the same when the body does not resize
it. If the body can change the subject's extent, a write re-reads `.len`;
`i >= len` is `CC_ERR_INVALID_ARG` (`"for-in write"`).
That check is the mut walk's Result, not a skip. The subject is
a name, a field path (`t->words`), or a view (`line.sub(start, line.len)` —
hoisted to a hidden local; mut walk stores through that header). Users do not write `s.access(i)`.
Zip is a statement that can fail: `@for (a, b in s, t) { … } !>;`.
`@for (&a, b in s, t) { … } !>;` stores through `a`'s subject. Copy walk, enumerate,
and range are not Results: a trailing `!>` is ill-formed. Mut walk and
zip consume `!>`.
A guess at a slot is `s.at(i) !>` / `s.set(i, v) !>`. Compare slices with
`s.eq(other)` / `s.eq_cstr("x")`, not `memcmp`, in CC examples. Point through a
local when you need C indexing: `char *p = s.ptr; p[i]`. `CCString`
is not a slice: `.data` is the SSO union — use `as_slice()` / `cstr()`, or dest-init `char[:] v = s`.
`int[:] xs = v` dest-wraps `Vec::[int]` the same way; `@for (x in v)` still
walks the live vec. `T*` is not an extent. C `for (;;)` is unchanged. Tutorial:
[@typehooks / @typeview](typehooks-typeviews.md#extent--len--access).
Recipe: [recipe_walk.ccs](../examples/recipe_walk.ccs).

| Arena | Lifetime + storage policy | Typical use |
|-------|---------------------------|-------------|
| `cc_arena_heap(n) @destroy` | Named lifetime; L1 heap `n`, L2 up to 4 slabs (~1.5×), then **Main** overflow | request / window |
| `cc_arena_stack(name, n)` | Same lifetime; L1 on the stack; `@destroy` at scope exit | hot-path / frame scratch |
| `cc_arena_buf(name, ptr, n)` | Same sugar as stack; caller L1 (no VLA) | existing buffer / `#define` scratch |
| `@scratch` | Arena operand of `@string` only — not a named `CCArena` | bind the `CCString`, or call-local print |

```c
/* Heap — owns the root; @destroy frees slabs + overflow. */
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(a);
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);   /* arena provenance */
/* C API: pass CCArena by value — cc_arena_alloc(a, n, align), cc_dir_cwd(scratch); not &a */

/* Stack — buffer lives in the frame; @destroy frees L2/Main at scope exit. */
cc_arena_stack(tmp, 1024);
char* q = tmp.allocT(32);

/* Scratch — @string arena operand only; bind before return. */
io.println(@string(`len=${a.remaining()}`, @scratch));

char[:0] hi = "hi";
```

**Auto overflow (policy detail):** when the current slab cannot fit an alloc,
heap/stack arenas grow L2 within `block_max` (default 4), then spill to Main
`malloc`. Main bytes still belong to that same named lifetime — `reset` /
`@destroy` frees them too. A tiny L1 still allocates; it just spends more
time in Main. Prefer another arena when lifetimes diverge; treat
`cc_arena_release` / Main as escape hatches, not the steady path. Fixed
arenas with overflow off return `NULL` on exhaustion (never silent success).
`a.live()` counts every live object on L1 + L2 + Main.

**Checkpoint / restore:** a checkpoint is a consumed loan —
`a.try_checkpoint() !>` / `cp.try_restore() !>` (or `@destroy` on the handle).
Restore rewinds the slab prefix and drains Main minted after the checkpoint
(same contract on heap, stack, and `cc_arena_malloc`). Overflow alloc does
not disable rewind. A mid-slab hole disables a new capture until last-live
root rewind or `reset`. Restore refuses (no mutate) if that handle's overflow
keep-set was released, or if the checkpoint would advance the tip. Dropping
a handle without consume leaves an outstanding loan (diagnostic on
free/reset/detach) and does not block a later capture. `a.detach() !>` refuses a
stack or caller-owned L1.

A view must not outlive its storage — no stack/arena borrow into an outliving
task or channel send. Capturing a non-unique arena slice into a nursery **pins**
that arena’s epoch until join (reset/destroy while pinned is a compile error).
Own crossing that boundary: unique (`T[:!]` / `cc_adopt`), static, or write into
the channel (`send_into`). Do not send or capture a slice from `@scratch` or a
stack arena past the frame.

Same `[…]` family: `T[n]` arrays, `T[~n >]` / `T[~n <]` channels.

---

## 5. Closures carry captures

[recipe_explicit_capture.ccs](../examples/recipe_explicit_capture.ccs)

`@parallel` names are the frame: a pointer copies; every other name is
the same object. There is no list. Spawn / `send_task` take a closure.
`[x]` copies `x` into the task and the copy is immutable. `[&x]` shares
the same variable for read-only access; mutation requires shipped
synchronization or an explicit `@unsafe` closure.

```c
n.spawn(() => [x] { use(x); });   // immutable value copy
n.spawn(() => [&x] { use(x); });  // shared, read-only reference
```

Stack slices cannot be captured. Arena slices only while the arena outlives the join.
Tasks do not inherit `@errhandler` — re-bind inside if you use `!>;`.

---

## 6. Absence is not a Result

No `T?`. Pick the shape that matches the operation:

- **Missing** → `T*`, bool+out, or a sentinel (empty slice — also “not one piece”)
- **Failed** → `T!>(E)` (§2)

---

## 7. Nurseries are the open bag

[recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) · Spec §8.1

A nursery is a join set with a handle when the set is not on the page
(late `n.spawn`, host, retract). Names on the page are `@parallel` (§8).
Lifecycle:

```
OPEN ──spawn*──┬── JOINING ── EMPTY ── DEAD     owner stays (wait / @destroy)
               └── LEFT    ── EMPTY ── DEAD     owner gone (leave)
```

| Call | Path |
|------|------|
| `wait` / `@destroy` | OPEN → JOINING → EMPTY → DEAD |
| `leave()` / `leave(ctx, finish)` | OPEN → LEFT → EMPTY → DEAD |
| `close(tx)` | arms EMPTY to close `tx` on both paths — not teardown |

Self-owned: `cc_nursery_create() !> @destroy` (or `leave`). Arena birth:
`a.create_nursery()` — handle lives in `a`; arena walk joins (no `leave`).
`n.spawn` admits children; `n.cancel()` is cooperative. Use either
`@destroy` / `wait` or `leave`, not both. Leftover at EMPTY runs only on
the LEFT path (`n.leave(ctx, finish)`), not on wait / `@destroy`.

---

## 8. `@parallel` joins independent work

[recipe_parallel.ccs](../examples/recipe_parallel.ccs) ·
[recipe_parallel_stream.ccs](../examples/recipe_parallel_stream.ccs) · Spec §8.11

`@parallel` is a lexical fork-join: the siblings are on the page. It is
not a nursery. The brace and
`for` forms are `CCParallel !>(CCError)`: create can fail; `.wait()` is
the join. A dest is live before the arms. Bare `@parallel { }` is an
unconsumed Result — bind (`CCParallel h = … !>;`) or `!>.wait()!>;`.
Binding starts the arms and does not join. When there is a kick, the
first arm has finished when the construct returns `h`; siblings may
still be running. A dest bound to one assignment arm is ill-formed:
this dest is never live on the caller. A dest bound to one expression
arm (`CCParallel h = @parallel { work(); } !>;`) is the worker
(spawned); dest is live. Join with `!>.wait()!>`, or bind the dest.
`h.wait()` joins them and publishes their writes. Pointer names copy
the pointer; other captured names are the frame object and must
outlive `.wait()`. `h.close(tx)` arms EMPTY to close `tx` on wait and
leave. `h.leave()` consumes the handle without joining; leftover runs
at EMPTY on the LEFT path only (`h.leave(ctx, finish)`). Do not mix
wait and leave. `h.cancelled` and `h.paused` are
atomic. `h.cancel()` is `true` when this call stored live→cancelled.
`h.live()` is planted and not joined or left. `cc_parallel_empty()` is idle.
After `h.wait()`, `h.joined` and `!h.live()`. Pause / resume / cancel
of idle or joined are `ok(false)`.
`h.pause()` / `h.resume()` flip `h.paused` on a live dest; poll
`h.paused` or `h.paused()`. They do not require `.wait()`.
The construct honors `paused` at thunk entry, the next
`@parallel for` half or leaf iteration, wait-for enter, and after
`@stage` wait (`cc_parallel_honor`). Cancel is a mark and does not
skip a thunk. It wakes parks on attached fibers. `.wait()` does not
resume. Pause does not complete a `recv` or `@stage` wait.
Spawned arms do not inherit `@with_deadline`; they poll `h.cancelled`.
When several arms share one deadline, name it (`as dl`) and use `dl`,
or write `@with_deadline(dl)` to make that object current.
`n.spawn` names a lifetime that may outlive the spawn point (`@destroy`
waits that nursery; `n.leave()` consumes the handle without joining).
Independent names stay `@parallel { }`. A meeting (blocking send/recv,
or a captured channel) is `@parallel spawn { }`: spawned arms are not
denied. The same shape without `spawn` is ill-formed, except under
`#pragma(@parallel) off`. A denied join that then parks on a channel
aborts.

| Form | Meaning |
|------|---------|
| `@parallel { a = f(); b = g(); }` | Independent assignment arms. First on the caller; the rest may spawn. |
| `@parallel spawn { … }` | Same arms. Spawned siblings are not denied. |
| `@parallel(h) { … }` | Growing dest: admit onto `h`. Statement; snapshot; never deny. Live set. |
| `@parallel spawn { @serial { …; tx.close(); } @serial { while (recv) } }` | On-page stream. Close next to produce. `.wait()` joins both. Not a nursery. |
| `CCParallel h = @parallel { … } !>;` | Starts arms; does not join. `h.live()` until `h.wait()` or `h.leave()`. Arms may cancel/adopt/pause `h`. `h.wait()` inside an arm of `h` is an error. |
| `@serial { …; a = t; }` | Sequential block as one sibling. Ordinary C; zero or one outer name. |
| `@parallel (pred) { … }` | Same arms. Spawn if `pred`; otherwise run in order. The body always runs. |
| `@parallel for (i in lo..hi) { … }` | Independent iterations over `[lo, hi)`. Bisects; a span of 0 or 1 is a plain `for`. `return` is `break` then `return` from the function after the join. |
| `@parallel wait (ts) for (i in lo..hi)` | Ordered spawn loop on a turnstile. Type: `bool !>(CCError)` — `true` if the range finished. `CCParallel h = … !>;` is live during enter; the statement joins. A targeting `break` is `ok(false)` and must be bound. `return` drains, then leaves the function. `@stage` is a handshake, not a Result. |

```c
int a = 0, b = 0;
@parallel {
    @serial {
        int t = f();
        a = t;
    }
    b = g();
} !>.wait()!>;

@parallel for (y in 0..h) {
    row(y);
} !>.wait()!>;

CCTurnstile ts@(cap, 1, arena) !> @destroy;
@parallel wait (ts) for (i in 0..n) {
    int v = work(i);
    @stage (ts, 0, i) {
        packed[pos] = v;
        pos++;
    }
} !>.wait()!>;
```

`@serial` is legal only as a direct child of `@parallel { }`. A bare `{ }`
is not an arm. A `for` as a direct child of `@parallel { }` is a compile
error; `for` inside `@serial` is ordinary C.

`return` in any of these forms drains in-flight work, then returns from
the function. The construct does not wait for a ticket that has not
returned. If two arms or iterations both `return`, which value is taken
is not specified. Sequential `seq` / `#pragma(@parallel) off` is ordinary C.

---

## Next

[Getting Started](getting-started.md) · [Cheatsheet](cheatsheet.md) ·
[recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) ·
[recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) ·
[recipe_async_await.ccs](../examples/recipe_async_await.ccs) ·
[recipe_parallel.ccs](../examples/recipe_parallel.ccs)
