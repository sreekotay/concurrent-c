# Getting Started with Concurrent-C

## Prerequisites

- C compiler (gcc or clang)
- C++ compiler (`c++`) — builds the runtime's float formatter
- POSIX system (Linux, macOS)
- make, git

## Install

```bash
git clone --filter=blob:none https://github.com/sreekotay/concurrent-c.git
cd concurrent-c
PREFIX="$HOME/.local" ./cc-install.sh
```

That fetches submodules, builds a patched TinyCC and the compiler, installs to
`$PREFIX`, and verifies the install by compiling a program against it. Add
`$PREFIX/bin` to `PATH` and `ccc` is ready:

```bash
ccc run hello.ccs
```

Build outputs land in `./out` and `./bin` under whatever directory you run
`ccc` from; `--out-dir` overrides that.

## Or build without installing

```bash
./scripts/fetch_submodules.sh
./scripts/apply_tcc_patches.sh
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
(cd third_party/tcc && ./configure --config-cc_ext && make -j"$jobs")
make cc -j"$jobs"
```

The compiler is now at `./cc/bin/ccc`, usable directly from the checkout.

## Your First Program

Create `hello.ccs`:

```c
#include <ccc/cc_runtime.cch>
#include <stdio.h>

int main(void) {
    printf("Starting...\n");
    
    CCNursery* n = @create(NULL) @destroy;
    if (!n) return 1;
    n->spawn(() => printf("Hello from task A!\n"));
    n->spawn(() => printf("Hello from task B!\n"));
    
    printf("Done.\n");
    return 0;
}
```

Build and run:

```bash
./cc/bin/ccc run hello.ccs
```

Output (order may vary):
```
Starting...
Hello from task A!
Hello from task B!
Done.
```

## Key Concepts

### Structured Concurrency

All concurrent tasks are scoped to an owned `CCNursery*`. Its `@destroy` waits for all spawned tasks to complete:

```c
{
    CCNursery* n = @create(NULL) @destroy;
    if (!n) return 1;
    n->spawn(() => task1());   // spawn takes a closure, not a call result
    n->spawn(() => task2());
}
// Both tasks complete before this line
```

### Adopting FFI buffers

Foreign owned pointers become unique slices with a trusted deleter:

```c
CCSliceUnique s = cc_adopt(malloc(64), 64, free) @destroy;
// s.destroy() / @destroy calls free once; send_take is rejected (not transferable)
```

`cc_slice_from_buffer` is untracked (no destructor). `cc_adopt` is the opposite: unique provenance plus a registered free function. The deleter must match the allocator — the language cannot check that (same trust boundary as Rust `unsafe` / `from_raw`).

### Channels

Send messages between tasks:

```c
int[~10 >] tx;  // sender, capacity 10
int[~10 <] rx;  // receiver
CCChan* ch = cc_channel_pair(&tx, &rx) !> @destroy;  // @destroy frees the channel

{
    CCNursery* outer = cc_nursery_create(NULL) !> @destroy;

    outer->spawn(() => [rx] {           // consumer: drains until tx closes
        int v;
        while (cc_io_avail(rx.recv(&v)))
            printf("got %d\n", v);
    });

    {
        CCNursery* inner = cc_nursery_create(outer) !> @destroy;
        (void)inner->close_on(tx);      // tx auto-closes when the producer finishes

        inner->spawn(() => [tx] {       // producer
            for (int i = 0; i < 5; i++)
                (void)tx.send(i);
        });
    }
}
```

This nested-nursery shape (consumer outside, producer + `close_on` inside) is the
deadlock-free close protocol — see `examples/recipe_channel_pipeline.ccs`. Putting the
consumer in the *same* nursery that closes `tx` deadlocks: the compiler does NOT catch
this pattern — the runtime deadlock detector reports it when it happens (or, with
`CC_NURSERY_CLOSING_RUNTIME_GUARD=1`, the recv fails with `EDEADLK` instead of hanging).

### Cleanup with `@defer`

Guaranteed cleanup on scope exit:

```c
FILE* f = fopen("data.txt", "r");
@defer fclose(f);
// ... use f ...
// fclose runs automatically
```

### Scoped Memory with Owned Arenas

Fast bump allocation with automatic cleanup:

```c
{
    CCArena arena = @create(kilobytes(4)) @destroy;
    if (!arena.base) return 1;
    void* buf = cc_arena_alloc(&arena, 1024, 8);
    // ... use buf ...
}
// Arena freed automatically
```

### Async/Await

For cooperative concurrency without threads:

```c
@async int fetch_value(int id) {
    // await suspends until operation completes
    intptr_t rc = await some_async_op();
    return (int)rc;
}

int main(void) {
    // Run async function to completion
    int result = cc_block_on(int, fetch_value(42));
}
```

### Task Combinators

Run multiple async tasks concurrently:

```c
// Create task handles
CCTaskIntptr tasks[] = {
    fetch_value(1),
    fetch_value(2),
    fetch_value(3),
};
intptr_t results[3];

// Wait for ALL to complete
cc_block_all(3, tasks, results);

// Or wait for first to complete (race)
int winner;
intptr_t first_result;
cc_block_race(3, tasks, &winner, &first_result);
```

## Next Steps

- Browse [examples/](../examples/) for more patterns
- Read the [language spec](../spec/concurrent-c-spec-complete.md) for full details
- See [debugging.md](debugging.md) for VS Code setup
