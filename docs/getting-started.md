# Getting Started with Concurrent-C

## Install

Homebrew:

```bash
brew tap sreekotay/concurrent-c https://github.com/sreekotay/concurrent-c.git
brew install sreekotay/concurrent-c/ccc
```

Or from source (`git`, `make`, and a C compiler):

```bash
git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
cd concurrent-c
PREFIX="$HOME/.local" ./cc-install.sh
```

That builds a patched TinyCC and the compiler (including `shadow_lower`, the
default native front), installs both to `$PREFIX/bin`, and verifies the install
by compiling a program against it. Add `$PREFIX/bin` to `PATH` and `ccc` is
ready:

```bash
ccc run hello.ccs
```

The default front is **native** (`shadow_lower`). Use `--frontend=legacy`
(or `CC_FRONTEND=legacy`) only for archaeology of the older multipass path.

Build outputs land in `./out` and `./bin` under whatever directory you run
`ccc` from; `--out-dir` overrides that.

## Hacking on the compiler (checkout builds)

```bash
./scripts/fetch_submodules.sh          # TinyCC; float formatting is vendored
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs")
make cc -j"$jobs"
```

The compiler is then at `./cc/bin/ccc`, usable directly from the checkout.

## Your First Program

[examples/hello.ccs](../examples/hello.ccs):

```c
#include <ccc/cc_runtime.cch>
#include <ccc/script/stdio.cch>
#include <unistd.h>

int main(void) {
    @errhandler(CCError e) cc_error_exit(e);

    CCNursery* n = cc_nursery_create(NULL) !> @destroy {
        println("All tasks completed.") !>;
    };

    n->spawn(() => {
        @errhandler(CCError e) cc_error_exit(e);
        usleep(100000);
        println("Hello from task A! (last)") !>;
    });
    n->spawn(() => {
        @errhandler(CCError e) cc_error_exit(e);
        println("Hello from task B!") !>;
    });

    return 0;
}
```

```bash
ccc run examples/hello.ccs
```

Language surface (defer, results, UFCS, slices, closures): [Language Concepts](language-concepts.md).

## Concurrency

### Structured concurrency

Tasks are scoped to an owned `CCNursery*`. `@destroy` waits for spawned tasks:

```c
@errhandler(CCError e) cc_error_exit(e);
{
    CCNursery* n = cc_nursery_create(NULL) !> @destroy;
    n->spawn(() => task1());   // closure, not a call result
    n->spawn(() => task2());
}
```

### Channels

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

Consumer outside, producer + `close_on` inside: deadlock-free close protocol
(`examples/recipe_channel_pipeline.ccs`). Consumer in the same nursery that
closes `tx` deadlocks at runtime (no general compile-time check). With
`CC_NURSERY_CLOSING_RUNTIME_GUARD=1`, that recv fails with `EDEADLK` instead of
hanging. Runtime deadlock detection exits 124 by default (`CC_DEADLOCK_ABORT=0`
to warn only).

### Async / await

Recipe: [recipe_async_await.ccs](../examples/recipe_async_await.ccs).

```c
@async int fetch_value(int id) {
    intptr_t rc = await some_async_op();
    return (int)rc;
}

int main(void) {
    int result = cc_block_on(int, fetch_value(42));
}
```

### Task combinators

```c
CCTaskIntptr tasks[] = {
    fetch_value(1),
    fetch_value(2),
    fetch_value(3),
};
intptr_t results[3];

cc_block_all(3, tasks, results);

int winner;
intptr_t first_result;
cc_block_race(3, tasks, &winner, &first_result);
```

## Next

- [Language Concepts](language-concepts.md)
- [Cheatsheet](cheatsheet.md)
- [examples/](../examples/)
- [Language spec](../spec/concurrent-c-spec-complete.md)
- [Debugging](debugging.md)
