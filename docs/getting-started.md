# Getting Started with Concurrent-C

## Prerequisites

- C compiler (gcc or clang)
- POSIX system (Linux, macOS)
- make

## Build the Compiler

```bash
git clone <repo>
cd concurrent-c
cd cc && make
```

The compiler is now at `./cc/bin/ccc`.

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
    n->spawn(task1());
    n->spawn(task2());
}
// Both tasks complete before this line
```

### Channels

Send messages between tasks:

```c
int[~10 >] tx;  // sender, capacity 10
int[~10 <] rx;  // receiver
CCChan* ch = channel_pair(&tx, &rx);

{
    CCNursery* producer = @create(NULL) @destroy {
        chan_close(tx);
    };
    if (!producer) return 1;
    producer->spawn(() => {
        for (int i = 0; i < 5; i++) {
            (void)chan_send(tx, i);
        }
    });

    int v = 0;
    while (cc_io_avail(chan_recv(rx, &v))) {
        printf("got %d\n", v);
    }
}
cc_chan_free(ch);
```

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
