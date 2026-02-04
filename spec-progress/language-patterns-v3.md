# Concurrent-C Language Patterns v3: Scaling the Runtime

This document addresses the "Global Broadcast" bottleneck in the `@match` (select) implementation and proposes architectural patterns for high-scale concurrency and advanced deadlock detection, rooted in the **Principle of Orthogonal Concerns**.

## 1. The Philosophy: Skeleton vs. Circulatory System

As codified in the core language spec, Concurrent-C distinguishes between the **Shape** of a program and its **Flow**:

*   **The Skeleton (Structure):** Nurseries and Arenas are hierarchical. They define the **Ownership Tree**. Their lifetimes are lexical and enforced by the compiler.
*   **The Circulatory System (Flow):** Channels are a graph. They define the **Communication Topology**. Their lifetimes are dynamic and can "cross-cut" the ownership tree.

The patterns below ensure the runtime can scale the "Flow" while using the "Skeleton" to provide precise diagnostics when the two systems conflict.

---

## 2. The "@match" Scaling Problem

### The Current Prototype (v2)
Currently, `@match` uses a single global broadcast condition variable (`g_chan_broadcast_cv`). 
- **Pros:** Deadlock-free, simple to implement, low memory overhead.
- **Cons:** "Thundering Herd" effect. Every channel operation wakes every fiber waiting on *any* `@match` block. CPU usage scales $O(N \times M)$ where $N$ is the number of active channels and $M$ is the number of waiting fibers.

---

## 3. Proposed Pattern: The "Wait Node" (Go-style)

To achieve $O(1)$ wakeups, we must move the waiting state from a global variable to the specific channels involved in the operation.

### The Mechanism
1. **The Winner Flag:** Each `@match` execution creates a single `_Atomic int winner = -1` on its stack.
2. **Registration:** The fiber locks all involved channels (in address order to prevent deadlock) and attaches a `WaitNode` to each.
3. **WaitNode Structure:**
   ```c
   typedef struct {
       cc__fiber* fiber;
       _Atomic int* winner_flag;
       int case_index;
   } CCSelectWaitNode;
   ```
4. **The Race:** The first channel to become ready attempts to `atomic_compare_exchange` the `winner_flag` from `-1` to its own index.
   - **Success:** This channel "won" the select. It unparks the fiber.
   - **Failure:** Another channel already won. This channel ignores the waiter.

### Why This Fits CC
- **Explicit Lowering:** The address-ordered locking and flag management can be part of the specified lowering in the language spec.
- **Zero-Cost:** If a channel is already ready, no nodes are ever attached (the "fast path").

---

## 4. The "Sharded Broadcast" (The Middle Ground)

If the Wait Node pattern is too complex for immediate implementation, **Sharding** provides a massive scalability boost with minimal code changes.

### The Syntax (Internal)
```c
// Shard the broadcast by channel address
pthread_cond_t g_chan_shards[128];
#define GET_SHARD(ch) (((uintptr_t)(ch) >> 6) % 128)
```
- **Pattern:** When a channel is modified, it only signals `g_chan_shards[GET_SHARD(ch)]`.
- **Result:** Reduces spurious wakeups by 128x.

---

## 5. Pattern: Explicit Poll Groups

For systems with thousands of channels (e.g., a web server), we can introduce **Poll Groups** to give the developer control over the broadcast scope.

### The Idiom
```c
// Group related channels to isolate their wakeups
CCPollGroup pg = cc_poll_group_create();
cc_chan_set_poll_group(rx1, pg);
cc_chan_set_poll_group(rx2, pg);

@match(pg) { 
    case rx1.recv(): ...
    case rx2.recv(): ...
}
```
- **Philosophy:** Matches the CC goal of **Local Reasoning**. The developer explicitly defines which channels "belong together" for multiplexing purposes.

---

## 6. Advanced Deadlock & Stall Detection

To move from global "Smoke Alarms" to precise "Fire Marshalling," we propose nursery-local and yield-aware monitoring.

### 6.1 Local Nursery Health Monitoring
**Problem:** Global detection misses partial deadlocks where some workers are still making progress.
**Pattern:** Nurseries monitor their own children for internal circular dependencies.
- **Mechanism:** When `cc_nursery_wait()` is called, it enters a monitoring state. If all children of that nursery are `PARKED` on channels that the nursery itself is registered to close (`closing(ch)`), it triggers a **Local Deadlock Error**.
- **Diagnostic Philosophy:** This uses the **Skeleton** (the nursery hierarchy) to diagnose a stall in the **Flow** (the channel graph).
- **Implementation Note:** Requires `fiber_task` to store a pointer to its parent nursery. `cc_nursery_wait` can then scan its task list and check the `park_reason` and channel provenance.

### 6.2 The "Synchronous Bridge" Watchdog
**Problem:** Fibers calling legacy C code can block OS threads (e.g., `pthread_mutex_lock`), making the worker unavailable to the scheduler without the runtime knowing.
**Pattern:** Track "Time Since Last Yield" per worker.
- **Mechanism:** Each worker thread updates a `last_yield_ns` timestamp in its core loop. A background watchdog thread periodically scans all workers. If `now - last_yield_ns > 500ms`, it emits a **Stall Warning** with a backtrace of the offending worker.
- **Diagnostic Philosophy:** Provides **Flow-Aware Backtraces**. By including the "Last Channel Op" in the stall report, the developer can see where the communication graph has snagged on a synchronous C call.
- **Implementation Note:** Low overhead—just one atomic store per fiber switch. The watchdog can use `pthread_kill(SIGUSR1)` or similar to force a backtrace dump from the stalled worker.

---

## 7. Sending Tasks to Channels

Any channel can receive tasks (closures) in addition to values. Use `chan_send_task` to spawn work and queue its result. The `ordered` flag on the channel controls whether results arrive in submission order or completion order.

### The Problem: Manual Task-Handle Plumbing

Consider a compression pipeline. The manual pattern requires:

```c
// 1. Wrapper function to convert Result to void*
void* compress_block_task(void* arg) {
    Block* blk = (Block*)arg;
    CompressedResult* !>(CCIoError) res = compress_block(...);
    if (cc_is_err(res)) return NULL;  // Error information lost!
    return cc_unwrap_as(res, CompressedResult*);
}

// 2. Channel of task handles (not results)
CCTask[~16 >] tasks_tx;
CCTask[~16 <] tasks_rx;
channel_pair(&tasks_tx, &tasks_rx);

// 3. Producer: spawn task, get handle, send handle
CCTask task = cc_fiber_spawn_task(compress_block_task, blk);
chan_send(tasks_tx, task);

// 4. Consumer: receive handle, await completion, null-check
CCTask task;
while (chan_recv(tasks_rx, &task)) {
    void* result = (void*)cc_block_on_intptr(task);
    if (!result) { /* error - but which error? */ }
    CompressedResult* r = (CompressedResult*)result;
    write_block(r);
}
```

Problems:
1. **Boilerplate:** ~40 lines for a simple producer-consumer flow
2. **Error Erasure:** The wrapper converts `T!>(E)` to `void*`, losing type info
3. **Manual Coordination:** Must remember spawn-then-send and recv-then-await

### The Solution: `chan_send_task`

```c
// Channel of results (not task handles)
CompressedResult*[~16 ordered >] results_tx;
CompressedResult*[~16 ordered <] results_rx;
channel_pair(&results_tx, &results_rx);

// Producer: send a task (spawns immediately, async)
chan_send_task(results_tx, () => compress_block(blk));

// Consumer: recv gets the result (awaits internally)
CompressedResult* r;
while (chan_recv(results_rx, &r)) {
    write_block(r);
}
```

### How It Works

1. **`chan_send_task(ch, () => expr)`** spawns a fiber immediately to execute the closure
2. The task handle is queued internally in the channel
3. **`chan_recv(ch, &result)`** receives the next task handle, awaits it, and extracts the result

The `ordered` flag controls result delivery:

| Channel Declaration | Task Execution | Result Delivery |
| :--- | :--- | :--- |
| `T[~N]` | Immediate, async | Completion order (first done, first out) |
| `T[~N ordered]` | Immediate, async | Submission order (FIFO) |

Both execute tasks immediately. The difference is purely in `recv` sequencing.

### Fan-In and Fan-Out Semantics

The `ordered` flag uses simple FIFO semantics based on send order:

**Fan-In (N producers → 1 consumer):**
```c
// Producer A sends A1, A2, A3
// Producer B sends B1, B2, B3
// Channel sees interleaved stream based on send timing: [A1, B1, A2, B2, A3, B3]
// Consumer receives in that order (each producer's results are FIFO)
```

- Per-sender FIFO is naturally preserved (each sender's sends complete in order)
- Inter-sender ordering is determined by the natural race (when sends complete)
- No artificial serialization - producers remain fully concurrent

**Fan-Out (1 producer → N consumers):**
```c
// Producer sends 1, 2, 3, 4, 5
// Consumer A might get 1, 3, 5
// Consumer B might get 2, 4
// Each consumer sees their subset in submission order
```

- Global submission order is maintained
- Each consumer receives items in the order they were submitted
- Which consumer gets which item depends on scheduling

**Key insight:** The channel is simply a FIFO queue of task handles. "Ordered" means `recv` awaits tasks in queue order rather than completion order. The queue order is determined by when `chan_send_task` calls complete.

### Mixing Values and Tasks

The same channel can receive both values and tasks:

```c
int[~16 >] tx;
int[~16 <] rx;
channel_pair(&tx, &rx);

chan_send(tx, 42);                      // send a value
chan_send_task(tx, () => compute());    // send a task

int x;
chan_recv(rx, &x);  // gets 42
chan_recv(rx, &x);  // gets compute() result (awaited internally)
```

### Error Propagation

When the closure returns `T !>(E)`, errors flow through:

```c
// Task returns Result type
chan_send_task(results_tx, () => {
    CompressedResult* !>(CCIoError) res = compress_block(blk);
    return res;  // Error preserved!
});

// Consumer can propagate errors with ?
CompressedResult* r;
while (chan_recv(results_rx, &r)?) {
    use(r);
}
```

`chan_recv` returns `bool !>(E)`:
- `Ok(true)` — value received
- `Ok(false)` — channel closed (EOF)
- `Err(e)` — task failed with error `e`

### The Lowering

**`chan_send_task`:**

```c
// User writes:
chan_send_task(tx, () => compress_block(blk));

// Compiler generates:
{
    CCTask task = cc_fiber_spawn_task(__wrapper, captures);
    cc_chan_send(tx.raw, &task, sizeof(task));
}

void* __wrapper(void* arg) {
    __captures_t* c = (__captures_t*)arg;
    typedef struct { T __result; } __result_t;
    __result_t* r = (__result_t*)cc_task_result_ptr(sizeof(__result_t));
    r->__result = compress_block(c->blk);
    return r;
}
```

**`chan_recv` (when channel has pending tasks):**

```c
// User writes:
T r;
chan_recv(rx, &r);

// Compiler generates (simplified):
CCTask task;
cc_chan_recv(rx.raw, &task, sizeof(task));
void* storage = (void*)cc_block_on_intptr(task);
r = *(T*)storage;  // __result at offset 0
```

For `ordered` channels, the runtime ensures tasks are awaited in submission order.

### Backpressure

Standard channel semantics apply:
- `chan_send_task` spawns immediately
- If channel is full, blocks until space available
- Provides parallelism up to channel capacity, then natural backpressure

### Full Example

**Before:** ~50 lines of manual plumbing

**After:**

```c
CompressedResult*[~16 ordered >] results_tx;
CompressedResult*[~16 ordered <] results_rx;
channel_pair(&results_tx, &results_rx);

@nursery {
    // Consumer
    spawn(() => [results_rx] {
        CompressedResult* r;
        while (chan_recv(results_rx, &r)?) {
            cc_file_write(out, r->data)?;
            cc_heap_arena_free(&r->arena);
        }
    });
    
    // Producer
    @nursery closing(results_tx) {
        while (read_block(&blk)?) {
            chan_send_task(results_tx, () => [blk] compress_block(blk));
        }
    }
}
```

### Why This Fits CC

- **Explicit:** `chan_send_task` clearly indicates task spawning (vs `chan_send` for values)
- **No Magic:** Compiler detects the API, not closure types
- **Error Preservation:** Task errors flow through Result types unchanged
- **Orthogonal:** `ordered` flag is independent—works with values or tasks
- **Local Reasoning:** Channel declaration shows ordering; send/recv sites are uniform

---

## 8. Unified Spawn Verbs: Fibers vs. Threads

To maintain the **Principle of Orthogonal Concerns**, Concurrent-C provides explicit verbs for choosing the underlying execution mechanism.

### The Idioms
```c
// 1. Fiber Spawn (The Default)
// Lowers to: cc_fiber_spawn_task()
spawn () => { ... };

// 2. Thread Spawn (The OS Offload)
// Lowers to: cc_thread_spawn()
spawn_thread () => { ... };

// 3. Channel Task Send (Parallel Pipeline)
// Lowers to: cc_fiber_spawn_task() + cc_chan_send()
chan_send_task(ch, () => { ... });
```

### The Lowering (Transparent Transformer)
1.  **`spawn`**: Targets the high-performance M:N fiber scheduler. Use for 99% of concurrent logic.
2.  **`spawn_thread`**: Targets a dedicated OS-thread pool. Use for offloading legacy C code, blocking syscalls, or thread-sensitive FFI.
3.  **`chan_send_task`**: Spawns a fiber task and queues its handle in the channel. `chan_recv` awaits internally.

### Why This Fits CC
- **Explicitness:** The developer chooses the "weight" of the task (`fiber` vs `thread`) based on the nature of the work.
- **Explicit Task Send:** `chan_send_task` clearly indicates spawning—no compiler magic to detect closures.
- **ABI Clarity:** The "naked" C functions (`cc_fiber_spawn_task` and `cc_thread_spawn`) provide a stable, searchable interface for the generated code.

---

## 9. Closure Syntax (v3)

Concurrent-C closures use a syntax designed to be unambiguous in all contexts, especially as function arguments.

### The Syntax

```c
// No captures, no params
() => expr
() => { body }

// With parameters (up to 2)
(x) => expr
(int x, int y) => { body }

// With explicit captures (after the arrow)
() => [x, y] expr
() => [x, &y] { body }
(a) => [x] a + x

// Single-param shorthand (no parens)
x => x * 2
```

### Capture Syntax

Captures are specified in `[...]` **after** the `=>` arrow:

| Syntax | Meaning |
| :--- | :--- |
| `[x]` | Capture `x` by value (copy) |
| `[&x]` | Capture `x` by reference (read-only) |
| `[x, &y]` | Mixed: `x` by value, `y` by reference |

**Important:** Capture-all forms (`[&]` and `[=]`) are **not allowed**. All captures must be explicit.

### Why Captures After Arrow?

The original syntax `[captures](params) => body` was ambiguous when closures appeared as function arguments:

```c
// AMBIGUOUS: Is [x] array subscript or capture list?
foo(bar[x](y) => z);  // Error: TCC parses [x] as subscript
```

By placing captures after `=>`, the syntax is unambiguous:

```c
// UNAMBIGUOUS: () => clearly starts a closure
foo(bar, (y) => [x] z);  // OK: captures after arrow
```

### Reference Capture Safety

Reference captures (`[&x]`) are checked for mutation at compile time:

```c
int shared = 0;
spawn(() => [&shared] {
    shared++;  // ERROR: mutation of shared reference
});
```

**Safe alternatives:**
- Use `@atomic int shared` for atomic operations
- Use `Mutex<int>` for synchronized access
- Use `@unsafe [&x]` to bypass checks (expert mode)

---

## 10. Summary of Scaling & Safety Idioms

| Pattern | Complexity | Scalability | Philosophy |
| :--- | :--- | :--- | :--- |
| **Wait Nodes** | High | High | Zero-cost abstraction |
| **Poll Groups** | Medium | High | Explicit orchestration |
| **`chan_send_task`** | Low | High | Explicit task spawning |
| **`ordered` flag** | Low | N/A | FIFO result delivery |
| **Spawn Verbs** | Low | N/A | Explicit mechanism |
| **Nursery Health** | Medium | N/A | Structured safety |
| **Stall Watchdog** | Low | N/A | Performance transparency |

### Recommendation for Phase 1.1
1. Implement **Wait Nodes** as the normative lowering for `@match`.
2. Implement **`chan_send_task`** (spawn + send) and `ordered` flag for FIFO recv.
3. Unify **Spawn Verbs** using `cc_thread_spawn` for OS threads and `cc_fiber_spawn_task` for fibers.
4. Integrate **Stall Watchdog** and **Nursery Health** checks for structural diagnostics.
