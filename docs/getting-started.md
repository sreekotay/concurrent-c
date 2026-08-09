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

`cc-install.sh` also works from an existing checkout, accepts a custom
`PREFIX`, and (unless `--no-editor-tools`) installs the Concurrent-C syntax
package for VS Code / Cursor plus CodeLLDB when those CLIs are present. See
the script’s `--help` and [Debugging](debugging.md).

Build outputs from `ccc` land in `./out` and `./bin` under the directory you
run from (`--out-dir` / `--bin-dir`); never into `$PREFIX`.

## Your first program

Create `hello.ccs`:

```c
#include <ccc/cc_runtime.cch>
#include <stdio.h>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);

    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => printf("Hello from task A!\n"));
    n->spawn(() => printf("Hello from task B!\n"));
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
| `CCNursery*` | Structured-concurrency scope: teardown waits for spawned tasks |
| `n->spawn(() => …)` | UFCS call to spawn a closure (not “call result of `…()`”) |

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
| Slices / arenas | `T[:]`, `cc_arena_heap` / `@scratch`, provenance rules |
| Closures | `() => …`, `[x]() => …`, `[&x]() => …` — tasks re-bind `@errhandler` |

Quick reference: [Cheatsheet](cheatsheet.md). Full rules: [language spec](../spec/concurrent-c-spec-complete.md).

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
