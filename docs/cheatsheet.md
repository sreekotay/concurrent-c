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
{
    CCNursery* n = @create(NULL) @destroy;
    if (!n) return 1;
    n->spawn(() => do_work());
    n->spawn(() => do_other_work());
}
// Both tasks complete before this line

// Nested nurseries
{
    CCNursery* outer = @create(NULL) @destroy;
    if (!outer) return 1;
    outer->spawn(() => {
        CCNursery* inner = @create(NULL) @destroy;
        if (!inner) return 1;
        inner->spawn(subtask1());
        inner->spawn(subtask2());
        return 0;
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

// Iterate until closed
{
    CCNursery* producer = @create(NULL) @destroy {
        chan_close(tx);
    };
    if (!producer) return 1;
    producer->spawn([tx]() => {
        for (int i = 0; i < 10; i++) chan_send(tx, i);
    });
    int v;
    while (chan_recv(rx, &v) == 0) {
        printf("%d\n", v);
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
{
    CCArena arena = @create(kilobytes(4)) @destroy;
    if (!arena.base) return 1;
    void* buf = cc_arena_alloc(&arena, 1024, 8);
    char* str = cc_arena_strdup(&arena, "hello");
    // auto-freed at scope exit
}

// Reset arena (reuse memory)
{
    CCArena arena = @create(kilobytes(4)) @destroy;
    if (!arena.base) return 1;
    for (int i = 0; i < 100; i++) {
        void* tmp = cc_arena_alloc(&arena, 64, 8);
        process(tmp);
        cc_arena_reset(&arena);  // reuse for next iteration
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

## "Maybe present" values (optionals are retired)

Concurrent-C no longer has an `Optional<T>` / `T?` surface. For values that may
be absent, pick the shape that matches the operation:

```c
// Nullable pointer — best for container lookups and struct fields.
int* hit = m.get(key);
if (hit) { use(*hit); }

// Bool + out-parameter — best for pop / next iterators.
int out;
if (v.pop(&out)) { use(out); }

// In-band sentinel — best for stream reads (empty slice = EOF).
CCSlice chunk = try buf.next();
if (chunk.len == 0) { /* EOF */ }

// Result — best for fallible operations with a real error channel.
int !>(CCError) lookup(int key);
```

See `cc/include/ccc/DEPRECATIONS.md` for the full migration matrix.

---

## Results (`T!>(E)`)

```c
typedef struct { int code; } MyError;

int!>(MyError) divide(int a, int b) {
    if (b == 0) return cc_err((MyError){.code = 1});
    return cc_ok(a / b);
}

// Usage
int!>(MyError) result = divide(10, 2);
if (cc_is_ok(result)) {
    printf("result: %d\n", cc_unwrap_ok(result));
} else {
    printf("error: %d\n", cc_unwrap_err(result).code);
}

// Propagate errors with try
int!>(MyError) caller(void) {
    int val = try divide(10, 0);  // returns early on error
    return cc_ok(val * 2);
}
```

---

## Closures

```c
CCNursery* n = @create(NULL) @destroy;
if (!n) return 1;

// Lambda syntax
n->spawn(() => printf("hello\n"));

// With captures (value by default)
int x = 42;
n->spawn([x]() => printf("x = %d\n", x));

// Reference capture
n->spawn([&x]() => { x++; });

// Explicit copy capture
n->spawn([=x]() => printf("x = %d\n", x));
```

---

## Common Patterns

### Worker Pool
```c
{
    int[~100 >] jobs_tx;
    int[~100 <] jobs_rx;
    channel_pair(&jobs_tx, &jobs_rx);

    CCNursery* workers = @create(NULL) @destroy;
    if (!workers) return 1;
    for (int i = 0; i < 4; i++) {
        workers->spawn([jobs_rx]() => {
            int job;
            while (chan_recv(jobs_rx, &job) == 0) {
                process(job);
            }
        });
    }

    CCNursery* feeder = @create(workers) @destroy {
        chan_close(jobs_tx);
    };
    if (!feeder) return 1;
    feeder->spawn([jobs_tx]() => {
        for (int j = 0; j < 100; j++) {
            chan_send(jobs_tx, j);
        }
    });
}
```

### Fan-out / Fan-in
```c
{
    int[~16 >] results_tx;
    int[~16 <] results_rx;
    channel_pair(&results_tx, &results_rx);

    CCNursery* workers = @create(NULL) @destroy {
        chan_close(results_tx);
    };
    if (!workers) return 1;
    for (int i = 0; i < 16; i++) {
        workers->spawn([i, results_tx]() => {
            int result = compute(i);
            chan_send(results_tx, result);
        });
    }

    int sum = 0, r;
    while (chan_recv(results_rx, &r) == 0) {
        sum += r;
    }
    printf("total: %d\n", sum);
}
```

### Producer/Consumer Pipeline
```c
{
    int[~10 >] tx;
    int[~10 <] rx;
    channel_pair(&tx, &rx);

    CCNursery* producer = @create(NULL) @destroy {
        chan_close(tx);
    };
    if (!producer) return 1;
    producer->spawn([tx]() => {
        for (int i = 0; i < 100; i++) {
            chan_send(tx, i);
        }
    });

    int v;
    while (chan_recv(rx, &v) == 0) {
        printf("got %d\n", v);
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
#include <ccc/cc_runtime.cch>     // Core runtime (owned nurseries, channels)
#include <ccc/std/prelude.cch>    // Convenience (kilobytes, heap arena, etc.)
#include <ccc/cc_atomic.cch>      // Portable atomics
```
