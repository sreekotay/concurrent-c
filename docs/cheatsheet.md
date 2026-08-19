# Concurrent-C Cheatsheet

Quick reference. Tutorial: [getting-started.md](getting-started.md) ·
concepts: [language-concepts.md](language-concepts.md) · recipes:
[examples/README.md](../examples/README.md#learning-path-recommended-order) ·
spec: [spec/](../spec/).

---

## Build & Run

Every unit starts with a **line-1 header** (kind + optional lowerer pin). Emit
strips it; it is not program text. Details:
[backwards compatibility](backwards_compatibility.md).

```text
#!ccc ccs                         # source (.ccs)
#!ccc cch                         # header (.cch)
#!ccc ccs version=0.3             # pin lowerer (usual: MAJOR.MINOR)
#!/usr/bin/env -S ./cc/bin/ccc version=0.3     # script (.shcc); OS shebang
```

`version=MAJOR.MINOR` keeps that file on the 0.3 line when the toolchain
moves. PATCH and SEED (`0.3.3`, `0.3.3-156`) tighten the match. Omit the pin
only when you intentionally want whatever `ccc` is running. Suffix (`.ccs` /
`.cch` / `.shcc`) is the fallback when the header is absent — prefer the
header so kind and pin travel with the file.

```bash
ccc run file.ccs                    # build + run
ccc build run file.ccs              # same, explicit
ccc build run file.ccs -- --arg     # args to the binary
ccc path/to/tool.shcc [args…]       # .shcc → implicit run (shebang-friendly)
ccc --emit-c-only file.ccs          # emit C only → out/file.c
ccc build -O file.ccs               # release (-O2 -DNDEBUG)
ccc build -g file.ccs               # debug (-O0 -g); default is -O2, asserts kept
ccc version=0.3 run file.ccs        # pin lowerer from CLI (same form as header)
ccc --as=ccs file                   # kind when there is no suffix / header
```

Outputs: `./out` (generated C) and `./bin` (binaries), relative to cwd.

---

## Results — declare & consume (`T!>(E)`)

Fallible work returns `T!>(E)`. Produce with `cc_ok` / `cc_err`; consume every
result with `?>` or `!>`. Recipe:
[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs).

### Declare / return

```c
int !>(CCError) read_config(const char* key) {
    if (!key || !key[0]) return cc_err(CC_ERR_INVALID_ARG, "empty key");
    if (strcmp(key, "timeout") == 0) return cc_ok(30);
    return cc_err(CC_ERR_NOT_FOUND, "key not found");
}
```

Inside a function whose return type is `T!>(E)`, `cc_ok(v)` / `cc_err(…)` infer
`T` and `E`. Forms:

| Form | Meaning |
|------|---------|
| `return cc_ok(v);` | success payload `v` |
| `return cc_ok();` | `void!>(E)` success |
| `return cc_err(e);` | propagate / wrap error `e` |
| `return cc_err(CC_ERR_*, "msg");` | build `CCError` when `E` is `CCError` |

Explicit when not in return context: `cc_ok(T, v)`, `cc_ok(T, E, v)`,
`cc_err(T, e)`, `cc_err(T, E, e)`.

### Consume

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

int a = read_config("timeout") ?> 30;
int b = read_config("timeout") !>;                    // routes E
int c = read_config("timeout") !>(e) { /* local */ @err(e); };
int d = read_config("timeout") !>(e) return cc_err(e); // propagate
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

Tasks do not inherit `@errhandler` — re-bind inside each spawn body.

### C lowered API

`T!>(E)` is sugar over a tagged union. The lowerer emits a concrete type and
typed constructors; plain C (and `@emit` / generators) use those, not `!>` / `?>`.

```c
/* Sugar                         Lowered */
int !>(CCError)                  CCResult_int_CCError
void !>(CCError)                 CCResult_void_CCError

return cc_ok(30);                return cc_ok_CCResult_int_CCError(30);
return cc_err(e);                return cc_err_CCResult_int_CCError(e);
```

Layout (from `CC_DECL_RESULT_SPEC`):

```c
typedef struct CCResult_int_CCError {
    bool ok;
    union { int value; CCError error; } u;
} CCResult_int_CCError;
```

**Prefer accessors** (work in `.ccs`, plain C, macros, generated code):

| API | Meaning |
|-----|---------|
| `cc_is_ok(r)` / `cc_is_err(r)` | tag |
| `cc_value(r)` / `cc_error(r)` | active arm (only after a check) |
| `r.is_ok()` / `r.value()` | same via UFCS (`.value()` aborts if err) |

```c
CCResult_int_CCError r = read_config("timeout");  /* or: __typeof__(read_config("")) r = … */
if (cc_is_ok(r)) use(cc_value(r));
else handle(cc_error(r));
/* peel after a check: r.ok / r.u.value / r.u.error — C interop detail */
```

In headers that name a Result for C callers, declare once with a guard:

```c
#ifndef CCResult_MyData_MyError_DEFINED
#define CCResult_MyData_MyError_DEFINED 1
CC_DECL_RESULT_SPEC(CCResult_MyData_MyError, MyData, MyError)
#endif
```

Do not hand-spell `CCResult_*` / `cc_ok_CCResult_*` in ordinary `.ccs` /
`.cch` bodies — write `T!>(E)` and `cc_ok` / `cc_err`. Reach for the mangled
names and accessors when operators cannot run (plain C, `@emit`, generators).

---

## Cleanup: `@defer` / `@destroy` / registration

`@destroy` attaches cleanup to **successful declaration construction** — `@defer`
sugar on the binding. After `!>`, that means the unwrap succeeded.

| Form | Meaning |
|------|---------|
| `@defer stmt;` | Always run on scope exit (LIFO) |
| `@defer(ok) stmt;` | Only on success exit (`return cc_ok(…)` / normal return) |
| `@defer(err) stmt;` | Only on error exit (`return cc_err(…)`) |
| `T x = … @destroy { … };` | Explicit defer body on the binding, then the type’s destroy chain |
| `T x = … @destroy;` | Bodyless → the type’s destroy chain |
| `x.destroy()` | UFCS: `Type_destroy` when that function exists |

The chain: registered pre-destroy → `@destroy { body }` → registered destroy →
each **value** field whose type has a hook, last-declared to first
(transitively). Pointer, array, and function-pointer fields are omitted.
Bodyless `@destroy` with an empty chain is a **compile error**. Stdlib types
ship hooks (`CCNursery*`, `CCArena`, channels, …). Register your own:

```c
@typehooks on MyRes {
    .destroy = cc_type_destroy_call("my_res_close"),
};
MyRes r = my_res_open() !> @destroy;
```

Nursery = wait → body → free. Nested struct fields with their own hooks run
after the outer hook.

```c
FILE* f = fopen(path, "w");
if (!f) return cc_err(CC_ERR_IO, "fopen failed");
@defer fclose(f);                 // always
@defer(ok)  commit(path);         // success path only
@defer(err) rollback(path);       // error return only

CCNursery* n = cc_nursery_create(NULL) !> @destroy;
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
```

Recipe: [recipe_defer_cleanup.ccs](../examples/recipe_defer_cleanup.ccs).

---

## UFCS

One rule: `recv.method(args)` calls the function the **receiver’s type** names.
**Usual path — no registration:** declaring the function installs the method
(contrast bodyless `@destroy`, which needs a registered destroy chain). Prefer
UFCS over the free-function spelling of the same API. `.destroy()` is that
UFCS path (`Type_destroy`).

```c
n->spawn(() => { … });      // cc_nursery_spawn(n, …)
tx.send(i) !>;
io.println("hi") !>;
v.push(10);                 // CCVec_int_push(&v, 10)
u.mean(6.0);                // mean(u, 6.0) — bare-name tier
get(21)!>.twice();          // unwrap, then method on the value

static double CCVec_double_median(CCVec_double* v) { … }
v.median();                 // declare = install

size_t Box_len(const Box *b);   /* header prototype — other TUs get b.len() */
size_t !>(CCError) Box_read_at(const Box *b, char *d, size_t n);
size_t Box_len(const Box *b) { … }  /* linked definition; not a static wrapper */
```

**Optional registration** (stdlib / your families):
`@typehooks on T { .ufcs = …, }` (strict C designated-init body). Legacy
`cc_type_register` / `cc_type_define`: [deprecated.md](deprecated.md).

```c
@typehooks on MyHandle* {
    .ufcs = my_handle_ufcs_lower_c,
};
```

Last-resort unresolved methods: `.ufcs_sink = cc_type_dynamic_call("callee", "WRAP")`.
Destination-aware: a typed dest composes `<callee>_<mangled dest>` when declared.

Receiver first; **arena last** when the call needs one. That is the
convention and what makes UFCS land on the data, not the arena:
`s.clone_into(&a)` is `clone_into(s, &a)`. Arena first would be
`a.clone_into(s)`. Recipe:
[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs).
Tutorial: [typehooks-typeviews.md](typehooks-typeviews.md). Spec: `spec/draft_typehooks.md`.

---

## Generics (`Name::[args]`)

`Name::[args]` instantiates a library factory (`CC_GENERIC_FACTORY`). The
emitted `${mangled}_<member>` functions are the methods. `Vec`, `Map`,
`ArrayMap`, and non-char `T[:]` are that same rule — not a separate compiler
container path.

```c
Vec::[int] v@(&arena) @destroy;          // struct CCVec_int
v.push(10);                              // dot: Vec is the struct
vec_new::[int](&arena);                  // same instance

Map::[int, double] m = map_new::[int, double](&arena);
Map::[size_t, int] n = map_new::[size_t, int](&arena);
m->insert(1, 2.5);                       // arrow: Map sugar is Name*

double[:] xs;                            // CCSlice_double (char[:] stays CCSlice)
SmallVec::[int, 8] sv;                   // integer args are factory string slices
SmallVec::[long long, 8] w;              // multi-word types stay one arg
```

`.` = value, `->` = pointer. Recipe:
[recipe_user_generics.ccs](../examples/recipe_user_generics.ccs). Spec
[§12.1](../spec/concurrent-c-spec-complete.md#121-registered-factories).

---

## Print

Prefer `io.println` when a `CCStdio` handle is in scope (`<ccc/script/stdio.cch>`):

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
CCStdio io = cc_stdio_create(&a);
io.println("hi") !>;
io.println(@string(`n=${n}`, @scratch)) !>;
/* also fine: println("hi") !>;  /  msg.println() !>; */
```

---

## Template strings (`@string`)

Backtick templates interpolate into a `CCString` or a stack `char[:]` borrow.
There is no printf `format` entry point. Spec:
[§9.1](../spec/concurrent-c-spec-complete.md#91-strings).

| Form | Yields |
|------|--------|
| `@string(expr, a)` | `CCString` from a literal or `expr.to_str(a)` |
| `` @string(`…`, a) `` | templated `CCString` in arena `a` |
| `` @string(policy, `…`, a) `` | same; `${…}` / `$~tag{…}` slots go through `policy` |
| `` @string(`…`, @scratch) `` / `@scratch(N)` | same, shared function scratch (default 1 KiB) |
| `` @string(`…`) `` | `char[:]` stack borrow — ints / `bool` / `char` only |

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
int n = 42;
CCString msg = @string(`n=${n}; price=$100`, &a);      // owned
char[:] hdr = @string(`:${n}\r\n`);                    // block-scoped borrow
println(@string(`len=${msg.len()}`, @scratch)) !>;     // throwaway
```

| In the template | Meaning |
|-----------------|--------|
| `${e}` | interpolate (`to_str` when `e` is not already text) |
| `$~tag{e}` | tagged slot (arena + policy); policy sees `"tag"` |
| other `$` | literal (`$100` needs no escape) |
| `\${` / `\$~` | emit `${` / `$~` |
| `${{…}}` | verbatim bytes — no escapes, no slots; inner backticks do not close the literal |

Multiline backticks **dedent** to the closing backtick's margin.

`@scratch` is only the arena operand of `@string`. Every site in a
function or closure shares one stack arena (not per line). Bind the
product (`CCString line =` `` @string(`…`, @scratch) ``) before a
consuming call. `return f(@string(…))` breaks `@destroy` return-rewrite;
a call-local `@string` is reclaimed after that call. To keep a product,
pass the arena it should live on (see [Keep](#keep-pass-the-arena-to-live-on)).
A newline before `@scratch` is fine. Do not `scratch.destroy()`. Do not
capture or send a `@scratch` product. Arena-less `` @string(`…`) `` is a
compile error on slices, `CCString`, floats, or pointers (pass an arena).
Growth failure poisons the `CCString`; it never truncates.

---

## Structured concurrency

```c
@errhandler(CCError e) cc_error_exit(e);
{
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => do_work());
    n->spawn(() => do_other_work());
}
/* both tasks finished — nursery @destroy waited */
```

Nested: `cc_nursery_create(outer)` parents the inner nursery under `outer`.
Independent value joins use `@parallel` (next), not a nursery.

To drop the handle without joining, register optional after-work then abandon
(pointer is dead; last child frees the nursery):

```c
n->on_last(q, finish_q);   // optional; not run by wait / @destroy
n->abandon();              // not cancel; in-flight work runs to completion
```

Use either `@destroy` / `wait` or `on_last` + `abandon`, not both. Spec §8.1.5.

---

## `@parallel`

Lexical fork-join — not a nursery, no task handle. Spec §8.11.
Recipe: [recipe_parallel.ccs](../examples/recipe_parallel.ccs).

| Form | Meaning |
|------|---------|
| `@parallel { a = f(); b = g(); }` | Independent assignment arms. First on the caller; the rest may spawn. |
| `@serial { …; a = t; }` | Multi-statement arm. Ordinary C; writes exactly one outer name. |
| `@parallel (pred) { … }` | Same arms. Spawn if `pred`; otherwise run in order. Body always runs. |
| `@parallel for (i in lo..hi) { … }` | Independent iterations over `[lo, hi)`. Bisects; span 0 or 1 is a plain `for`. |
| `@parallel wait (ts) for (i in lo..hi)` | Ordered spawn loop on a turnstile (§8.11.6). |
| `cache (zs)` | After `wait`: adopt enclosing scratch; instance identity unobservable. |
| `@stage (ts.read, i) { … }` | Ticket handshake in a wait-for body; pass on every exit. |
| `break` / `continue` / `return` | Same as `for`; parallel path drains first. Stage work is the contract; counters are best-effort under early exit. `goto` cannot leave the body. |

```c
int a = 0, b = 0;
@parallel {                    // always try to spawn
    a = f();
    b = g();
}

@parallel (d < k) {            // spawn if pred; else run in order
    @serial {
        int t = f();
        a = t;                 // exactly one outer name
    }
    b = g();
}

@parallel for (i in 0..n) {    // half-open; bisects
    work(i);
}
```

`@serial` is only a direct child of `@parallel { }`. Bare `{ }` is not an
arm. `for` as a direct child of `@parallel { }` is an error; `for` inside
`@serial` is ordinary C. `n->spawn` still names a task lifetime.

---

## Channels

```c
@errhandler(CCError e) cc_error_exit(e);

int[~10 >] tx;
int[~10 <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy;

{
    CCNursery* outer = cc_nursery_create(NULL) !> @destroy;

    outer->spawn(() => [rx] {
        @errhandler(CCError e) cc_error_exit(e);
        int v;
        while (cc_io_avail(rx.recv(&v)))
            printf("got %d\n", v);
    });

    {
        CCNursery* inner = cc_nursery_create(outer) !> @destroy;
        (void)inner->close_on(tx);          // close tx when inner joins
        inner->spawn(() => [tx] {
            @errhandler(CCError e) cc_error_exit(e);
            for (int i = 0; i < 5; i++)
                tx.send(i) !>;
        });
    }
}
```

Consumer outside, producer + `close_on` inside. Full recipe:
[recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs).

---

## Closures / captures

Spawn takes a closure. Captures into a task are copies (value) unless `&`.

```c
n->spawn(() => { … });                 // no capture list
n->spawn(() => [x] { use(x); });       // value
n->spawn(() => [&x] { use(x); });      // reference (still no shared mutation)
```

Re-bind `@errhandler` inside the task. Do not capture stack / `@scratch` slices
past the frame; arena slices pin the arena until join.

---

## Named exclusive (`CCExclusive`)

Short critical sections on a `uint64_t` name. Do not `@await` under the hold.

```c
CCExclusive* excl = cc_exclusive_create(&arena, 0);
CCExclusiveGuard g = excl->acquire(name);
… mutate …
g.release();

/* Park until pred is true *under* the name. Not a condvar-on-held-guard. */
CCExclusiveGuard w = excl->acquire_when(name, pred, env) !> @destroy;
/* held; pred was observed true while held */
```

The holder that makes `pred` true signals **while still holding**: `h.signal()`
/ `h.broadcast()`. The wait parks a fiber or an OS thread. An expired
`cc_current_deadline()` (`@with_deadline` / `cc_deadline_push`) is
`CC_ERR_TIMEOUT`. A cancelled deadline, or a cancelled nursery when one
exists, is `CC_ERR_CANCELLED`. No nursery is not cancel. Errors are
`CCError` — never a zeroed guard. `acquire_when_into` runs a builder once
under that invariant and releases.
Recipe: [recipe_exclusive_named.ccs](../examples/recipe_exclusive_named.ccs).

---

## Pipeline turnstile (`CCTurnstile`)

Depth cap plus ordered stages. Declare the turnstile **before** the nursery.

```c
CCTurnstileRW ts@(n, cap, &arena) @destroy;
ts.enter(i) !>;
ts.read.wait(i);   …  ts.read.pass(i);
ts.write.wait(i);  …  ts.write.pass(i);
ts.leave() !>;

CCTurnstile t@(n, cap, n_stages, &arena) @destroy;
t.enter(i) !>;
t.stage(k)->wait(i);  t.wait(k, i);
```

`enter(i)` takes a token and arms every stage's `i+1`. A closed depth channel is an error, not `Ok(false)`.
Recipe: [recipe_turnstile.ccs](../examples/recipe_turnstile.ccs).

---

## Arenas name a lifetime

**An arena is a named lifetime, not an allocator strategy.** Storage is
three tiers (cache-shaped): **L1** root slab, **L2** grown extents, **Main**
overflow. Constructors pick how those tiers are obtained — always named
explicitly (`CCArena a = …`); there is no ambient or hidden arena.

Size L1 for the typical live set. Default heap/stack: bump in L1 → up to 4
slabs (L2, ~1.5×) → **Main** (`malloc`, still arena-owned; freed on reset /
`@destroy`). `a.live()` counts all three tiers.

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;  // names the lifetime
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);   // arena provenance

cc_arena_stack(tmp, 1024);             // same policy; L1 on the stack; @destroy at scope exit
cc_arena_buf(win, frame, sizeof frame); // same sugar; caller L1 (no VLA)
a.reset();                             // drain epoch; reuse L1

CCArenaCheckpoint cp = a.try_checkpoint() !> @destroy; // consumed loan
/* …scratch, including Main… */
cp.try_restore() !>;                   // or leave the scope: @destroy restores
```

Slices (`T[:]`) carry provenance. Views must not outlive their arena.
A mid-slab hole disables a new capture until last-live rewind or `reset`.
Restore of a handle whose overflow keep-set was released refuses.
`detach()` moves heap-owned mallocs only — a stack or caller L1 is refused.
Details: [getting-started § Arenas](getting-started.md#arenas-name-a-lifetime).

---

## Keep: pass the arena to live on

`@scratch` is throwaway — gone after the consuming call. To **keep** a
product, pass the arena it should live on. That arena is the **last**
parameter (convention, and so UFCS binds the data: `s.clone_into(&a)`
is `clone_into(s, &a)`).

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;

io.println(@string(`tmp`, @scratch)) !>;     // gone after println

CCString keep = @string(`keep me`, &a);      // lives on `a`
CCString out  = cc_script_sh_read_at(keep, &a) !>;

CCString load(CCSlice path, CCArena *a);     // arena last → keep on `a`
s.clone_into(&a);
```

Recipe: [recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs).

---

## Locality (program shape)

Memory is owned or it is a view. Lifetime is a field. A constructor assumes
dead — reopen is `d.destroy(); d.from_view(src) !>;`, not `memset`. Fallible
work is `T!>(E)`; a giving-up path is **unchanged** or **`broken`**, not a
zero that looks like success. `char[:]` is `{ptr, len, id}` (storing it does
not take the bytes; `id == 0` is untracked). Faces (`@typeview`) are decided
at the use site.

[Getting started](getting-started.md#locality-owned-or-view) ·
[recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) ·
[typehooks-typeviews.md](typehooks-typeviews.md).

---

## Absence (no `T?`)

| Shape | Use when |
|-------|----------|
| `T*` / bool+out | missing lookup / pop |
| empty slice | no bytes, or “not one piece” — not automatically EOF |
| `T!>(E)` | operation failed |

---

## Timeouts & cancellation

```c
@with_deadline(millis(50)) {
    while (!cc_deadline_expired(cc_current_deadline())) {
        do_work();
        cc_sleep_ms(10);                 // cancellation-aware
    }
}

@with_deadline(millis(50)) as dl {       // bind handle
    while (!cc_deadline_expired(dl)) { … }
}

if (cc_is_cancelled()) return;
```

---

## Async / await

Prefer `n->spawn` for sibling work. Prefer `@async` / `@await` for one
suspendable call stack. Recipe: [recipe_async_await.ccs](../examples/recipe_async_await.ccs).

```c
@async int bump(int value) { return value + 1; }

int main(void) {
    @errhandler(CCError e) { cc_error_log(e); return 1; }
    int result = @await bump(41);
    return result == 42 ? 0 : 1;
}
```

---

## `.shcc` scripts

Same language as `.ccs`; script prelude + synthetic `main` when you omit
`main`. Ambient `a` / `io` / `in` / `args` when those names appear.
`ccc tool.shcc` is an implicit run. See
[getting-started § `.shcc`](getting-started.md#shcc-scripts) and
[spec §9.5](../spec/concurrent-c-spec-complete.md#95-script-library-shcc--cccscript).

```bash
ccc examples/py/pydemo.shcc         # CC hosts Python
ccc examples/js/jsdemo.shcc         # CC→JS (guest; Node owns env)
./tools/perf.shcc @                 # list @task entries
```

---

## Common patterns (pointers)

| Pattern | Recipe |
|---------|--------|
| Owned or view / reopen | [recipe_owned_view.ccs](../examples/recipe_owned_view.ccs) |
| Worker pool | [recipe_worker_pool.ccs](../examples/recipe_worker_pool.ccs) |
| Fan-out / captures | [recipe_fanout_capture.ccs](../examples/recipe_fanout_capture.ccs) |
| Ordered parallel | [recipe_ordered_parallel.ccs](../examples/recipe_ordered_parallel.ccs) |
| `@parallel` / `@serial` | [recipe_parallel.ccs](../examples/recipe_parallel.ccs) |
| Channel pipeline | [recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs) |

---

## Build system (`build.cc`)

```c
CC_TARGET main exe main.ccs utils.ccs
CC_TARGET_LIBS main pthread
CC_DEFAULT main
```

```bash
ccc build                           # default target
ccc build run                       # build + run default
ccc build list
ccc build --build-file path/build.cc
```

---

## Python & JS

```c
#include <ccc/script/py.cch>
#include <ccc/script/js.cch>
```

**Embed Python** (CC owns `main`): [recipe_py_interop.ccs](../examples/recipe_py_interop.ccs).

**One file → native modules** (no `main` + export): [js-py-modules.md](js-py-modules.md).

```c
@comptime cc_py_export("counter", "Counter", &seed);   // → counter.abi3.so
@comptime cc_js_export("counter", "Counter", &seed);   // → counter.node
```

```bash
ccc build counter.ccs
PYTHONPATH=bin python3 -c "import counter; counter.bump(4)"
```

Bridges: npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python)
(Python from Node — default call blocks; `py.task` for a Promise; `{ isolated: true }`
is a child interpreter, not a different calling convention),
pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/)
(JS from Python — always a `node` child; thenables awaited in the child;
Jupyter/Colab: `from cc_node import require`).

---

## Environment

| Variable | Purpose |
|----------|---------|
| `CC` | Host C compiler |
| `CC_OUT_DIR` | Generated C + objects (default `out/`) |
| `CC_BIN_DIR` | Linked executables (default `bin/`) |
| `CC_HOME` | Override install tree resolution |
| `CC_NO_CACHE` | Disable incremental cache |
| `CC_CACHE_MAX_MB` | Cache cap (0 = uncapped) |
| `CC_CACHE_EVICT_INTERVAL` | Min seconds between sweeps (default 60) |

---

## Includes

```c
#include <ccc/cc_runtime.cch>      // nurseries, channels, core
#include <ccc/std/prelude.cch>     // kilobytes, common std
#include <ccc/script/stdio.cch>    // CCStdio / io.println
#include <ccc/script/prelude.cch>  // forced in for .shcc; usable from .ccs too
#include <ccc/cc_atomic.cch>       // portable atomics
#include "leaf.cch"                // local face; nested .cch is fine
```

A local `.cch` with statement unwrap (`!>(e) {`) is spliced into the
including unit. `T !>(E)` on a declaration does not force that. A
`@typehooks` / `@typeview` face still extracts to a lowered `.h`; callers
keep `char[:]` argument wrap and the proto's Result error type from the
original `.cch`. A quoted interface `.cch` from a `.ccs` still extracts;
the `#include` stays in source order so types declared above it are in
scope.
