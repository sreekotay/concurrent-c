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

## 6. Summary of Scaling & Safety Idioms

| Pattern | Complexity | Scalability | Philosophy |
| :--- | :--- | :--- | :--- |
| **Wait Nodes** | High | High | Zero-cost abstraction |
| **Poll Groups** | Medium | High | Explicit orchestration |
| **Nursery Health** | Medium | N/A | Structured safety |
| **Stall Watchdog** | Low | N/A | Performance transparency |

### Recommendation for Phase 1.1
1. Implement **Wait Nodes** as the normative lowering for `@match`.
2. Integrate **Stall Watchdog** into the base fiber scheduler to identify "leaky" synchronous calls.
3. Add **Nursery Health** checks to `cc_nursery_wait` to catch partial deadlocks in complex pipelines.
