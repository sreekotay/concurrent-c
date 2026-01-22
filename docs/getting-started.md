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
#include "cc_runtime.cch"
#include <stdio.h>

int main(void) {
    printf("Starting...\n");
    
    @nursery {
        spawn(() => printf("Hello from task A!\n"));
        spawn(() => printf("Hello from task B!\n"));
    }
    
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

All concurrent tasks are scoped to a `@nursery` block. The block waits for all spawned tasks to complete:

```c
@nursery {
    spawn(task1());
    spawn(task2());
}
// Both tasks complete before this line
```

### Channels

Send messages between tasks:

```c
int[~10 >] tx;  // sender, capacity 10
int[~10 <] rx;  // receiver
channel_pair(&tx, &rx);

@nursery {
    spawn(() => {
        for (int i = 0; i < 5; i++) {
            chan_send(tx, i);
        }
    });
    
    spawn(() => {
        int v;
        while (chan_recv(rx, &v) == 0) {
            printf("got %d\n", v);
        }
    });
}
```

### Cleanup with `@defer`

Guaranteed cleanup on scope exit:

```c
FILE* f = fopen("data.txt", "r");
@defer fclose(f);
// ... use f ...
// fclose runs automatically
```

### Scoped Memory with `@arena`

Fast bump allocation with automatic cleanup:

```c
@arena(a, kilobytes(4)) {
    void* buf = cc_arena_alloc(a, 1024, 8);
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
