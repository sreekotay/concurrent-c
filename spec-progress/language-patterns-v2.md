# Concurrent-C Language Patterns v2

This document captures patterns that emerged from real-world use (pigz_cc) and extensive design discussion.

## Principles

Before adding anything:
1. **Can we delete something instead?** Fewer concepts are better.
2. **Can we generalize an existing construct?** Prefer extending over adding.
3. **Is this actually common?** Only add patterns that appear repeatedly.
4. **Does it compose well?** New features should work with existing ones.

---

## The Big Insight: Channels Are The Primitive

After extensive discussion, we arrived at a key insight:

**Channels don't own items. They're communication primitives.**

When you need ownership semantics (pools, resource management), you need the `owned` modifier.

### Channel vs Owned Channel (Pool)

| Aspect | Channel | Owned Channel (Pool) |
|--------|---------|----------------------|
| Ownership | Transfers (sender → receiver) | Retained (pool owns, user borrows) |
| Item flow | One-way (pass through) | Circular (borrow/return) |
| Cleanup | Consumer's problem | Pool handles automatically |
| RAII | Not needed | Required (create/destroy/reset) |
| API | send/recv | send/recv (same!) |

**The key distinction is provenance:** Channels pass items through. Pools own items.

---

## 1. Owned Channels (Pools)

### The Syntax

```c
// Plain channel - no ownership
T[~N >] tx;
T[~N <] rx;

// Bidirectional channel - no ownership, both send and recv
T[~N] ch;

// Owned channel (Pool) - RAII with lifecycle closures
T[~N owned {
    .create = [captures]() => create_expr,
    .destroy = [](item) => destroy_expr,
    .reset = [](item) => reset_expr
}] pool;
```

### Syntax Breakdown

| Part | Syntax | Precedent |
|------|--------|-----------|
| Channel type | `T[~N]` | Existing CC |
| Direction modifier | `>`, `<` | Existing CC |
| Bidirectional | `T[~N]` (no direction) | New |
| Ownership modifier | `owned` | New (like `>`, `<`) |
| Config block | `{ ... }` | C struct init |
| Named callbacks | `.field = value` | C designated init |
| Closures | `[captures](params) => body` | Existing CC (spawn) |

### Full Example (pigz arena pool)

```c
size_t arena_size = block_size + BLOCK_ARENA_OVERHEAD + 65536;

CCArena[~(chan_cap + num_workers) owned {
    .create = [arena_size]() => cc_heap_arena(arena_size),
    .destroy = [](a) => cc_heap_arena_free(&a),
    .reset = [](a) => cc_arena_reset(&a)
}] blk_arena_pool;

@nursery {
    // Worker
    spawn([blk_arena_pool, ...]() => {
        Block* blk;
        while (chan_recv(blocks_rx, &blk) == 0) {
            // ... process block ...
            
            // Return arena to pool (auto-reset via .reset)
            chan_send(blk_arena_pool, blk->arena);
        }
    });

    // Reader
    spawn([blk_arena_pool, ...]() => {
        while (!hit_eof) {
            // Borrow arena (creates if empty via .create)
            CCArena arena;
            chan_recv(blk_arena_pool, &arena);
            
            // ... use arena, build block ...
            
            chan_send(blocks_tx, blk);
        }
    });
}
// blk_arena_pool destroyed here - remaining arenas freed via .destroy
```

### Lifecycle Semantics

| Event | Callback | When |
|-------|----------|------|
| `chan_recv` on empty pool | `.create` | Lazily create new item |
| `chan_send` to pool | `.reset` | Reset item before re-pooling |
| Pool goes out of scope | `.destroy` | Called on each remaining item |
| `chan_send` when full | `.destroy` | Drop excess, call destroy |

### Why This Design

1. **Same verbs** - `chan_send`/`chan_recv` work for both channels and pools
2. **Ownership at declaration** - closures bound to pool declaration (structural)
3. **CC closures** - same syntax as spawn, captures work naturally
4. **C-style config** - `{}` and `.field =` match existing CC/C patterns
5. **RAII is automatic** - cleanup on scope exit, no manual drain

---

## 2. Channel Policies (Logging, Metrics, Lossy Channels)

### The Problem

Different use cases need different backpressure behavior:

```c
// From stdlib logging spec
void log_drop(LogEvent evt);      // Never blocks, drops if full
void log_block(LogEvent evt, Duration timeout);  // Blocks with timeout  
void log_sample(LogEvent evt, f32 rate);  // Probabilistic
```

These are all **send policies** on a channel!

### The Syntax

```c
// Default channel - blocks when full
T[~N] ch;

// Lossy channel - drops if full (for logging, metrics)
T[~N drop] ch;

// Ring buffer - evicts oldest when full
T[~N evict] ch;

// With timeout
T[~N timeout(5s)] ch;
```

### Logging Example

```c
// Log channel - drops if slow, never blocks request path
LogEvent[~1000 drop] log_ch;

// In request handler (latency-sensitive)
chan_send(log_ch, event);  // Never blocks - drops if full

// Log writer task
spawn([]() => {
    LogEvent evt;
    while (chan_recv(log_ch, &evt)?) {
        write_to_file(evt);
    }
});
```

### Metrics Ring Buffer Example

```c
// Keep latest N metrics, evict oldest when full
Metric[~100 evict] metrics_ch;

// Producer - always succeeds, evicts old data
chan_send(metrics_ch, metric);  // Never blocks

// Consumer - gets latest data
while (chan_recv(metrics_ch, &m)?) {
    aggregate(m);
}
```

### Policy Summary

| Modifier | On send when full | Use case |
|----------|-------------------|----------|
| (default) | Block | Work queues, pipelines |
| `drop` | Drop newest | Logging, best-effort |
| `evict` | Drop oldest | Metrics, ring buffers |
| `timeout(d)` | Fail after duration | Request handling |

---

## 3. Unified I/O Pattern: `bool !>(E)`

All I/O (channels, files, network) uses the same three-state result:

```c
bool !>(E) got = read_something(source, &output);
```

| Return | Meaning |
|--------|---------|
| `cc_ok(true)` | Got data |
| `cc_ok(false)` | EOF / closed |
| `cc_err(e)` | Error |

**One loop pattern everywhere:**
```c
while (true) {
    T item;
    if (!chan_recv(rx, &item)?) break;  // ? propagates error, ! detects EOF
    process(item);
}
```

---

## 4. Pipeline Error Propagation

### The Problem

In parallel pipelines, error handling is fundamentally different from sequential code:

```
Reader ──► [blocks] ──► Workers ──► [results] ──► Writer
```

**When Worker 2 hits an error:**
- Reader doesn't know → keeps sending → blocks forever
- Writer doesn't know → keeps waiting → blocks forever
- Other workers don't know → partial/corrupted output
- Result: **deadlock or corruption**

Sequential `?` propagates errors **up the call stack** (vertical).
Pipelines need errors to propagate **across tasks** (horizontal).

### Current Solution (manual, error-prone)

```c
// Global atomic flag
_Atomic int g_pipeline_error = 0;

// Worker hits error:
cc_atomic_store(&g_pipeline_error, 1);
// Must manually drain to avoid deadlock:
while (chan_recv(blocks_rx, &blk) == 0) {
    cc_heap_arena_free(&blk->arena);
}

// Every component must poll:
if (cc_atomic_load(&g_pipeline_error)) break;
```

### The Solution: Bidirectional Error Close

**Key insight:** Allow closing **either end** of a channel with error.

```c
chan_close_err(tx, e);  // Error propagates downstream (recv gets error)
chan_close_err(rx, e);  // Error propagates upstream (send gets error!) ← NEW
```

### API Changes

| Operation | Current | New |
|-----------|---------|-----|
| `chan_send()` | `void` | `bool !>(E)` |
| `chan_recv()` | `bool !>(E)` | (no change) |
| `chan_close_err(tx, e)` | downstream | (no change) |
| `chan_close_err(rx, e)` | N/A | **upstream!** |

### How It Works

```c
// Worker hits error - propagate BOTH directions:
chan_close_err(results_tx, e);  // → Writer sees error on recv
chan_close_err(blocks_rx, e);   // → Reader sees error on send!
return;  // Clean exit, no manual drain needed!
```

**Reader (upstream):**
```c
spawn([]() => {
    while (true) {
        Block* b = read_block()?;
        chan_send(blocks_tx, b)?;  // Returns error if rx was error-closed!
    }
});
```

**Writer (downstream):**
```c
spawn([]() => {
    while (chan_recv(results_rx, &r)?) {  // Returns error from close_err!
        write(r)?;
    }
});
```

### What Happens on Error Close

When `chan_close_err(endpoint, e)` is called:
1. Channel enters error state with error `e`
2. All pending operations return `cc_err(e)`
3. All future operations return `cc_err(e)`
4. Blocked tasks unblock immediately with error

### Full Pipeline Example

```c
@nursery {
    // Reader
    spawn([blocks_tx]() => {
        while (true) {
            Block* b = read_block()?;
            chan_send(blocks_tx, b)?;  // Fails if worker error-closed blocks_rx
        }
        chan_close(blocks_tx);
    });
    
    // Workers
    for (int w = 0; w < N; w++) {
        spawn([blocks_rx, results_tx]() => {
            Block* blk;
            while (chan_recv(blocks_rx, &blk)?) {
                Result r = process(blk);
                if (cc_is_err(r)) {
                    // Propagate error both directions!
                    chan_close_err(results_tx, cc_unwrap_err(r));
                    chan_close_err(blocks_rx, cc_unwrap_err(r));
                    return;
                }
                chan_send(results_tx, cc_unwrap(r))?;
            }
        });
    }
    
    // Writer
    spawn([results_rx]() => {
        Result r;
        while (chan_recv(results_rx, &r)?) {
            write(r)?;
        }
    });
}
```

**`?` just works** - errors propagate through channel operations in both directions.

### Why This Is Elegant

| Problem | Solution |
|---------|----------|
| Reader doesn't know | `chan_send()` returns error |
| Writer doesn't know | `chan_recv()` returns error |
| Need to drain manually | Channel handles it |
| Global atomic flag | Gone! |
| `?` doesn't compose | Now it does! |

---

## 5. Ordered Parallel Output

### The Default Idiom: Pooled State + Task Handles

**The pattern:** Pool expensive state. Spawn async tasks that borrow/return. Await in FIFO.

```c
// Pool expensive state (z_stream, GPU context, connection, etc.)
z_stream[~num_workers owned {
    .create = []() => { z_stream s; deflateInit2(&s, ...); return s; },
    .destroy = [](s) => deflateEnd(&s),
    .reset = [](s) => deflateReset(&s)
}] strm_pool;

// Task handles channel (preserves order)
CCTask[~WINDOW >] tasks_tx;
CCTask[~WINDOW <] tasks_rx;
channel_pair(&tasks_tx, &tasks_rx);

// @async function borrows state, does work, returns state
@async CompressedResult* compress_async(Block* blk) {
    z_stream strm;
    chan_recv(strm_pool, &strm);  // Borrow
    @defer chan_send(strm_pool, strm);  // Return (auto-reset)
    
    return compress(&strm, blk);
}

// Producer: spawn async tasks, send handles (FIFO order)
spawn([tasks_tx]() => {
    while (read_block(&block)?) {
        CCTask task = compress_async(block);
        chan_send(tasks_tx, task)?;
    }
    chan_close(tasks_tx);
});

// Consumer: await in FIFO order (NO REORDER BUFFER!)
spawn([tasks_rx]() => {
    CCTask task;
    while (chan_recv(tasks_rx, &task)?) {
        CompressedResult* r = await task?;  // Blocks until THIS task done
        write(r)?;
    }
});
```

### Why This Preserves Order

```
Reader:     task0 ──► task1 ──► task2 ──► task3
               │         │         │         │
Channel:    [t0, t1, t2, t3] (FIFO)
               │
Scheduler:  runs in any order (t2 might finish before t0!)
               
Writer:     await t0 ──► await t1 ──► await t2 ──► await t3
            (blocks)     (maybe ready) (blocks)   (ready)
```

**Order comes from await sequence, not completion sequence.**

### The Overhead

| Cost | Amount | Notes |
|------|--------|-------|
| State borrow | ~100ns/op | Channel recv |
| State return | ~100ns/op | Channel send |
| Total per item | ~200ns | vs 1ms+ work = 0.02% |

**Trade-off:** Small overhead (~200ns/item) eliminates reorder buffer entirely.

### When To Use Manual Reorder Instead

- Extreme latency sensitivity (200ns matters)
- State can't be pooled (thread affinity, large state)
- Already have worker pool for other reasons

### The Philosophy

> *Pool expensive state. Use task handles for ordering. `await` in FIFO.*

This works for: compression, video encoding, graphics pipelines, any stateful + ordered work.

---

## 6. Summary: What We're Adding

### Language Additions

| Addition | Type | Description |
|----------|------|-------------|
| `T[~N]` bidirectional | Syntax | Channel with no direction (both send/recv) |
| `owned { ... }` | Modifier | Ownership semantics with lifecycle closures |
| `drop`, `evict`, `timeout(d)` | Modifiers | Channel backpressure policies |
| `chan_send()` returns `bool !>(E)` | API | Send can fail (error-closed channel) |
| `chan_close_err(rx, e)` | API | Close recv end with error (upstream propagation) |
| Rename `CCTaskIntptr` → `CCTask` | Naming | Clean up implementation-leaky name |
| `await` outside @async | Language | Typed await anywhere |
| `bool !>(E)` I/O pattern | API | Unified EOF/error handling |

### What We're NOT Adding

| Idea | Why Not |
|------|---------|
| `CCPool` type | Owned channel covers this |
| `CCReorderBuf` | Task-handles-in-channel pattern |
| Separate pool verbs | Same `chan_send`/`chan_recv` work |
| Function pointer callbacks | Closures are structural (CC way) |

### The Philosophy

- **One primitive (channel)** with modifiers for different behaviors
- **Ownership is opt-in** via `owned` modifier
- **Closures are structural** - bound to declaration, captures explicit
- **Same verbs everywhere** - `chan_send`/`chan_recv`
- **Policies at declaration** - behavior is channel property, not per-operation

---

## 7. Full Example: pigz_cc with All Patterns

```c
int compress_file(...) {
    size_t blk_arena_size = block_size + OVERHEAD;
    
    // Work channels (normal - blocking)
    Block*[~chan_cap >] blocks_tx;
    Block*[~chan_cap <] blocks_rx;
    CompressedResult*[~chan_cap >] results_tx;
    CompressedResult*[~chan_cap <] results_rx;
    CCChan* blocks_ch = channel_pair(&blocks_tx, &blocks_rx);
    CCChan* results_ch = channel_pair(&results_tx, &results_rx);
    @defer cc_chan_free(blocks_ch);
    @defer cc_chan_free(results_ch);
    
    // Arena pool (owned - RAII)
    CCArena[~(chan_cap + num_workers) owned {
        .create = [blk_arena_size]() => cc_heap_arena(blk_arena_size),
        .destroy = [](a) => cc_heap_arena_free(&a),
        .reset = [](a) => cc_arena_reset(&a)
    }] arena_pool;
    
    // Stats channel (lossy - never block compression)
    CompressStats[~100 drop] stats_ch;
    
    @nursery {
        // Reader
        spawn([arena_pool, blocks_tx]() => {
            while (!eof) {
                CCArena arena;
                chan_recv(arena_pool, &arena);  // Borrow (creates if needed)
                Block* blk = read_into_arena(&arena)?;
                chan_send(blocks_tx, blk)?;  // Fails if worker error-closed!
            }
            chan_close(blocks_tx);
        });
        
        // Workers (stateful - reorder buffer still needed downstream)
        for (int w = 0; w < num_workers; w++) {
            spawn([arena_pool, blocks_rx, results_tx, stats_ch]() => {
                z_stream strm;
                deflateInit2(&strm, ...);
                @defer deflateEnd(&strm);
                
                Block* blk;
                while (chan_recv(blocks_rx, &blk)?) {
                    CompressedResult* r = compress(&strm, blk);
                    if (cc_is_err(r)) {
                        // Propagate error both directions!
                        chan_close_err(results_tx, cc_unwrap_err(r));
                        chan_close_err(blocks_rx, cc_unwrap_err(r));
                        return;
                    }
                    
                    chan_send(results_tx, cc_unwrap(r))?;
                    chan_send(stats_ch, (CompressStats){...});  // Best-effort
                    chan_send(arena_pool, blk->arena);  // Return arena
                }
            });
        }
        
        // Writer (still needs reorder buffer for stateful workers)
        spawn([results_rx, arena_pool]() => {
            CompressedResult* r;
            while (chan_recv(results_rx, &r)?) {
                // ... reorder buffer logic ...
                write(r)?;
                chan_send(arena_pool, r->arena);  // Return arena
            }
        });
        
        // Stats collector (best-effort)
        spawn([stats_ch]() => {
            CompressStats s;
            while (chan_recv(stats_ch, &s)?) {
                update_dashboard(s);
            }
        });
    }
    // arena_pool cleanup automatic (destroy remaining)
}
```

---

## 8. Remaining Gaps

| Gap | Status | Notes |
|-----|--------|-------|
| Extreme latency (200ns matters) | Accept | Manual reorder buffer is correct for this edge case |
| Unpoolable state | Accept | Thread-affine or very large state needs manual handling |

**Closed gaps:**
- Stateful + ordered → Pooled state + task handles (0.02% overhead, no reorder buffer)

---

## 9. Implementation Notes

### @async vs spawn for Pool-Heavy Code

**The spec already handles sync ops in @async** via automatic `run_blocking` wrapping. So this works:

```c
@async Result* work() {
    chan_recv(sync_pool, &item);  // Auto-wrapped in run_blocking
    // ...
}
```

**But there's overhead:** Each sync op becomes a thread pool dispatch.

**For pool-heavy workloads, prefer spawn:**

```c
spawn([]() => {
    chan_recv(sync_pool, &item);  // Direct, no dispatch overhead
    // ...
});
```

**TODO:** Consider lint to hint when sync ops in @async might be better as spawn.

### Owned Channels: Sync by Default?

Open question: Should `owned` channels be implicitly sync? Mental model would be simpler:
- "Pools are spawn-land, use blocking ops"
- "@async is for cooperative non-blocking work"

This would make it a compile error to use owned pool in @async without explicit wrap.

---

## 10. Benchmark Results

### Task-Handle Pattern vs Worker Pool (pigz, 100MB enwik8)

| Approach | Time | z_stream inits | Reorder buffer |
|----------|------|----------------|----------------|
| **Worker pool + reorder** | 0.53-0.99s | 8 (per worker) | Yes (50+ lines) |
| **Task-handle + FIFO await** | 0.45-0.51s | 763 (per block!) | No |

**Result:** Task-handle pattern is **faster and simpler** despite 95x more z_stream initializations.

**Why:** Simpler pipeline, better load balancing, no reorder buffer overhead.

**Implication:** With pooled state (`owned` channels), the task-handle pattern would be even faster - same simplicity with amortized init cost.

---

## Next Steps

1. **Implement `owned` modifier** - Core language change for pools
2. **Implement channel policies** - `drop`, `evict`, `timeout`
3. **Implement bidirectional error close** - `chan_close_err(rx, e)` + `chan_send()` returns `bool !>(E)`
4. **Write smoke tests** - Pool pattern, drop policy, error propagation
5. **Update pigz_cc** - Apply new patterns, measure improvement
