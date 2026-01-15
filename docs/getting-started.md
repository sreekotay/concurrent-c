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
./cc/bin/ccc build run hello.ccs
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
@arena(a) {
    char* buf = arena_alloc(&a, 1024);
    // ... use buf ...
}
// Memory freed automatically
```

## Next Steps

- Browse [examples/](../examples/) for more patterns
- Read the [language spec](../spec/concurrent-c-spec-complete.md) for full details
- See [debugging.md](debugging.md) for VS Code setup
