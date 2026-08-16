# Getting Started with Concurrent-C

Concurrent-C is a **strict C11-superset**: you write `.ccs` (or `.cch`
headers, or `.shcc` scripts — [below](#shcc-scripts)), the `ccc` driver lowers
it to plain C (with `#line` sourcemaps), and your **host C compiler** builds the
binary. Structured concurrency, results, UFCS, slices/arenas (an arena names a
lifetime), and a header-first runtime ship with the language.

You do **not** need to build the compiler from a checkout to use the language.
Install `ccc`, run a program, then follow the examples learning path.

Every unit starts with a **line-1 header** (kind + optional lowerer pin). Emit
strips it; it is not program text:

```text
#!ccc ccs                         # source (.ccs)
#!ccc cch                         # header (.cch)
#!ccc ccs version=0.3.3           # pin lowerer (prefix of ccc --version)
#!/usr/bin/env -S ./cc/bin/ccc version=0.3.3   # script (.shcc); OS shebang
```

`version=MAJOR[.MINOR[.PATCH[-SEED]]]` keeps that file on a matching bootstrap
seed when the toolchain moves. Omit the pin only when you intentionally want
whatever `ccc` is running. A `.ccs` / `.cch` / `.shcc` suffix is the fallback
when the header is absent — prefer the header so kind and pin travel with the
file. Details: [backwards compatibility](backwards_compatibility.md).

## Install

Needs a C compiler (`cc` / `clang`) on `PATH`. Homebrew also needs `make`.

### Homebrew (macOS)

The formula is HEAD-only:

```bash
brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
brew install --HEAD sreekotay/concurrent-c/ccc
ccc --version
```

### From source

`git`, `make`, and a C compiler. Roughly half a minute on four cores and about
40M of disk:

```bash
git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
cd concurrent-c
PREFIX="$HOME/.local" ./cc-install.sh
```

That builds the toolchain (and its TinyCC comptime dependency), installs under
`$PREFIX`, and compiles a small program to prove the install. Put `$PREFIX/bin`
on `PATH` (the script can edit your shell rc via `--add-to-path`).
`cc-install.sh` also works from an existing checkout with a custom `PREFIX`.

Build outputs from `ccc` land in `./out` and `./bin` under the directory you
run from (`--out-dir` / `--bin-dir`); never into `$PREFIX`.

### VS Code / Cursor syntax (required for a good edit experience)

> **Install the Concurrent-C syntax extension.** Search **Concurrent-C** in the
> Extensions view, or:
>
> ```bash
> code --install-extension sreekotay.concurrent-c-syntax
> cursor --install-extension sreekotay.concurrent-c-syntax
> ```
>
> | How you installed `ccc` | What to do |
> |-------------------------|------------|
> | **Marketplace / Extensions view** | Preferred. Language mode should read **Concurrent-C**. |
> | **`./cc-install.sh`** | Also copies the in-tree package (unless `--no-editor-tools`). Tries CodeLLDB when the `code` / `cursor` CLI exists. |
> | **From a clone** (no store) | `./vscode/ccs-syntax/install-local.sh --both` then **Developer: Reload Window**. |
>
> Details: [`vscode/ccs-syntax/README.md`](../vscode/ccs-syntax/README.md).
> Debug configs: [Debugging](debugging.md).

## Your first program

Create `hello.ccs`:

```c
#!ccc ccs
#include <ccc/cc_runtime.cch>
#include <ccc/script/stdio.cch>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);

    CCArena a = cc_arena_heap(kilobytes(4)) @destroy;
    CCStdio io = cc_stdio_create(&a);

    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => [io] {
        @errhandler(CCError e) cc_error_exit(e);
        io.println("Hello from task A!") !>;
    });
    n->spawn(() => [io] {
        @errhandler(CCError e) cc_error_exit(e);
        io.println("Hello from task B!") !>;
    });
    return 0;
}
```

```bash
ccc run hello.ccs
```

Typical output (task order is not fixed):

```text
Hello from task B!
Hello from task A!
```

What that program uses:

| Piece | Role |
|-------|------|
| `@errhandler(…)` | Scope policy that **bare `!>`** routes `E` to |
| `T!>(E)` | Fallible value. `?>` : `E → T`. `!>` : `E →` control flow |
| `@destroy` | Cleanup on **successful declaration construction** (`!> @destroy` = unwrap succeeded, then defer) |
| `CCArena` / `CCStdio` | Arena **names** the window’s lifetime; growth/overflow is storage policy — prefer **`io.println(…)`** (see [Arenas](#arenas-name-a-lifetime)) |
| `CCNursery*` | Structured-concurrency scope: `@destroy` waits; `abandon` drops the handle |
| `n->spawn(() => [io] { … })` | UFCS spawn of a closure; capture `io` by value into the task |

So `CCNursery* n = cc_nursery_create(NULL) !> @destroy;` is `!>` (unwrap or
route `E`) then `@destroy` (cleanup on that successful construction). Teardown
runs at the end of `main` here, so both tasks finish before the process exits.

A fuller hello (stdio helpers, per-task `@errhandler`, local-then-default
errors) is in the repo: [examples/hello.ccs](../examples/hello.ccs).

```bash
ccc run examples/hello.ccs    # from a clone
```

Expected shape of output:

```text
Hello from task B!
Hello from task C!
Hello from task A! (last)
All tasks completed.
```

## Language surface (day one)

Concurrent-C keeps C’s model and adds a small surface. Cleanup, results, then
UFCS show up in almost every example — treat them as day-one, not advanced.

| Idea | Spellings |
|------|-----------|
| Cleanup | `@defer …` / `@destroy` (cleanup on successful declaration construction) |
| Errors | `T!>(E)`; `?>` : `E → T`; `!>` : `E →` control flow; `(e)` exposes `E`; bare `!>` routes `E` |
| Methods (UFCS) | `recv.method(args)` — ordinary functions; prefer this form |
| Generics | `Name::[args]` — Vec / Map / ArrayMap / `T[:]` are factory families |
| Arenas / slices | arena **names a lifetime**; alloc strategy is policy for that lifetime’s storage; `T[:]` views carry provenance (below) |
| Closures | `() => …`, `() => [x] { … }`, `() => [&x] { … }` — tasks re-bind `@errhandler` |

### Destroy registration — what bodyless `@destroy` calls

`@destroy` attaches cleanup to **successful declaration construction** — `@defer`
sugar on the binding. After `!>`, that means the unwrap succeeded (no binding →
no destroy). The destroy chain is registered pre-destroy → `@destroy { body }`
if present → registered destroy → each value field whose type has a hook,
last-declared to first. Pointer, array, and function-pointer fields are
omitted. Bodyless `@destroy` emits that list without a call-site body. An
empty chain is a compile error. `.destroy()` is UFCS (`Type_destroy` when
that function exists), not this list.

Stdlib owners ship registered (`CCNursery*` waits then frees, `CCArena` frees
slabs + overflow, channels, `CCPy`, …). For your own types:

```c
static void my_res_close(MyRes* r) { /* … */ }

@typehooks on MyRes {
    .destroy = cc_type_destroy_call("my_res_close"),
};

MyRes r = my_res_open() !> @destroy;   // → defer my_res_close
```

Nursery: wait → your body → free, then any nested value-field hooks.
Details: [Language Concepts §1](language-concepts.md#1-cleanup-binds-to-a-place).
Full walkthrough (hooks + views): [typehooks-typeviews.md](typehooks-typeviews.md).

### Results — `T!>(E)`

Fallible work returns `T!>(E)`. Two operators; three modifiers:

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
int b = read() !>;                         // routes E
int c = read() !>(e) { /* local */ @err(e); };
CCNursery* n = cc_nursery_create(NULL) !> @destroy;
```

Tasks do not inherit `@errhandler` — re-bind inside each spawn. More:
[recipe_result_error_handling.ccs](../examples/recipe_result_error_handling.ccs) ·
[Language Concepts §2](language-concepts.md#2-errors-map-to-a-value-or-to-control-flow).

### UFCS — methods are ordinary functions

`recv.method(args)` calls the function the **receiver’s type** names. For the
usual path, **declaring that function *is* installing the method** — no trait
and no `@typehooks` step (unlike bodyless `@destroy`). `.destroy()` uses
this path. Method and free forms are the same API; Concurrent-C examples
prefer the method form:

```c
n->spawn(() => { … });     // not cc_nursery_spawn(n, …)
tx.send(i) !>;             // not cc_chan_send(tx, i)
io.println("hi") !>;       // not cc_stdio_println(&io, "hi")
v.push(10);                // == CCVec_int_push(&v, 10)
u.mean(6.0);               // == mean(u, 6.0)  (bare-name: 1st param fits)

/* extend a family: one declaration */
static double CCVec_double_median(CCVec_double* v) {
    return v->len ? v->data[v->len / 2] : 0.0;
}
v.median();
```

**UFCS registration** (optional, library-author): a type can also register a
`.ufcs` lowerer so `x.method` rewrites through a naming family (`cc_<type>_…`,
nursery/channel hooks, …). Stdlib types do this; you usually don’t until you
own a family:

```c
@typehooks on MyHandle* {
    .ufcs = my_handle_ufcs_lower_c,   /* → cc_my_handle_<method>(…) */
};
```

Receiver first (arena last when needed). Fallible chain: unwrap (`!>` / `?>`),
then the next method sees the value (`get(21)!>.twice()`). Full matrix:
[recipe_ufcs_forms.ccs](../examples/recipe_ufcs_forms.ccs) ·
[Language Concepts §3](language-concepts.md#3-methods-are-ordinary-functions).

Quick reference: [Cheatsheet](cheatsheet.md). Spec: [language spec](../spec/concurrent-c-spec-complete.md).

### Generics — `Name::[args]`

`Name::[args]` names a monomorph of a library factory. The factory’s emitted
`${mangled}_<member>` functions are the methods — the same dispatch as
`v.push`. Stdlib `Vec::[T]`, `Map::[K,V]`, `ArrayMap::[K,V]`, and non-char
`T[:]` use `CC_GENERIC_FACTORY` like a user `Pair::[A,B]`. Arguments may be
types or non-negative decimal integers (`SmallVec::[int, 8]`). Multi-word types stay one argument
(`SmallVec::[long long, 8]`). A missing
factory is a use-site error — include the header that registers it.

```c
Vec::[int] v@(&arena) @destroy;     // CCVec_int; dot UFCS
v.push(10);
Map::[int, double] m = map_new::[int, double](&arena);
m->insert(1, 2.5);                  // Map is Name*; arrow UFCS
```

`.` = value receiver, `->` = pointer. Free-name grid:
`vec_new::[int](&arena)` is the same instance as the binder. Recipe:
[recipe_user_generics.ccs](../examples/recipe_user_generics.ccs) ·
[Cheatsheet](cheatsheet.md#generics-nameargs) · spec
[§12.1](../spec/concurrent-c-spec-complete.md#121-registered-factories).

## Arenas name a lifetime

**An arena names a lifetime. Its allocation strategy is an implementation
policy for storage belonging to that lifetime.**

The `CCArena` binding is that lifetime: allocate into it; when it is destroyed
(or reset), every allocation from that epoch is gone. Heap vs stack root,
slab growth, and overflow are **how** storage for that lifetime is obtained —
not a second concept of “allocator object” separate from the lifetime.
Slices (`T[:]`) remember which arena (or stack / static / unique) they came
from — that provenance is what the compiler uses to reject “view outlives
storage.”

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;  /* names this lifetime */
CCStdio io = cc_stdio_create(&a);
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);               /* arena provenance */
io.println(@string(`len=${s.len}`, @scratch)) !>;  /* @scratch: @string arena only */
```

**Allocation policy** (`cc_arena_heap` / `cc_arena_stack` defaults) — three
storage tiers for the named lifetime, not a different lifetime model:

1. **L1** — bump-allocate in the root slab of size `N` (size `N` for the typical live set).
2. **L2** — when L1 is full, grow with more slabs (up to four, ~1.5× each).
3. **Main** — past that budget, `malloc` for the request, still owned by the
   arena — freed on `reset` / `@destroy`, same as slab bytes.

So a tiny L1 still works; traffic just spends more time in Main (costlier
alloc and drain). Main is the escape hatch, not the steady-state path.
Prefer a second arena when lifetimes diverge rather than churning
`cc_arena_release` on a long-lived one. `a.live()` counts L1 + L2 + Main.

`a.try_checkpoint() !>` / `cp.try_restore() !>` work after overflow alloc:
the handle is a consumed loan (`@destroy` restores). Restore rewinds slabs
and frees Main minted in the later epoch. A mid-slab hole disables a new
capture until last-live rewind or `reset`. Restore of a handle whose
overflow keep-set was released refuses (does not pretend to succeed).
`detach()` refuses a stack or caller-owned L1.

| Constructor | Role |
|-------------|------|
| `cc_arena_heap(N) @destroy` | Named lifetime; heap L1 + L2 grow + Main overflow |
| `cc_arena_stack(name, N)` | Same lifetime; L1 on the stack; `@destroy` at scope exit |
| `@scratch` | Arena operand of `@string` only — bind the `CCString` before `return` / `cc_script_sh_read`; do not `scratch.destroy()` |

Rules of thumb: a view must not outlive its arena; do not capture stack /
`@scratch` slices into a task or channel. Capturing an arena slice into a
nursery **pins** that arena until join (no reset/destroy while the pin is live).
More: [Language Concepts §4](language-concepts.md#4-slices-remember-where-bytes-live),
[recipe_arena_scope.ccs](../examples/recipe_arena_scope.ccs),
[allocator strategy](../spec/draft_alloc_strategy.md).

## Concurrency

### Nurseries

Tasks are scoped to an owned `CCNursery*`. `@destroy` waits for everything
spawned into that nursery (and nested children) before the binding ends:

```c
@errhandler(CCError e) cc_error_exit(e);
{
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => task1());
    n->spawn(() => task2());
}
/* both tasks have finished */
```

`@destroy` waits. To consume the handle without joining, register optional
after-work and abandon (`n->on_last(ctx, finish); n->abandon();`). Last-exit
closes registered channels, runs the hook, and frees the nursery. That is
not cancel. Spec §8.1.5.

### `@parallel`

Independent work that joins at a brace — no nursery, no task handle.
`@serial` is a multi-statement arm that writes one outer name.

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

`@parallel (pred) { … }` runs the same arms; spawn only if `pred` is true.
Recipe: [recipe_parallel.ccs](../examples/recipe_parallel.ccs). Spec §8.11.

### Channels

Typed ends `T[~N >]` (send) and `T[~N <]` (recv). Pair them, send/recv with
UFCS, and let nested nurseries own the close protocol — consumer outside,
producer + `close_on(tx)` inside:

```c
@errhandler(CCError e) cc_error_exit(e);

int[~10 >] tx;
int[~10 <] rx;
CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy;

{
    CCNursery* outer = cc_nursery_create(NULL) !> @destroy;

    outer->spawn(() => [rx] {
        int v;
        while (cc_io_avail(rx.recv(&v)))
            printf("got %d\n", v);
    });

    {
        CCNursery* inner = cc_nursery_create(outer) !> @destroy;
        (void)inner->close_on(tx);

        inner->spawn(() => [tx] {
            for (int i = 0; i < 5; i++)
                (void)tx.send(i);
        });
    }
}
```

Runnable version with expected sum:
[examples/recipe_channel_pipeline.ccs](../examples/recipe_channel_pipeline.ccs).
Close/deadlock details and env guards live there and in [Debugging](debugging.md)
— not required to start writing programs.

### Named exclusive

When several fibers must briefly mutate the same named resource,
`CCExclusive` is per-name mutual exclusion. Keep the section short — do not
`@await` under the hold. To wait until a condition is already true *under*
that name, use `acquire_when` (Result; cancel is not a zeroed guard):

```c
CCExclusiveGuard g = excl->acquire_when(name, pred, env) !> @destroy;
```

The holder that makes `pred` true calls `h.signal()` while still holding.
The wait parks a fiber or an OS thread. An expired deadline is
`CC_ERR_TIMEOUT`; a cancelled deadline or nursery (when one exists) is
`CC_ERR_CANCELLED`. Recipe: [recipe_exclusive_named.ccs](../examples/recipe_exclusive_named.ccs).

### Async / await

Prefer `@parallel` for independent value joins that finish at a brace.
Prefer `n->spawn` for sibling work under a nursery (named lifetime, channels,
cancel). Prefer `@async` / `@await` when one call stack should suspend
without inventing a nursery just to join. Drive an async stack from sync
`main` with `@await` (or `cc_block_on` where appropriate).

Full recipe: [examples/recipe_async_await.ccs](../examples/recipe_async_await.ccs).

```c
@async int bump(int value) {
    return value + 1;
}

int main(void) {
    @errhandler(CCError e) { cc_error_log(e); return 1; }
    int result = @await bump(41);
    return result == 42 ? 0 : 1;
}
```

## Learning path

Work the recipes in order — they are the intended tutorial:

[examples/README.md — Learning Path](../examples/README.md#learning-path-recommended-order)

In short: `hello` → results / unwrap / UFCS → generics (`recipe_user_generics`) → captures → channels → async →
timeouts / worker pool → arenas / defer. Then networking
(`recipe_tcp_echo.ccs`, `recipe_http_get.ccs`) and build-system examples under
`examples/`.

Larger measured programs (pigz, Redis subset, CPython extension patterns,
the Shirley weekend raytracer) live under
[`real_projects/`](../real_projects/); benches under [`perf/`](../perf/).

## `.shcc` scripts

`.shcc` is the **script** form of Concurrent-C — same language as `.ccs`, but
meant for short tools (read bytes, `@grammar` / SERDES, `@string`, spawn
processes, print reports). The extension is deliberately outside the
`.ccs` / `.cch` source–header pair.

What the driver does for a `.shcc` unit:

- Strips a leading `#!` shebang when present
- Force-includes `<ccc/script/prelude.cch>` (that prelude includes
  `<ccc/std/cli.cch>`, so `@grammar(cli)` is in scope without a further
  include; `.ccs` units include `cli.cch` themselves)
- If there is no top-level `main`, wraps top-level statements in a synthetic
  `main` with a default `@errhandler(CCError)`
- May inject ambient `a` / `io` / `in` / `args` into that wrap when you use those names
- Treats a bare `ccc path/to/tool.shcc …` as **run** (shebang-friendly)

```bash
ccc examples/py/pydemo.shcc          # CC hosts Python
ccc examples/js/jsdemo.shcc          # CC→JS call surface (Node owns env)
ccc run tools/cc_perf_check.shcc -- --help
./tools/perf.shcc @                  # list @task entries
```

Shebang from a repo checkout (cwd = repo root):

```text
#!/usr/bin/env -S ./cc/bin/ccc [--as=shcc] [version=0.3.3]
```

`--as=shcc` is optional on a `ccc` interpreter shebang. `#!ccc shcc` is
ill-formed — scripts must be OS-executable. Pins:
[backwards compatibility](backwards_compatibility.md).

Try [examples/py/pydemo.shcc](../examples/py/pydemo.shcc),
[examples/js/jsdemo.shcc](../examples/js/jsdemo.shcc), or
[tools/cc_perf_check.shcc](../tools/cc_perf_check.shcc). Full rules:
[Language spec §9.5 — Script Library](../spec/concurrent-c-spec-complete.md#95-script-library-shcc--cccscript);
one-liners: [draft_script_oneliners.md](../spec/draft_script_oneliners.md).

## Python and JavaScript

Three related doors:

| Goal | Doc / package |
|------|----------------|
| One `.ccs` → native Node **and** CPython module | [js-py-modules.md](js-py-modules.md), `examples/recipe_js_module.ccs`, `examples/recipe_py_module.ccs` |
| Call Python from Node | npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python) |
| Call JS / npm from Python | pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/) |

Embed Python from a CC `main`: `examples/recipe_py_interop.ccs`.
Script form of the same door: [examples/py/pydemo.shcc](../examples/py/pydemo.shcc).

## Next

- [Language Concepts](language-concepts.md)
- [@typehooks / @typeview](typehooks-typeviews.md) — register destroy/create, faces, allow-lists
- [Cheatsheet](cheatsheet.md)
- [Backwards compatibility](backwards_compatibility.md) — unit headers and version pins
- [`.shcc` scripts](#shcc-scripts) · [spec §9.5](../spec/concurrent-c-spec-complete.md#95-script-library-shcc--cccscript)
- [examples/](../examples/)
- [Debugging](debugging.md)
- [Docs index](README.md)
- [Language spec](../spec/concurrent-c-spec-complete.md) · [Stdlib](../spec/concurrent-c-stdlib-spec.md)

## Hacking on the compiler (optional)

Only if you are changing this repository’s toolchain — not required to write
Concurrent-C programs.

**When to run which command:** [build-when.md](build-when.md).

First checkout (once; same idea as `cc-install.sh` without installing to
`$PREFIX`):

```bash
./scripts/fetch_submodules.sh
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs" libtcc.a tcc libtcc1.a)
make cc -j"$jobs"
./cc/bin/ccc run examples/hello.ccs
```

After that: stdlib → `make -C cc lower-headers`; lowerer faces →
`./scripts/iterate_shadow_lower.sh`; ship seed →
`./scripts/iterate_shadow_lower.sh --ship --smoke`. Binaries: `./cc/bin/ccc`,
`./out/cc/bin/shadow_lower`. Architecture notes:
[cc/docs/ARCHITECTURE.md](../cc/docs/ARCHITECTURE.md).
