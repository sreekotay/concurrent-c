# Concurrent-C Language Concepts

Normative detail: [language spec](../spec/concurrent-c-spec-complete.md).
Learning path: [examples/README.md](../examples/README.md#learning-path-recommended-order).

---

## 1. Defer and ownership

Recipes: [recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs), [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs).

`@defer stmt;` runs `stmt` on scope exit, in reverse declaration order, on every exit path.

```c
FILE* f = fopen(path, "r");
@defer fclose(f);
```

| Form | Meaning |
|------|---------|
| `@defer stmt;` | Always on scope exit |
| `@defer(ok) stmt;` | Only when returning success from a `T!>(E)` function |
| `@defer(err) stmt;` | Only when returning an error from a `T!>(E)` function |
| `@defer name: stmt;` | Named; cancellable with `@cancel name;` |

`@defer(ok)` / `@defer(err)` are only valid in functions that return `T!>(E)`.

`@destroy` attaches cleanup to a declaration. Combined with unwrap:

```c
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCNursery* n2 = @create(NULL) @destroy;   // type-owned create + destroy
```

Rules:

- `@destroy` binds to the declaration; if the initializer fails under `!>`, the variable is not constructed and destroy does not run.
- An optional `@destroy { body }` runs on scope exit (with any registered type destroy).
- Types that register a create hook require `@destroy` or `@detach` after `@create(...)`.
- `@cancel name;` is a compile error if the named defer has already run or been cancelled, or if `name` is out of scope.
- Cleanup resolves locally (enclosing block). No dynamic handler search.

---

## 2. Results, `!>`, `?>`, `@errhandler`

Recipes: [recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs), [recipe_unwrap_destroy_forms.ccs](../examples/recipe_unwrap_destroy_forms.ccs), [hello.ccs](../examples/hello.ccs).

Fallible functions return `T!>(E)`. Every result must be consumed. Two operators:

| Form | Error becomes | Example |
|------|---------------|---------|
| `expr ?> default` | A value | `int t = read() ?> 30;` |
| `expr ?>(e) default_expr` | A value (error bound) | `int t = read() ?>(e) fallback(e);` |
| `expr !>(e) { … }` / `expr !> { … }` | Code (must diverge at expression position) | `int t = read() !>(e) return cc_err(e);` |
| `expr !>;` | The matching `@errhandler` | `int t = read() !>;` |

```c
int !>(CCError) read_timeout(void);

@errhandler(CCError e) cc_error_exit(e);

int t1 = read_timeout() ?> 30;
int t2 = read_timeout() !>(e) return 2;
int t3 = read_timeout() !>;
```

`@errhandler(E e) stmt` (or `{ … }`) is block-scoped policy for bare `!>;` and for `@err(e);`. Selection is by error type `E` (nearest exact match; see `@as` conversion rules in the spec when types differ).

Compose then forward:

```c
int v = read_timeout() !>(e) {
    /* local work */
    @err(e);
};
```

`CALL() !> @destroy { D };` unwraps, then attaches declaration cleanup. On error, `D` does not run because the binding never completes.

Construct results with `cc_ok(value)` / `cc_err(...)`. Inspect with `cc_is_ok` / `cc_unwrap_ok` / `cc_unwrap_err` when not using `!>` / `?>`.

---

## 3. UFCS

Recipes: [recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs), [recipe_user_generics.ccs](../examples/recipe_user_generics.ccs).

One rule: `recv.method(args)` / `recv->method(args)` calls the function the receiver's type names. Declaring that function installs the method.

```c
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
n->spawn(() => printf("hi\n"));   // cc_nursery_spawn(n, …)
```

Spellings of the same call:

| Spelling | Resolves to |
|----------|-------------|
| Family / instance member | `CCVec_int_push(&v, x)` from `v.push(x)` |
| Scalar family | `cc_double_halve(d)` from `d.halve()` |
| Bare-name | Any function whose first parameter accepts the receiver |
| Member factory | `recv.member::[T](args)` ≡ `snake(Recv)_member::[T](&recv, args)` |
| Free-name factory | `name::[T](args)` |

Generics use `Name::[args]`:

```c
CCVec::[int] numbers = cc_vec_new::[int](&arena);
numbers.push(10);
```

Fallible hops unwrap, then dispatch on the value:

```c
int ok = get(21)!>.twice();
int recovered = (get(-1) ?> 0).twice();
```

UFCS works on values, pointers, and results after unwrap. Unknown methods fail at compile time with the receiver type and installed set.

---

## 4. Slices and arenas

Recipes: [recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs), [recipe_long_lived_store.ccs](../examples/recipe_long_lived_store.ccs).

Container type constructors share `[…]`:

| Type | Meaning |
|------|---------|
| `T[n]` | Fixed array of `n` elements |
| `T[:]` | Slice — length + pointer + provenance |
| `T[:!]` | Unique (owned) slice |
| `T[~n >]` / `T[~n <]` | Channel send / receive handle |

A slice remembers provenance: arena, stack, static, unique, or untracked. The compiler uses that for escape and send checks.

**Arenas** are bump allocators with lexical ownership:

```c
{
    CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
    void* buf = cc_arena_alloc(&a, 1024, 8);
    char[:] s = /* arena-backed view */;
}
```

Rules that matter day to day:

- Arena-backed and stack views are borrows; do not free them with `free` / `cc_slice_destroy`.
- `cc_arena_reset` / restore is an error while a derived borrow of that arena is still in scope.
- Capturing a non-unique arena slice into a task pins the arena epoch until the join scope ends; epoch-ending ops are then compile errors.
- Channel send of a non-unique view is ill-formed; send unique / static, or use reserve-then-write (`send_into`).
- Foreign buffers: `cc_slice_from_buffer` is untracked (no destructor). `cc_adopt(ptr, len, free_fn)` yields a unique slice with a trusted deleter — the deleter must match the allocator.

```c
CCSliceUnique s = cc_adopt(malloc(64), 64, free) @destroy;
```

Typed element slices (`double[:]`, …) auto-instantiate element-wise methods (`len`, `at`, `sub`, …).

---

## 5. Closures

Recipes: [recipe_explicit_capture.ccs](../examples/recipe_explicit_capture.ccs), [recipe_fanout_capture.ccs](../examples/recipe_fanout_capture.ccs).

```c
n->spawn(() => do_work());
n->spawn([x]() => use(x));      // capture by value (default)
n->spawn([&x]() => { x++; });   // reference capture
n->spawn([=x]() => use(x));     // explicit copy
```

Spawn takes a closure, not the result of calling one.

Capture into a task/thread copies values into the closure. Eligibility:

| Value | Capturable into outliving task? |
|-------|----------------------------------|
| Primitives, capturable structs | Yes |
| Static / unique slices | Yes |
| Stack slices | No |
| Arena slices | Only if the arena outlives the join (else pin / error) |
| Channels | Yes |
| Scope-bound handles | No |

Value-capturing `T* p = &local` and then writing through `*p` in the task is a compile error (same class as mutating a reference capture).

Task bodies do not share the caller's `@errhandler` frame; re-bind inside the closure when using `!>;`.

---

## 6. Maybe-present values

Lookups in [recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs); fallible ops in [recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs).

There is no `T?` / `Optional<T>`. Choose the shape that matches the operation:

| Shape | Use when |
|-------|----------|
| Nullable pointer (`T*`) | Lookups, optional fields — `int* hit = m.get(key);` |
| Bool + out-parameter | Pop / next — `if (v.pop(&out)) …` |
| In-band sentinel | Streams — empty slice for EOF |
| `T!>(E)` | Real failure with an error channel |

---

## Next

- [Getting Started](getting-started.md) — install, nurseries, channels, async
- [Cheatsheet](cheatsheet.md) — short patterns
- Concurrency recipes: [recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs), [recipe_async_await.ccs](../examples/recipe_async_await.ccs), [recipe_worker_pool.ccs](../examples/recipe_worker_pool.ccs)
