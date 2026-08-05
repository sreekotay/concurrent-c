# Concurrent-C Language Concepts

Spec: [concurrent-c-spec-complete.md](../spec/concurrent-c-spec-complete.md).
Recipes: [examples/README.md](../examples/README.md#learning-path-recommended-order).

---

## 1. Cleanup binds to a place

[recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs) · [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs)

| Bind | Meaning |
|------|---------|
| `@defer` | A **statement**: run this when the **scope** exits (LIFO) |
| `@destroy` | A **declaration**: clean up when **this binding** ends |

```c
FILE* f = fopen(path, "r");
@defer fclose(f);

CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

If the unwrap fails, the binding never exists — destroy does not run.
Named `@defer` can be `@cancel`led; `@defer(ok)` / `@defer(err)` gate on result returns.

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

`recv.method(args)` is the function the receiver's type names.
Declare the function → the method exists. Prefix and postfix are one call.

```c
v.push(10);           // CCVec_int_push(&v, 10)
n->spawn(() => …);    // cc_nursery_spawn(n, …)
```

Generics: `Name::[args]`. Fallible chain: unwrap (`!>` / `?>`), then the next method sees the value.

---

## 4. Slices remember where bytes live

[recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs) · [recipe_long_lived_store.ccs](../examples/recipe_long_lived_store.ccs)

`T[:]` is a view with provenance (stack, arena, static, unique, …).
Arenas bump-allocate; one `@destroy` frees the bump.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
```

A view must not outlive its storage — no stack/arena borrow into an outliving task or channel send.
Own crossing that boundary: unique (`T[:!]` / `cc_adopt`), static, or write into the channel (`send_into`).

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
