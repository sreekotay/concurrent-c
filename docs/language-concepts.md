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
fields are omitted. Bodyless `@destroy` and `x.destroy()` emit that list
(without a call-site body). An empty chain is a compile error; a block body
alone is enough to make it non-empty.

| Bind | Meaning |
|------|---------|
| `@defer` | A **statement**: run this when the **scope** exits (LIFO) |
| `@destroy` | Same defer, on **successful declaration construction** |

```c
FILE* f = fopen(path, "r");
@defer fclose(f);

/* !> unwraps (or routes E); @destroy runs if construction succeeded */
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

Named `@defer` can be `@cancel`led; `@defer(ok)` / `@defer(err)` gate on result returns.
Registration is visible in source; discharge sites (soft-return epilogue,
cancelled-resume, never-entered `env_drop`) are defined by the spec emit.

---

## 2. Errors map to a value or to control flow

[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs) · [hello.ccs](../examples/hello.ccs)

Fallible work returns `T!>(E)`. Consume every result. Two operators; three modifiers:

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
mean(u, 6.0);         // plain C
u.mean(6.0);          // same call
```

Parameter order for methods: **receiver first, arena last** (when an arena is needed).
That is what makes `s.clone_into(&a)` and `clone_into(s, &a)` the same shape.

Generics: `Name::[args]` instantiates a factory (`CC_GENERIC_FACTORY`).
`Vec::[T]`, `Map::[K,V]`, `ArrayMap::[K,V]`, and non-char `T[:]` are that
rule — Vec is the struct (`v.push`), Map/ArrayMap sugar is `Name*`
(`m->insert`). Recipe:
[recipe_user_generics.ccs](../examples/recipe_user_generics.ccs).

Fallible chain: unwrap (`!>` / `?>`), then the next method sees the value.

**Print** (include `<ccc/script/stdio.cch>`): prefer **`io.println`** when a `CCStdio` handle is in scope. Naked `println` / data-first `.println()` remain valid. Templates need an arena — prefer `@scratch` for throwaways.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(&a);
io.println("hi") !>;
io.println(@string(`n=${n}`, @scratch)) !>;
/* also fine: println("hi") !>;  /  @string(`…`, @scratch).println() !>; */
```

---

## 4. Slices remember where bytes live

[recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs) · [recipe_long_lived_store.ccs](../examples/recipe_long_lived_store.ccs) · [Allocator strategy](../spec/draft_alloc_strategy.md) · [Restricted access](../spec/draft_facets.md)

**An arena names a lifetime. Its allocation strategy is an implementation
policy for storage belonging to that lifetime.**

The `CCArena` binding *is* that lifetime (the epoch). Size the root for the
typical live set of that lifetime; choose heap/stack/fixed/overflow as
**policy** for how its storage is obtained — not a separate allocator identity.
Slices are views — `T[:]` carries provenance (stack, arena, static, unique, …)
so the compiler can reject views that outlive their storage. `char[:0]` is the
NUL-terminated refinement — prefer `char[:0] s = "hi";` for string literals.

Ordinary slice sites allow loads and UFCS; field stores (`s.len = …`) are
denied (`@typeview` on the slice family). Tutorial:
[@typehooks / @typeview](typehooks-typeviews.md).

| Arena | Lifetime + storage policy | Typical use |
|-------|---------------------------|-------------|
| `cc_arena_heap(n) @destroy` | Named lifetime; heap root `n`, up to 4 slabs (~1.5×), then **heap overflow** (`malloc`, still arena-owned) | request / window |
| `cc_arena_stack(name, n)` | Same lifetime idea; root on the stack | hot-path / frame scratch |
| `@scratch` | Throwaway compiler stack scratch (not a long-lived named epoch) | `@string` / one-shot print |

```c
/* Heap — owns the root; @destroy frees slabs + overflow. */
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(&a);
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);   /* arena provenance */

/* Stack — buffer lives in the frame; good for short work. */
cc_arena_stack(tmp, 1024);
char* q = tmp.allocT(32);

/* Scratch — only for throwaway templates / print. */
io.println(@string(`len=${a.remaining()}`, @scratch)) !>;

char[:0] hi = "hi";
```

**Auto overflow (policy detail):** when the current slab cannot fit an alloc,
heap/stack arenas grow within `block_max` (default 4), then spill to overflow
`malloc`. Overflow bytes still belong to that same named lifetime — `reset` /
`@destroy` frees them too. A tiny root still allocates; it just spends more
time in overflow. Prefer another arena when lifetimes diverge; treat
`cc_arena_release` / overflow as escape hatches, not the steady path. Fixed
arenas with overflow off return `NULL` on exhaustion (never silent success).

A view must not outlive its storage — no stack/arena borrow into an outliving
task or channel send. Capturing a non-unique arena slice into a nursery **pins**
that arena’s epoch until join (reset/destroy while pinned is a compile error).
Own crossing that boundary: unique (`T[:!]` / `cc_adopt`), static, or write into
the channel (`send_into`). Do not send or capture a slice from `@scratch` or a
stack arena past the frame.

Same `[…]` family: `T[n]` arrays, `T[~n >]` / `T[~n <]` channels.

---

## 5. Closures carry captures

[recipe_explicit_capture.ccs](../examples/recipe_explicit_capture.ccs) · [recipe_fanout_capture.ccs](../examples/recipe_fanout_capture.ccs)

Spawn takes a closure. Captures into a task are copies.

```c
n->spawn([x]() => use(x));     // value
n->spawn([&x]() => { x++; });  // reference
```

Stack slices cannot be captured. Arena slices only while the arena outlives the join.
Tasks do not inherit `@errhandler` — re-bind inside if you use `!>;`.

---

## 6. Absence is not a Result

No `T?`. Pick the shape that matches the operation:

- **Missing** → `T*`, bool+out, or a sentinel (empty slice)
- **Failed** → `T!>(E)` (§2)

---

## 7. `@parallel` joins independent work

[recipe_parallel.ccs](../examples/recipe_parallel.ccs) · Spec §8.11

`@parallel` is a lexical fork-join. It is not a nursery and it does not
create a task the program can hold. `n->spawn` names a lifetime that may
outlive the spawn point; `@parallel` joins at the closing brace.

| Form | Meaning |
|------|---------|
| `@parallel { a = f(); b = g(); }` | Independent assignment arms. First on the caller; the rest may spawn. |
| `@serial { …; a = t; }` | Multi-statement arm. Ordinary C; writes exactly one outer name. |
| `@parallel (pred) { … }` | Same arms. Spawn if `pred`; otherwise run in order. The body always runs. |
| `@parallel for (i in lo..hi) { … }` | Independent iterations over `[lo, hi)`. Bisects; a span of 0 or 1 is a plain `for`. |

```c
int a = 0, b = 0;
@parallel {
    @serial {
        int t = f();
        a = t;
    }
    b = g();
}

@parallel for (y in 0..h) {
    row(y);
}
```

`@serial` is legal only as a direct child of `@parallel { }`. A bare `{ }`
is not an arm. A `for` as a direct child of `@parallel { }` is a compile
error; `for` inside `@serial` is ordinary C.

---

## Next

[Getting Started](getting-started.md) · [Cheatsheet](cheatsheet.md) ·
[recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) ·
[recipe_async_await.ccs](../examples/recipe_async_await.ccs) ·
[recipe_parallel.ccs](../examples/recipe_parallel.ccs)
