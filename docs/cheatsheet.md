# Concurrent-C Cheatsheet

Quick reference for common patterns. See [spec/](../spec/) for full details.

---

## Build & Run

```bash
ccc run file.ccs                    # build + run (shorthand)
ccc build run file.ccs              # same, explicit
ccc build run file.ccs -- --arg     # pass args to binary
ccc --emit-c-only file.ccs          # emit C only → out/file.c
ccc build -O file.ccs               # release build
ccc build -g file.ccs               # debug build
```

---

## Structured Concurrency

```c
#include <ccc/cc_runtime.cch>

// Basic nursery - waits for all spawned tasks
@nursery {
    spawn(() => do_work());
    spawn(() => do_other_work());
}
// Both tasks complete before this line

// Nested nurseries
@nursery {
    spawn(() => {
        @nursery {
            spawn(subtask1());
            spawn(subtask2());
        }
    });
}
```

---

## Channels

```c
// Declare sender (>) and receiver (<)
int[~10 >] tx;   // capacity 10, sender
int[~10 <] rx;   // capacity 10, receiver
channel_pair(&tx, &rx);

// Send and receive
chan_send(tx, 42);
int val;
chan_recv(rx, &val);

// Close sender when done
chan_close(tx);

// Check if channel closed
if (chan_recv(rx, &val) != 0) {
    // channel closed
}

// Iterate until closed (in closing() scope)
@nursery {
    spawn([tx]() => {
        for (int i = 0; i < 10; i++) chan_send(tx, i);
    });
    closing(tx) {
        int v;
        while (chan_recv(rx, &v) == 0) {
            printf("%d\n", v);
        }
    }
}
```

---

## Timeouts & Cancellation

```c
// Deadline scope - cooperative timeout
@with_deadline(1000) {  // 1000ms
    CCDeadline* dl = cc_current_deadline();
    while (!cc_deadline_expired(dl)) {
        do_work();
    }
}

// Check cancellation in tasks
if (cc_is_cancelled()) return;

// Sleep (cancellation-aware)
cc_sleep_ms(100);
```

---

## Memory: Arenas

```c
// Heap-backed arena (recommended)
@arena(a, kilobytes(4)) {
    void* buf = cc_arena_alloc(a, 1024, 8);
    char* str = cc_arena_strdup(a, "hello");
    // auto-freed at scope exit
}

// Reset arena (reuse memory)
@arena(a, kilobytes(4)) {
    for (int i = 0; i < 100; i++) {
        void* tmp = cc_arena_alloc(a, 64, 8);
        process(tmp);
        cc_arena_reset(a);  // reuse for next iteration
    }
}
```

---

## Cleanup: Defer

```c
FILE* f = fopen("data.txt", "r");
@defer fclose(f);
// ... use f ...
// fclose() called automatically on scope exit

// Multiple defers run in reverse order
@defer printf("3\n");
@defer printf("2\n");
@defer printf("1\n");
// prints: 1, 2, 3
```

---

## Optionals (`T?`)

```c
int? maybe = cc_some(42);
int? empty = cc_none(int);

if (cc_is_some(maybe)) {
    int val = *maybe;  // unwrap
}

// Or use cc_unwrap (asserts non-empty)
int val = cc_unwrap(maybe);
```

---

## Results (`T!E`)

```c
typedef struct { int code; } MyError;

int!MyError divide(int a, int b) {
    if (b == 0) return cc_err((MyError){.code = 1});
    return cc_ok(a / b);
}

// Usage
int!MyError result = divide(10, 2);
if (cc_is_ok(result)) {
    printf("result: %d\n", cc_unwrap_ok(result));
} else {
    printf("error: %d\n", cc_unwrap_err(result).code);
}

// Propagate errors with try
int!MyError caller(void) {
    int val = try divide(10, 0);  // returns early on error
    return cc_ok(val * 2);
}
```

---

## Closures

```c
// Lambda syntax
spawn(() => printf("hello\n"));

// With captures (value by default)
int x = 42;
spawn([x]() => printf("x = %d\n", x));

// Reference capture
spawn([&x]() => { x++; });

// Explicit copy capture
spawn([=x]() => printf("x = %d\n", x));
```

---

## Common Patterns

### Worker Pool
```c
@nursery {
    int[~100 >] jobs_tx;
    int[~100 <] jobs_rx;
    channel_pair(&jobs_tx, &jobs_rx);

    // Spawn workers
    for (int i = 0; i < 4; i++) {
        spawn([jobs_rx]() => {
            int job;
            while (chan_recv(jobs_rx, &job) == 0) {
                process(job);
            }
        });
    }

    // Send jobs then close
    closing(jobs_tx) {
        for (int j = 0; j < 100; j++) {
            chan_send(jobs_tx, j);
        }
    }
}
```

### Fan-out / Fan-in
```c
@nursery {
    int[~16 >] results_tx;
    int[~16 <] results_rx;
    channel_pair(&results_tx, &results_rx);

    // Fan-out: spawn N workers
    for (int i = 0; i < 16; i++) {
        spawn([i, results_tx]() => {
            int result = compute(i);
            chan_send(results_tx, result);
        });
    }

    // Fan-in: collect results
    closing(results_tx) {
        int sum = 0, r;
        while (chan_recv(results_rx, &r) == 0) {
            sum += r;
        }
        printf("total: %d\n", sum);
    }
}
```

### Producer/Consumer Pipeline
```c
@nursery {
    int[~10 >] tx;
    int[~10 <] rx;
    channel_pair(&tx, &rx);

    // Producer
    spawn([tx]() => {
        for (int i = 0; i < 100; i++) {
            chan_send(tx, i);
        }
    });

    // Consumer
    closing(tx) {
        int v;
        while (chan_recv(rx, &v) == 0) {
            printf("got %d\n", v);
        }
    }
}
```

---

## Build System (`build.cc`)

```c
// build.cc
CC_TARGET main exe main.ccs utils.ccs
CC_TARGET_LIBS main pthread
CC_DEFAULT main

// Multi-target
CC_TARGET lib obj lib.ccs
CC_TARGET app exe app.ccs
CC_TARGET_DEPS app lib
```

```bash
ccc build                           # build default target
ccc build run                       # build + run default
ccc build list                      # show targets
ccc build app                       # build specific target
ccc build --build-file path/build.cc
```

---

## Environment Variables

| Variable | Purpose |
|----------|---------|
| `CC` | C compiler (default: cc/gcc/clang) |
| `CC_OUT_DIR` | Generated C + objects (default: out/) |
| `CC_BIN_DIR` | Linked executables (default: bin/) |
| `CC_NO_CACHE` | Disable incremental cache |

---

## Includes

```c
#include <ccc/cc_runtime.cch>     // Core runtime (nursery, spawn, channels)
#include <ccc/std/prelude.cch>    // Convenience (kilobytes, heap arena, etc.)
#include <ccc/cc_atomic.cch>      // Portable atomics
```
