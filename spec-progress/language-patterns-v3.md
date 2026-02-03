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

## 2. Proposed Pattern: The "Wait Node" (Go-style)

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

## 3. The "Sharded Broadcast" (The Middle Ground)

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

## 4. Pattern: Explicit Poll Groups

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

## 5. Advanced Deadlock & Stall Detection

To move from global "Smoke Alarms" to precise "Fire Marshalling," we propose nursery-local and yield-aware monitoring.

### 5.1 Local Nursery Health Monitoring
**Problem:** Global detection misses partial deadlocks where some workers are still making progress.
**Pattern:** Nurseries monitor their own children for internal circular dependencies.
- **Mechanism:** When `cc_nursery_wait()` is called, it enters a monitoring state. If all children of that nursery are `PARKED` on channels that the nursery itself is registered to close (`closing(ch)`), it triggers a **Local Deadlock Error**.
- **Diagnostic Philosophy:** This uses the **Skeleton** (the nursery hierarchy) to diagnose a stall in the **Flow** (the channel graph).
- **Implementation Note:** Requires `fiber_task` to store a pointer to its parent nursery. `cc_nursery_wait` can then scan its task list and check the `park_reason` and channel provenance.

### 5.2 The "Synchronous Bridge" Watchdog
**Problem:** Fibers calling legacy C code can block OS threads (e.g., `pthread_mutex_lock`), making the worker unavailable to the scheduler without the runtime knowing.
**Pattern:** Track "Time Since Last Yield" per worker.
- **Mechanism:** Each worker thread updates a `last_yield_ns` timestamp in its core loop. A background watchdog thread periodically scans all workers. If `now - last_yield_ns > 500ms`, it emits a **Stall Warning** with a backtrace of the offending worker.
- **Diagnostic Philosophy:** Provides **Flow-Aware Backtraces**. By including the "Last Channel Op" in the stall report, the developer can see where the communication graph has snagged on a synchronous C call.
- **Implementation Note:** Low overhead—just one atomic store per fiber switch. The watchdog can use `pthread_kill(SIGUSR1)` or similar to force a backtrace dump from the stalled worker.

---

## 6. Pipelined Channels: Seamless Ordered Parallelism

To eliminate the "Task-Handle Crutch" (manually passing `CCTaskPtr` through channels), we introduce **Pipelined Channels**. This pattern allows the **Circulatory System** to natively manage the execution order of tasks.

### The Idiom
```c
// The 'pipelined' modifier tells the compiler this channel manages task handles
CompressedResult*[~16 pipelined] results_ch;

@nursery {
    // 'spawn into' creates a task and pushes its handle into the channel FIFO
    spawn into(results_ch) () => compress(blk);

    // IDIOMATIC RECV:
    // 1. results_ch.recv(&r) returns bool !> (CCIoError)
    // 2. 'await' suspends until the task at the head of the FIFO is DONE
    // 3. '?' propagates any channel or task errors
    CompressedResult* r;
    while (await results_ch.recv(&r)?) {
        write_block(r);
    }
}
```

### The Lowering (Transparent Transformer)
The `pipelined` modifier is a directive for the compiler's lowering phase:
1.  **Storage:** The underlying `CCChan` is allocated to hold `CCTaskPtr` (task handles) instead of raw `T*`.
2.  **Verb (`spawn into`):** Lowers to `cc_spawn` followed immediately by a `chan_send` of the resulting handle.
3.  **Verb (`await recv`):** Lowers to a `chan_recv` of the handle, followed by a **Wait Node** suspension on that specific task's completion.

### Why This Fits CC
- **Type Safety:** The compiler ensures you only `spawn into` a pipelined channel and only `await` its results.
- **Composition:** It fuses **Ordering** (Channel FIFO) with **Parallelism** (Spawn) and **Synchronization** (Await) into a single, non-magical flow.
- **Zero-Cost:** By using the **Wait Node** pattern (Section 2), the `await` on `recv()` is $O(1)$ and avoids global broadcasts.

---

## 7. Unified Spawn Verbs: Fibers vs. Threads

To maintain the **Principle of Orthogonal Concerns**, Concurrent-C provides explicit verbs for choosing the underlying execution mechanism.

### The Idioms
```c
// 1. Fiber Spawn (The Default)
// Lowers to: cc_spawn_fiber()
spawn () => { ... };

// 2. Thread Spawn (The OS Offload)
// Lowers to: cc_spawn_thread()
spawn_thread () => { ... };

// 3. Pipelined Spawn (The Ordered Flow)
// Lowers to: cc_spawn_fiber() + chan_send()
spawn into(results_ch) () => { ... };
```

### The Lowering (Transparent Transformer)
1.  **`spawn`**: Targets the high-performance M:N fiber scheduler. Use for 99% of concurrent logic.
2.  **`spawn_thread`**: Targets a dedicated OS-thread pool. Use for offloading legacy C code, blocking syscalls, or thread-sensitive FFI.
3.  **`spawn into`**: Fuses a spawn with a channel push to preserve execution order in a pipeline.

### Why This Fits CC
- **Explicitness:** The developer chooses the "weight" of the task (`fiber` vs `thread`) based on the nature of the work.
- **Symmetry:** All verbs start with `spawn`, making the "Skeleton" of the program easy to scan.
- **ABI Clarity:** The "naked" C functions (`cc_spawn_fiber` and `cc_spawn_thread`) provide a stable, searchable interface for the generated code.

---

## 8. Summary of Scaling & Safety Idioms

| Pattern | Complexity | Scalability | Philosophy |
| :--- | :--- | :--- | :--- |
| **Wait Nodes** | High | High | Zero-cost abstraction |
| **Poll Groups** | Medium | High | Explicit orchestration |
| **Pipelined Channels** | Medium | High | Seamless composition |
| **Spawn Verbs** | Low | N/A | Explicit mechanism |
| **Nursery Health** | Medium | N/A | Structured safety |
| **Stall Watchdog** | Low | N/A | Performance transparency |

### Recommendation for Phase 1.1
1. Implement **Wait Nodes** as the normative lowering for `@match`.
2. Implement **Pipelined Channels** (`pipelined` modifier + `spawn into`).
3. Unify **Spawn Verbs** by renaming `cc_spawn` to `cc_spawn_thread` and providing the `spawn_thread` keyword.
4. Integrate **Stall Watchdog** and **Nursery Health** checks for structural diagnostics.
