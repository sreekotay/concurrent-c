# Async vs Spawn: When to Use Which

Concurrent-C has two main concurrency primitives: `@async` functions and `spawn`. They serve different purposes and have dramatically different performance characteristics.

## TL;DR Decision Tree

```
Is your code I/O-bound or channel-heavy?
  └─ YES → Use @async
  └─ NO → Is it CPU-bound and benefits from parallelism?
            └─ YES → Use spawn
            └─ NO → Use @async (it's cheaper)
```

## Performance Comparison

| | `@async` | `spawn` |
|--|---------|---------|
| **Overhead** | ~15M tasks/sec | ~15K tasks/sec |
| **Cost** | State machine (cheap) | Thread pool task (expensive) |
| **Blocking** | Yields at `await` | Blocks OS thread |
| **Parallelism** | Single-threaded execution | Multi-core |

**Key insight:** `@async` is **1000x cheaper** than `spawn` for task creation.

## When to Use `@async`

### ✅ I/O-Bound Operations

```c
@async void fetch_and_process(char* url) {
    char[:] data = await http_get(url);  // Network I/O
    await ch.send(data);                  // Channel operation
}
```

### ✅ Many Concurrent Tasks

```c
// Create 100 async tasks - cheap!
CCTaskIntptr tasks[100];
for (int i = 0; i < 100; i++) {
    tasks[i] = fetch_item(i);
}

// Wait for all
intptr_t results[100];
cc_block_all(100, tasks, results);
```

### ✅ Channel Pipelines

```c
@async void pipeline_stage(int[~ <] in, int[~ >] out) {
    @for await (int x : in) {
        int result = process(x);
        await out.send(result);
    }
}
```

## When to Use `spawn`

### ✅ CPU-Bound Parallelism

```c
// Parallel computation across cores
@nursery {
    spawn(() => { compute_shard(data, 0, n/4); });
    spawn(() => { compute_shard(data, n/4, n/2); });
    spawn(() => { compute_shard(data, n/2, 3*n/4); });
    spawn(() => { compute_shard(data, 3*n/4, n); });
}
```

### ✅ Blocking FFI Calls

```c
// Blocking C library that can't be made async
spawn(() => {
    sqlite3_exec(db, "SELECT ...", callback, NULL, NULL);
});
```

### ✅ Background Workers

```c
// Long-running worker threads
@nursery {
    for (int i = 0; i < NUM_WORKERS; i++) {
        spawn([i]() => {
            worker_loop(i);  // Runs until shutdown
        });
    }
}
```

## Anti-Patterns to Avoid

### ❌ Spawning for Channel Operations

```c
// BAD: 1000 thread pool tasks for I/O
for (int i = 0; i < 1000; i++) {
    spawn([ch, i]() => {
        ch.send(i);  // This should be @async!
    });
}

// GOOD: 1000 async tasks (1000x cheaper)
for (int i = 0; i < 1000; i++) {
    tasks[i] = send_value(ch, i);  // @async function
}
cc_block_all(1000, tasks, results);
```

### ❌ Spawning for Trivial Work

```c
// BAD: Thread pool overhead for trivial computation
spawn(() => { result = a + b; });

// GOOD: Just do it inline
result = a + b;
```

### ❌ `cc_block_on` Inside `spawn`

```c
// BAD: Blocks the thread pool worker, deadlock risk!
spawn(() => {
    int x = cc_block_on(int, async_work());  // ⚠️ Deadlock hazard
});

// GOOD: Keep async work in async context
@async int do_work() {
    int x = await async_work();
    return x;
}
```

## Mixed Patterns

Sometimes you need both. The key is to use the right tool at each level:

```c
// Outer: spawn for parallelism
@nursery {
    spawn(() => {
        // Inner: @async for I/O
        @nursery closing(tx) {
            @async produce(tx, shard_id);  // Channel-heavy work
        }
    });
}
```

## Summary

| Use Case | Primitive |
|----------|-----------|
| Channel operations | `@async` |
| Network/file I/O | `@async` |
| Many concurrent tasks | `@async` + `cc_block_all` |
| CPU-bound parallelism | `spawn` |
| Blocking FFI | `spawn` |
| < 100 concurrent items | Either (perf similar) |
| > 1000 concurrent items | `@async` (required) |
