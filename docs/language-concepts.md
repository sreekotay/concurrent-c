# Concurrent-C Language Concepts

Spec: [concurrent-c-spec-complete.md](../spec/concurrent-c-spec-complete.md).
Recipes: [examples/README.md](../examples/README.md#learning-path-recommended-order).

---

## 1. Cleanup binds to a place

[recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs) · Spec §5.1 / §4.2.2 / §2.2

`@destroy` is **`@defer` sugar on a declaration**: same scope-exit LIFO ledger,
attached to the binding instead of written as a following statement. Bodyless
`@destroy` runs the type’s registered destroy; a block is an explicit defer body.
With `!>`, cleanup is scheduled only if the unwrap succeeds (no binding → no defer).

| Bind | Meaning |
|------|---------|
| `@defer` | A **statement**: run this when the **scope** exits (LIFO) |
| `@destroy` | Same defer, written on the **declaration** (RAII spelling) |

```c
FILE* f = fopen(path, "r");
@defer fclose(f);

/* sugar for: unwrap, bind n, then @defer the nursery destroy */
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

Named `@defer` can be `@cancel`led; `@defer(ok)` / `@defer(err)` gate on result returns.
Registration is visible in source; discharge sites (soft-return epilogue,
cancelled-resume, never-entered `env_drop`) are defined by the spec emit.

---

## 2. Errors become a value or code

[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs) · [hello.ccs](../examples/hello.ccs)

Fallible work returns `T!>(E)`. Consume every result.

| Operator | Error becomes |
|----------|---------------|
| `?>` | A **value** — `x ?> default` |
| `!>` | **Code** that must leave — `x !> { … }` |

Modifiers on those two: `(e)` binds the error; `!>;` runs the scope's `@errhandler`; `@destroy` attaches cleanup after a successful unwrap.

```c
@errhandler(CCError e) cc_error_exit(e);

int a = read() ?> 30;
int b = read() !>;
int c = read() !>(e) { /* local */ @err(e); };
```

---

## 3. Methods are ordinary functions

[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs) · [recipe_user_generics.ccs](../examples/recipe_user_generics.ccs)

`recv.method(args)` calls the function the receiver's type names.
**Declaring that function is installing the method** — no registry, no trait, no header edit.
Method and free forms are the same API (`v.push(10)` ↔ `CCVec_int_push(&v, 10)`); Concurrent-C examples prefer the method form.

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

Generics: `Name::[args]`. Fallible chain: unwrap (`!>` / `?>`), then the next method sees the value.

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

[recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs) · [recipe_long_lived_store.ccs](../examples/recipe_long_lived_store.ccs) · [Restricted access](../spec/draft_facets.md)

`T[:]` is a view with provenance (stack, arena, static, unique, …).
`char[:0]` is the NUL-terminated (sentinel) refinement — prefer `char[:0] s = "hi";` for string literals.
Arenas bump-allocate. Ordinary slice sites allow loads and UFCS; field stores (`s.len = …`) are denied (`@restricted` on the slice family).

| Arena | Storage | Typical use |
|-------|---------|-------------|
| `cc_arena_heap(n) @destroy` | malloc root; `@destroy` frees | request / long bump |
| `cc_arena_stack(name, n)` | stack slab first (may grow) | hot-path / frame scratch |
| `@scratch` | compiler stack scratch | `@string` / one-shot print |

```c
/* Heap — owns the root; destroy at end of scope. */
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
char* p = a.allocT(64);

/* Stack — buffer lives in the frame; good for short work. */
cc_arena_stack(tmp, 1024);
char* q = tmp.allocT(32);

/* Scratch — only for throwaway templates / println. */
println(@string(`len=${a.remaining()}`, @scratch)) !>;

char[:0] hi = "hi";
```

A view must not outlive its storage — no stack/arena borrow into an outliving task or channel send.
Own crossing that boundary: unique (`T[:!]` / `cc_adopt`), static, or write into the channel (`send_into`).
Do not send or capture a slice from `@scratch` or a stack arena past the frame.

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

## Next

[Getting Started](getting-started.md) · [Cheatsheet](cheatsheet.md) ·
[recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) ·
[recipe_async_await.ccs](../examples/recipe_async_await.ccs)
