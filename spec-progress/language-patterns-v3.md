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

## 7. Ordered Channels: Seamless Ordered Parallelism

To eliminate the "Task-Handle Crutch" (manually passing `CCTask` through channels), we introduce **Ordered Channels**. This pattern allows the **Circulatory System** to natively manage execution order while preserving Result-based error propagation.

### The Problem: Manual Task-Handle Plumbing

Consider the compression pipeline in `pigz_cc.ccs`. The current pattern requires:

```c
// 1. Wrapper function to convert Result to void*
void* compress_block_task(void* arg) {
    Block* blk = (Block*)arg;
    CompressedResultPtr !>(CCIoError) res = compress_block(...);
    if (cc_is_err(res)) return NULL;  // Error information lost!
    return cc_unwrap_as(res, CompressedResult*);
}

// 2. Channel of task handles (not results)
CCTask[~chan_cap >] tasks_tx;
CCTask[~chan_cap <] tasks_rx;
CCChan* tasks_ch = channel_pair(&tasks_tx, &tasks_rx);

// 3. Producer: spawn task, get handle, send handle
CCTask task = cc_fiber_spawn_task(compress_block_task, blk);
chan_send(tasks_tx, task);

// 4. Consumer: receive handle, await completion, null-check for errors
CCTask task;
while (cc_io_avail(chan_recv(tasks_rx, &task))) {
    void* result = (void*)cc_block_on_intptr(task);
    if (!result) { /* error - but which error? */ }
    CompressedResult* r = (CompressedResult*)result;
    write_block(r);
}
```

This pattern has three problems:
1. **Boilerplate:** ~40 lines of plumbing for a simple producer-consumer flow.
2. **Error Erasure:** The wrapper converts `T!>(E)` to `void*`, losing error type information.
3. **Manual Coordination:** The developer must remember to spawn-then-send and recv-then-await.

### The Idiom: Ordered Channels

The `ordered` modifier eliminates this ceremony:

```c
// Declaration: tx/rx split - only rx can be 'ordered'
CompressedResult*[~16 >] results_tx;
CompressedResult*[~16 ordered <] results_rx;
CCChan* results = channel_pair(&results_tx, &results_rx);  // pair enforces compatibility

@nursery closing(results_tx) {
    // Producer: 'spawn into' fuses spawn + send handle
    while (read_block(&blk)?) {
        spawn into(results_tx)? () => compress_block(blk);  // ? for send error
    }
}

// Consumer: 'await recv' fuses recv + await + error propagation
CompressedResult* r;
while (await results_rx.recv(&r)?) {
    write_block(r);
}
```

### Error Propagation Semantics

The `await ch.recv(&r)` operation returns `bool !> (E)` with three outcomes:

| Return Value | Meaning | Action |
| :--- | :--- | :--- |
| `Ok(true)` | Value received | `r` is valid, continue loop |
| `Ok(false)` | Channel closed | Normal EOF, exit loop |
| `Err(e)` | Task failed | Propagate via `?` operator |

This matches CC's existing I/O patterns (e.g., `cc_io_avail()`), where `bool !> (E)` cleanly separates "done" from "error."

### The Lowering (Transparent Transformer)

The `ordered` modifier is a directive for the compiler's lowering phase:

**Storage:**
```c
// User declares:
T*[~cap >] results_tx;
T*[~cap ordered <] results_rx;  // only rx has 'ordered' modifier
CCChan* results = channel_pair(&results_tx, &results_rx);

// Compiler generates:
// channel_pair validates that tx is paired with an ordered rx
// The channel stores CCTask handles internally, not T* values
CCChan* results = cc_chan_create(cap, sizeof(CCTask));
```

The `channel_pair` call enforces that `spawn into` can only be used on a tx end that is paired with an `ordered` rx. The `ordered` modifier on rx enables `await recv` semantics.

**Type Validation:**

The `channel_pair(&tx, &rx)` call, where `rx` is declared with `ordered`, propagates type information to `tx` at compile time. At each `spawn into(tx)` site, the compiler validates that the closure returns `T*` or `T* !>(E)`, matching the rx's declared element type.

**Verb (`spawn into`):**

When the spawned closure returns `T* !> (E)`, the compiler generates a wrapper that uses fiber-local storage for the result:

```c
// User writes:
spawn into(results_tx)? () => compress_block(blk);

// Compiler generates:
{
    // Pass captures directly as arg (no allocation needed)
    CCTask task = cc_fiber_spawn_task(__ordered_wrapper, blk);
    if (!cc_io_avail(chan_send(results_tx, &task))) {
        cc_task_free(&task);
        // ? propagates send error
    }
}

// Generated wrapper uses fiber-local storage:
void* __ordered_wrapper(void* arg) {
    Block* blk = (Block*)arg;
    
    // Result struct stored in fiber-local buffer (no malloc)
    // __result is ALWAYS at offset 0 for multi-producer support
    typedef struct {
        T* !>(E) __result;  // offset 0 - fixed location
        Block* blk;         // captures (optional, for debugging)
    } __captures_t;
    
    __captures_t* cap = (__captures_t*)cc_task_result_ptr(sizeof(__captures_t));
    cap->__result = compress_block(blk);
    return cap;
}
```

**Verb (`await recv`):**

The recv lowering retrieves the Result from offset 0, without needing to know the full captures struct layout. This enables multiple producers with different captures:

```c
// User writes:
T* r;
bool !>(E) ok = await results_rx.recv(&r);

// Compiler generates:
CCTask task;
if (!cc_io_avail(cc_chan_recv(results_rx, &task, sizeof(task)))) {
    return cc_ok(false);  // Channel closed
}

// Await task and retrieve result from fiber-local storage
void* cap = (void*)cc_block_on_intptr(task);

// IMPORTANT: Copy result immediately - fiber may be reused after this point
T* !>(E) task_result = *(T* !>(E)*)cap;  // __result is at offset 0

// Use standard Result machinery
if (cc_is_err(task_result)) {
    return cc_err(cc_unwrap_err(task_result));  // Propagate task error
}
*r = cc_unwrap(task_result);
return cc_ok(true);
```

**Result Storage Lifetime:**

The result is stored in fiber-local storage (48 bytes, via `cc_task_result_ptr`). This storage is valid until `cc_block_on_intptr` returns, after which the fiber may be reused. The generated `await recv` code copies the Result value immediately, so the consumer only sees the extracted `T*` value. This eliminates malloc/free overhead (~77% performance improvement in benchmarks).

**Backpressure:**

Backpressure follows standard CC channel semantics: `spawn into` spawns the fiber first, then blocks on the internal `chan_send` if the channel is full. This provides parallelism up to the channel capacity, then backpressure naturally limits further spawning.

**Send Error Propagation (PROPOSAL):**

When the channel is closed (e.g., consumer cancelled), the internal `chan_send` fails. The proposed syntax places `?` after the channel to clarify what can fail:

```c
spawn into(results_tx)? () => compress_block(blk);
//                    ^ send error propagates here (channel closed)
```

This distinguishes send errors (propagated via `?`) from task errors (propagated through the channel to the consumer's `await recv`).

### Cancellation Semantics

Cancellation in ordered channels uses standard CC channel error propagation—no special mechanism is needed:

**Consumer cancels (stops receiving):**
```c
// Consumer encounters error and closes rx end
if (cc_is_err(write_result)) {
    cc_chan_rx_close_err(results, EIO);  // signal upstream
    return cc_err(write_result);
}
```

The producer's next `spawn into(results_tx)` will fail when the internal `chan_send` returns a channel error. The `?` operator propagates this naturally:

```c
// Producer side - channel error propagates via ?
spawn into(results_tx)? () => compress_block(blk);  // send fails, error propagates
```

**Producer cancels (stops sending):**

When the producer's nursery exits (via `closing(results_tx)`), the channel closes normally. The consumer's `await results_rx.recv(&r)` returns `Ok(false)`, exiting the loop cleanly.

**In-flight tasks on cancellation:**

Tasks already spawned continue to completion, but their results are discarded when `chan_send` fails. The spawning nursery still waits for all its children before exiting, maintaining structured concurrency guarantees.

### Full Example: pigz Compression Pipeline

**Before (manual task-handle pattern):** ~50 lines

```c
// Wrapper function (error erasure)
void* compress_block_task(void* arg) {
    Block* blk = (Block*)arg;
    CompressedResultPtr !>(CCIoError) res = compress_block(strm, blk, ...);
    return_input_arena(blk->arena, blk->pool);
    if (cc_is_err(res)) return NULL;
    return cc_unwrap_as(res, CompressedResult*);
}

// Pipeline setup
CCTask[~chan_cap >] tasks_tx;
CCTask[~chan_cap <] tasks_rx;
CCChan* tasks_ch = channel_pair(&tasks_tx, &tasks_rx);

@nursery {
    // Writer task
    spawn([tasks_rx]() => {
        CCTask task;
        while (cc_io_avail(chan_recv(tasks_rx, &task))) {
            void* result = (void*)cc_block_on_intptr(task);
            if (!result) { write_failed = true; break; }
            CompressedResult* r = (CompressedResult*)result;
            cc_file_write(out, r->data);
            cc_heap_arena_free(&r->arena);
        }
    });
    
    // Reader task
    @nursery closing(tasks_tx) {
        spawn([tasks_tx]() => {
            while (read_block(&blk)) {
                CCTask task = cc_fiber_spawn_task(compress_block_task, blk);
                chan_send(tasks_tx, task);
            }
        });
    }
}
```

**After (ordered channels):** ~20 lines

```c
CompressedResult*[~chan_cap >] results_tx;
CompressedResult*[~chan_cap ordered <] results_rx;
CCChan* results = channel_pair(&results_tx, &results_rx);

@nursery {
    // Writer task (consumer)
    spawn([results_rx]() => {
        CompressedResult* r;
        while (await results_rx.recv(&r)?) {
            cc_file_write(out, r->data)?;
            cc_heap_arena_free(&r->arena);
        }
    });
    
    // Reader task (producer)
    @nursery closing(results_tx) {
        spawn([results_tx]() => {
            while (read_block(&blk)?) {
                spawn into(results_tx)? () => compress_block(blk);
            }
        });
    }
}
```

### Why This Fits CC

- **Type Safety:** The compiler ensures you only `spawn into` a tx paired with an ordered rx, and only `await recv` on an ordered rx. Misuse is a compile error.
- **Error Preservation:** Task errors flow through the Result type system instead of being erased to `NULL`.
- **Multi-Producer Support:** Since `ordered` is on rx only, multiple producers can `spawn into` the same tx. Results arrive in submission order (channel FIFO), enabling work distribution patterns.
- **Composition:** Fuses **Ordering** (Channel FIFO) with **Parallelism** (Spawn) and **Synchronization** (Await) into a single, non-magical flow.
- **Zero-Cost:** By using the **Wait Node** pattern (Section 3), the `await` on `recv()` is $O(1)$ and avoids global broadcasts.
- **Local Reasoning:** The `ordered` modifier is visible at the rx declaration, making the channel's semantics clear without tracing through code.

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

// 3. Ordered Spawn (The Ordered Flow)
// Lowers to: cc_fiber_spawn_task() + chan_send()
spawn into(results_tx)? () => { ... };  // ? for send error (PROPOSAL)
```

### The Lowering (Transparent Transformer)
1.  **`spawn`**: Targets the high-performance M:N fiber scheduler. Use for 99% of concurrent logic.
2.  **`spawn_thread`**: Targets a dedicated OS-thread pool. Use for offloading legacy C code, blocking syscalls, or thread-sensitive FFI.
3.  **`spawn into`**: Fuses a spawn with a channel push to preserve execution order in a pipeline.

### Why This Fits CC
- **Explicitness:** The developer chooses the "weight" of the task (`fiber` vs `thread`) based on the nature of the work.
- **Symmetry:** All verbs start with `spawn`, making the "Skeleton" of the program easy to scan.
- **ABI Clarity:** The "naked" C functions (`cc_fiber_spawn_task` and `cc_thread_spawn`) provide a stable, searchable interface for the generated code.

---

## 9. Summary of Scaling & Safety Idioms

| Pattern | Complexity | Scalability | Philosophy |
| :--- | :--- | :--- | :--- |
| **Wait Nodes** | High | High | Zero-cost abstraction |
| **Poll Groups** | Medium | High | Explicit orchestration |
| **Ordered Channels** | Medium | High | Seamless composition |
| **Spawn Verbs** | Low | N/A | Explicit mechanism |
| **Nursery Health** | Medium | N/A | Structured safety |
| **Stall Watchdog** | Low | N/A | Performance transparency |

### Recommendation for Phase 1.1
1. Implement **Wait Nodes** as the normative lowering for `@match`.
2. Implement **Ordered Channels** (`ordered` modifier + `spawn into` + `await recv`).
3. Unify **Spawn Verbs** using `cc_thread_spawn` for OS threads and `cc_fiber_spawn_task` for fibers.
4. Integrate **Stall Watchdog** and **Nursery Health** checks for structural diagnostics.
