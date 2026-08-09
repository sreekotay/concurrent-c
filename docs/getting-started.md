# Getting Started with Concurrent-C

Concurrent-C is a **strict C11-superset**: you write `.ccs`, the `ccc` driver
lowers it to plain C (with `#line` sourcemaps), and your **host C compiler**
builds the binary. Structured concurrency, results, UFCS, slices/arenas, and a
header-first runtime ship with the language.

You do **not** need to build the compiler from a checkout to use the language.
Install `ccc`, run a program, then follow the examples learning path.

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

> **Install the Concurrent-C syntax extension** — not on the marketplace.
> Highlighting for `.ccs` / `.cch` / `.shcc` ships in-tree.
>
> | How you installed `ccc` | What to do |
> |-------------------------|------------|
> | **`./cc-install.sh`** | Already done (unless you passed `--no-editor-tools`). Also tries CodeLLDB when the `code` / `cursor` CLI exists. |
> | **Homebrew** (or you skipped editor tools) | From a clone of this repo: |
>
> ```bash
> git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
> cd concurrent-c
> ./vscode/ccs-syntax/install-local.sh --both   # VS Code + Cursor
> ```
>
> Then **Developer: Reload Window**. Open a `.ccs` file — language mode should
> read **Concurrent-C**.
>
> Details: [`vscode/ccs-syntax/README.md`](../vscode/ccs-syntax/README.md).
> Debug configs: [Debugging](debugging.md).

## Your first program

Create `hello.ccs`:

```c
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
| `@errhandler(…)` | Scope policy for fallible unwraps that use bare `!>;` |
| `T!>(E)` / `!>` | Fallible value; `!>` unwraps or runs error code |
| `!> @destroy` | Unwrap, then schedule cleanup — **`@destroy` is `@defer` sugar attached to the binding** (same LIFO ledger; only runs if the unwrap succeeded) |
| `CCArena` / `CCStdio` | Arena = allocator **and** lifetime for the window; `io` borrows it — prefer **`io.println(…)`** (see [Arenas](#arenas-lifetime-not-just-malloc)) |
| `CCNursery*` | Structured-concurrency scope: teardown waits for spawned tasks |
| `n->spawn(() => [io] { … })` | UFCS spawn of a closure; capture `io` by value into the task |

So `CCNursery* n = cc_nursery_create(NULL) !> @destroy;` is the short form of
“unwrap, bind `n`, `@defer` the nursery’s destroy on scope exit.” Teardown runs
at the end of `main` here, so both tasks finish before the process exits.

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

Concurrent-C keeps C’s model and adds a small surface. Learn these next in
[Language Concepts](language-concepts.md); the one-line map:

| Idea | Spellings |
|------|-----------|
| Cleanup | `@defer …` (statement on the scope). `@destroy` is the same defer ledger, written on the **declaration** — bodyless form calls the type’s registered destroy |
| Errors as values | `T!>(E)`, then `?>` (default) or `!>` / `!>;` (must leave); `!> @destroy` = successful unwrap + deferred destroy |
| Methods | `recv.method(args)` — ordinary functions; prefer UFCS at call sites |
| Arenas / slices | bump allocator as a **lifetime**; `T[:]` views carry provenance (below) |
| Closures | `() => …`, `[x]() => …`, `[&x]() => …` — tasks re-bind `@errhandler` |

Quick reference: [Cheatsheet](cheatsheet.md). Full rules: [language spec](../spec/concurrent-c-spec-complete.md).

## Arenas (lifetime, not just malloc)

An arena is Concurrent-C’s usual allocator **and** a lifetime annotation: the
`CCArena` binding is the scope that owns the bytes. Allocate into it; when the
arena is destroyed (or reset), every allocation from that epoch is gone.
Slices (`T[:]`) remember which arena (or stack / static / unique) they came
from — that provenance is what the compiler uses to reject “view outlives
storage.”

```c
CCArena a = cc_arena_heap(kilobytes(4)) @destroy;  /* lifetime = this binding */
CCStdio io = cc_stdio_create(&a);
char* p = a.allocT(64);
char[:] s = a.alloc_slice_bytes(32);               /* arena provenance */
io.println(@string(`len=${s.len}`, @scratch)) !>;  /* @scratch: throwaway only */
```

**How growth works** (`cc_arena_heap` / `cc_arena_stack` defaults):

1. Bump-allocate in the **root** slab of size `N` (size `N` for the typical live set).
2. When the root is full, grow with more slabs (up to four, ~1.5× each).
3. Past that budget, **heap overflow** kicks in: `malloc` for the request, still
   owned by the arena — freed on `reset` / `@destroy`, same as slab bytes.

So a tiny root still works; traffic just spends more time in overflow (costlier
alloc and drain). Overflow is the escape hatch, not the steady-state path.
Prefer a second arena when lifetimes diverge rather than churning
`cc_arena_release` on a long-lived one.

| Constructor | Role |
|-------------|------|
| `cc_arena_heap(N) @destroy` | Heap root; default grow + overflow — request / window scratch |
| `cc_arena_stack(name, N)` | Same policy; root on the stack — hot frame scratch |
| `@scratch` | Compiler stack scratch for one-shot `@string` / print — do not capture or send |

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

### Async / await

Prefer `n->spawn` for sibling work under a nursery. Prefer `@async` / `@await`
when one call stack should suspend (channel ops, nested async) without inventing
a nursery just to join. Drive an async stack from sync `main` with `@await`
(or `cc_block_on` where appropriate).

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

In short: `hello` → results / unwrap / UFCS → captures → channels → async →
timeouts / worker pool → arenas / defer. Then networking
(`recipe_tcp_echo.ccs`, `recipe_http_get.ccs`) and build-system examples under
`examples/`.

Larger measured programs (pigz, Redis subset, CPython extension patterns) live
under [`real_projects/`](../real_projects/); benches under [`perf/`](../perf/).

## Python and JavaScript

Three related doors:

| Goal | Doc / package |
|------|----------------|
| One `.ccs` → native Node **and** CPython module | [js-py-modules.md](js-py-modules.md), `examples/recipe_js_module.ccs`, `examples/recipe_py_module.ccs` |
| Call Python from Node | npm [`concurrent-c-python`](https://www.npmjs.com/package/concurrent-c-python) |
| Call JS / npm from Python | pip [`concurrent-c-node`](https://pypi.org/project/concurrent-c-node/) |

Embed Python from a CC `main`: `examples/recipe_py_interop.ccs`.

## Next

- [Language Concepts](language-concepts.md)
- [Cheatsheet](cheatsheet.md)
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
