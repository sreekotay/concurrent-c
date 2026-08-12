# Concurrent-C Cheatsheet

Quick reference. Tutorial: [getting-started.md](getting-started.md) ·
concepts: [language-concepts.md](language-concepts.md) · recipes:
[examples/README.md](../examples/README.md#learning-path-recommended-order) ·
spec: [spec/](../spec/).

---

## Build & Run

```bash
ccc run file.ccs                    # build + run
ccc build run file.ccs              # same, explicit
ccc build run file.ccs -- --arg     # args to the binary
ccc path/to/tool.shcc [args…]       # .shcc → implicit run (shebang-friendly)
ccc --emit-c-only file.ccs          # emit C only → out/file.c
ccc build -O file.ccs               # release (-O2 -DNDEBUG)
ccc build -g file.ccs               # debug (-O0 -g); default is -O2, asserts kept
ccc version=0.3.2 run file.ccs      # pin lowerer (prefix of ccc --version)
ccc --as=ccs file                   # kind when there is no suffix / header
```

First line of a unit: `#!ccc ccs [version=…]` (headers `cch`; scripts use an
OS shebang). Suffix is the fallback. Details:
[backwards compatibility](backwards_compatibility.md).

Outputs: `./out` (generated C) and `./bin` (binaries), relative to cwd.

---

## Cleanup: `@defer` / `@destroy` / registration

`@destroy` is **`@defer` sugar on a declaration** — same LIFO ledger. With `!>`,
cleanup schedules only if unwrap succeeds.

| Form | Meaning |
|------|---------|
| `@defer stmt;` | Always run on scope exit (LIFO) |
| `@defer(ok) stmt;` | Only on success exit (`return cc_ok(…)` / normal return) |
| `@defer(err) stmt;` | Only on error exit (`return cc_err(…)`) |
| `T x = … @destroy { … };` | Explicit defer body on the binding (no registry needed) |
| `T x = … @destroy;` | Bodyless → type’s **registered** destroy / pre-destroy |

Bodyless `@destroy` with no registered hook is a **compile error**. Stdlib types
ship hooks (`CCNursery*`, `CCArena`, channels, …). Register your own:

```c
@typehooks on MyRes {
    .destroy = cc_type_destroy_call("my_res_close"),
};
MyRes r = my_res_open() !> @destroy;
```

Order when both exist: registered pre-destroy → `@destroy { body }` → registered
destroy. Nursery = wait → body → free.

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

## Results (`T!>(E)`)

Two operators; everything else is a modifier:

| | Error becomes |
|--|--|
| `?>` | a **value** — `x ?> default` / `x ?>(e) …` |
| `!>` | **code** that must leave — `x !> { … }` / `x !>;` |

```c
@errhandler(CCError e) cc_error_exit(e);   // policy for bare !>;

int a = read() ?> 30;
int b = read() !>;                         // → @errhandler
int c = read() !>(e) { /* local */ @err(e); };
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

Tasks do not inherit `@errhandler` — re-bind inside each spawn body.

---

## UFCS

One rule: `recv.method(args)` calls the function the **receiver’s type** names.
**Usual path — no registration:** declaring the function installs the method
(contrast bodyless `@destroy`, which needs a registered destroy hook). Prefer
UFCS over the free-function spelling of the same API.

```c
n->spawn(() => { … });      // cc_nursery_spawn(n, …)
tx.send(i) !>;
io.println("hi") !>;
v.push(10);                 // CCVec_int_push(&v, 10)
u.mean(6.0);                // mean(u, 6.0) — bare-name tier
get(21)!>.twice();          // unwrap, then method on the value

static double CCVec_double_median(CCVec_double* v) { … }
v.median();                 // declare = install
```

**Optional registration** (stdlib / your families):
`@typehooks on T { .ufcs = …, }` (strict C designated-init body). Legacy
`cc_type_register` / `cc_type_define`: [deprecated.md](deprecated.md).

```c
@typehooks on MyHandle* {
    .ufcs = my_handle_ufcs_lower_c,
};
```

Receiver first; arena last when needed. Recipe:
[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs).
See `spec/draft_typehooks.md`.

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

## Arenas name a lifetime

**An arena is a lifetime annotation, not an allocator strategy.** Heap vs
stack root, slabs, and overflow are how storage for that lifetime is
obtained — always named explicitly (`CCArena a = …`); there is no ambient
or hidden arena.

Size the root for the typical live set of that lifetime. Default heap/stack
policy: bump in root → up to 4 slabs (~1.5×) → **heap overflow** (`malloc`,
still arena-owned; freed on reset / `@destroy`).

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;  // names the lifetime
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);   // arena provenance

cc_arena_stack(tmp, 1024);             // same policy; stack root
a.reset();                             // drain epoch; reuse root

/* @scratch — throwaway @string / print only; do not capture or send */
io.println(@string(`len=${s.len}`, @scratch)) !>;
```

Slices (`T[:]`) carry provenance. Views must not outlive their arena.
Details: [getting-started § Arenas](getting-started.md#arenas-name-a-lifetime).

---

## Absence (no `T?`)

| Shape | Use when |
|-------|----------|
| `T*` / bool+out | missing lookup / pop |
| empty slice | EOF / no bytes |
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
| Worker pool | [recipe_worker_pool.ccs](../examples/recipe_worker_pool.ccs) |
| Fan-out / captures | [recipe_fanout_capture.ccs](../examples/recipe_fanout_capture.ccs) |
| Ordered parallel | [recipe_ordered_parallel.ccs](../examples/recipe_ordered_parallel.ccs) |
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

Bridges: npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python),
pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/).

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
```
