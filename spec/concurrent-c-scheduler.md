# Concurrent-C Fiber Scheduler

## Overview

The Concurrent-C fiber scheduler provides M:N scheduling of user-space fibers onto OS threads. It automatically adapts to both I/O-bound and CPU-bound workloads, achieving pthread-level performance without requiring users to choose between threads and fibers.

## Design Goals

1. **Unified API**: Users write `@async` functions without choosing thread vs fiber
2. **Automatic scaling**: Start lean (1x cores), scale to 2x for CPU-bound work
3. **Zero overhead for I/O**: Work stealing handles yielding tasks efficiently
4. **Pthread parity for CPU-bound**: Match thread performance for compute workloads
5. **Zero user tuning**: Round-robin distribution happens automatically

## Architecture

### Worker Pool

- **Initial workers**: 1x CPU cores (lean start for I/O workloads)
- **Maximum workers**: 2x CPU cores (auto-scale limit via sysmon)
- **Growth rate**: 50% per scale event (exponential growth)

### Queue Structure

```
Global Queue (FIFO, MPMC)
    └── Overflow only - when local queues are full

Local Queues (per-worker, SPMC)
    └── All spawns go here via round-robin
    └── Stealable by other workers

Inbox Queues (per-worker, MPSC)
    └── Cross-worker fiber handoffs
    └── Lower contention than global queue
```

### Automatic Round-Robin Distribution

All `cc_fiber_spawn_task()` calls distribute tasks evenly across workers:

```c
static _Atomic size_t spawn_counter = 0;
size_t target = atomic_fetch_add(&spawn_counter, 1) % num_workers;
push_to_local_queue(target, task);
wake_sleeping_worker();  // Critical: always wake after spawn
```

This ensures even load distribution for CPU-bound batch work without any user intervention.

### Sysmon Thread

Background thread that monitors for CPU-bound work and triggers scaling:

- **Check interval**: 250µs
- **Stuck threshold**: 750K cycles (~250µs) without heartbeat update
- **Scale trigger**: Pending work + stuck workers detected
- **Growth**: Spawn 50% more workers each scale event

## Fiber States and Scheduling

Fiber scheduling uses a single state machine with an explicit parking intermediate:

```
CREATED -> QUEUED -> RUNNING -> PARKING -> PARKED -> QUEUED -> RUNNING -> DONE
                         \                              ^
                          \--- (pending_unpark) -------/
```

### State Definitions

| State | Meaning |
|-------|---------|
| `FIBER_CREATED` | Allocated but not yet enqueued |
| `FIBER_QUEUED` | On a run queue, waiting for a worker |
| `FIBER_RUNNING` | Actively executing on a worker |
| `FIBER_PARKING` | Transitioning to parked (between CAS and yield) |
| `FIBER_PARKED` | Suspended, waiting for unpark |
| `FIBER_DONE` | Completed execution |

### Key Invariants

- A fiber may only be resumed when its state is `QUEUED`.
- Enqueue paths must transition to `QUEUED` exactly once before push (CAS from expected state).
- Stale/duplicate queue entries are dropped when the CAS fails.
- Dequeue paths must CAS `QUEUED -> RUNNING` before `mco_resume`.
- The `PARKING` state is visible to concurrent `unpark` calls; see below.

### The `pending_unpark` Latch

When `cc__fiber_unpark()` is called on a fiber that is `RUNNING` or `QUEUED` (not yet parked), the unpark cannot take effect immediately. Instead, it sets `pending_unpark = 1`.

`cc__fiber_park_if()` checks `pending_unpark` twice:
1. Before the `RUNNING -> PARKING` CAS — if set, bail out immediately.
2. After the CAS (now `PARKING`) — if set, revert to `RUNNING` and bail out.

This ensures no unpark signal is ever lost, regardless of timing.

### `cc__fiber_clear_pending_unpark`

Before entering a multi-channel select (`@match`), the fiber calls `cc__fiber_clear_pending_unpark()` to reset the latch. This prevents a stale `pending_unpark` from a previous channel operation (e.g., a send that completed via direct handoff) from causing the select's `park_if` to skip immediately, which would spin-loop without making progress.

**Invariant:** `pending_unpark` is a *per-park-attempt* signal, not a persistent flag. It must be cleared before any new wait operation that will use `park_if`. Currently this is only needed before select park loops; single-channel send/recv operations use fresh wait nodes whose `notified` field serves the same purpose.

**Audit note:** If a future code path adds a new wait primitive that uses `park_if` across multiple unrelated channels or objects, it must also clear `pending_unpark` before its park loop.

### `cc__fiber_park_if` (Conditional Park)

```c
void cc__fiber_park_if(_Atomic int* flag, int expected, const char* reason, ...);
```

Parks the fiber only if `*flag == expected` at the point of commitment. The sequence is:

1. Check `pending_unpark` — bail if set
2. CAS `RUNNING -> PARKING` — bail if fiber not running
3. Check `pending_unpark` again — bail if set (revert to RUNNING)
4. Check `*flag != expected` — bail if condition changed
5. CAS `PARKING -> PARKED` — commit to park
6. `mco_yield()` — suspend

If `unpark()` fires between steps 4 and 5, it CAS's `PARKING -> QUEUED`, and step 5 finds `state == QUEUED` instead of `PARKING`, so the fiber yields and resumes immediately.

### `cc__fiber_unpark`

Non-blocking. Attempts `PARKED -> QUEUED` or `PARKING -> QUEUED` via CAS. If the fiber is `RUNNING` or `QUEUED`, sets `pending_unpark = 1` as a latch for the next park attempt.

## Fiber Join Synchronization

The scheduler implements a careful handshake protocol for fiber join operations to avoid lost wakeups and deadlocks.

### Join Lock Protocol

Each fiber has a `join_lock` spinlock that synchronizes the completion handshake:

```c
// Fiber completion (in fiber_entry):
join_spinlock_lock(&f->join_lock);
atomic_store(&f->done, 1);           // Mark done
atomic_store(&f->state, FIBER_DONE); // Set state atomically with done
fiber_task* waiter = atomic_exchange(&f->join_waiter_fiber, NULL);
int cv_initialized = atomic_load(&f->join_cv_initialized);
join_spinlock_unlock(&f->join_lock);

// Signal waiters
if (waiter) cc__fiber_unpark(waiter);
if (cv_initialized) {
    pthread_mutex_lock(&f->join_mu);
    pthread_cond_broadcast(&f->join_cv);
    pthread_mutex_unlock(&f->join_mu);
}
```

### Fiber-to-Fiber Join

When a fiber joins another fiber, it uses park/unpark:

```c
join_spinlock_lock(&f->join_lock);
if (atomic_load(&f->done)) {
    // Already done - fast path
    join_spinlock_unlock(&f->join_lock);
    return;
}
// Register as waiter under lock
atomic_store(&f->join_waiter_fiber, current);
join_spinlock_unlock(&f->join_lock);

// Park until unparked by completing fiber
while (!atomic_load(&f->done)) {
    cc__fiber_park_if(&f->done, 0, "join");
}
```

### Thread-to-Fiber Join

When a thread (not a fiber) joins a fiber, it uses a condvar:

```c
join_spinlock_lock(&f->join_lock);
if (atomic_load(&f->done)) {
    join_spinlock_unlock(&f->join_lock);
    return;
}

// Initialize condvar if needed
if (CAS(&f->join_cv_initialized, 0, 1)) {
    pthread_mutex_init(&f->join_mu, NULL);
    pthread_cond_init(&f->join_cv, NULL);
}

// Lock condvar mutex BEFORE releasing join_lock
// This prevents lost wakeups
pthread_mutex_lock(&f->join_mu);
join_spinlock_unlock(&f->join_lock);

while (!atomic_load(&f->done)) {
    pthread_cond_wait(&f->join_cv, &f->join_mu);
}
pthread_mutex_unlock(&f->join_mu);
```

### Wait for Fiber Done State

After seeing `done=1`, joiners must wait for `state=FIBER_DONE` and `running_lock=0`:

```c
static inline void wait_for_fiber_done_state(fiber_task* f) {
    int in_fiber = (tls_current_fiber && tls_current_fiber->coro);
    
    // Spin briefly for state=FIBER_DONE
    for (int i = 0; i < 10000; i++) {
        if (atomic_load(&f->state) == FIBER_DONE) goto state_done;
        cpu_pause();
    }
    
    if (in_fiber) {
        // Can't block in fiber context - proceed anyway
        return;
    }
    
    // Thread context: safe to yield
    while (atomic_load(&f->state) != FIBER_DONE) {
        sched_yield();
    }
    
state_done:
    // Wait for running_lock=0 (worker finished mco_resume)
    for (int i = 0; i < 10000; i++) {
        if (atomic_load(&f->running_lock) == 0) return;
        cpu_pause();
    }
    
    if (in_fiber) return;  // Can't block
    
    while (atomic_load(&f->running_lock) != 0) {
        sched_yield();
    }
}
```

**Critical**: In fiber context, we MUST NOT call `sched_yield()` because it blocks the worker thread while holding `running_lock`, which can deadlock other fibers trying to unpark us.

## Channel Notification Protocol

Channel wait nodes use a typed notification field to distinguish wakeup reasons:

| Value | Constant | Meaning |
|-------|----------|---------|
| 0 | `CC_CHAN_NOTIFY_NONE` | Not yet notified (initial state) |
| 1 | `CC_CHAN_NOTIFY_DATA` | Direct handoff — data written to `node.data` |
| 2 | `CC_CHAN_NOTIFY_CANCEL` | Select: this node lost the race |
| 3 | `CC_CHAN_NOTIFY_CLOSE` | Channel closed while waiting |
| 4 | `CC_CHAN_NOTIFY_SIGNAL` | Buffer state changed — retry the operation |

### Direct Handoff (`DATA`)

A sender can pop a receiver's wait node and write data directly to `node.data` (which points to the receiver's `out_value`). The sender sets `notified = DATA`. The receiver sees `DATA` and returns immediately — no buffer interaction needed.

### Signal (`SIGNAL`)

Used when a sender enqueues to the buffer and signals a waiting receiver to retry. The receiver wakes, attempts a lock-free dequeue, and loops back if it fails.

### Key Invariant: No Gap When Parking

The receiver must be on the channel's wait list at all times when it could park. The pre-park dequeue attempt is structured to maintain this:

1. Take mutex. Node is on the wait list.
2. Check `notified` under lock — handle `DATA`, `SIGNAL`, `CLOSE`.
3. If `notified == 0`: check `lfqueue_count` under lock.
   - `count <= 0`: buffer empty — **stay on wait list**, unlock, park.
   - `count > 0`: remove node under lock, unlock, try dequeue.
4. If dequeue fails (CAS contention): loop back (running, not parking — safe).

This eliminates the "remove → dequeue → re-add" gap that previously allowed `signal_recv_waiter` to find no waiters and lose the wake signal.

## Unbuffered (Rendezvous) Channels

Unbuffered channels have `cap == 0` and require a synchronous sender-receiver handshake. Once a fiber commits to a rendezvous (adds itself to the wait list), it must see that operation through. Nursery cancellation checks are performed only **before** committing, not inside the waiting loop.

### `lfqueue_inflight` Counter

For lock-free buffered channels, an `inflight` counter tracks in-progress enqueue attempts. This prevents the close/drain path from seeing an empty queue and returning `EPIPE` while a sender is between the closed check and the actual CAS enqueue. The drain path spins until `inflight == 0`.

## Unpark Semantics

`cc__fiber_unpark()` is non-blocking and only transitions `PARKED -> QUEUED` (or `PARKING -> QUEUED`) and enqueues the fiber. It does not resume a fiber directly. If the fiber is `RUNNING` or `QUEUED`, it sets `pending_unpark = 1`.

## Multi-Channel Select (@match)

The select implementation uses per-channel wait lists with atomic winner selection:

1. A `select_wait_group` is created with `selected_index = -1`.
2. Wait nodes are registered on each channel involved, each pointing to the group.
3. Any channel activity calls `cc__chan_select_try_win(node)` which CAS's `selected_index` from `-1` to the node's index. Only one node wins.
4. Non-winners are cancelled (`notified = CANCEL`) and signaled so the fiber can re-check.
5. The winning node's channel operation proceeds normally.

This avoids global wakeups and ensures exactly one winner per select round.

## Auto-Scaling Algorithm

```
every 250µs:
    if no pending work in any queue: continue
    
    count stuck workers (no heartbeat update in 750K cycles)
    if no stuck workers: continue
    
    # Exponential growth: add 50% of current capacity
    total = base_workers + temp_workers
    to_spawn = total / 2
    to_spawn = min(to_spawn, MAX_EXTRA - current_extra)
    
    spawn to_spawn replacement workers
```

## Work Stealing

Workers steal from each other's local queues when idle:

1. Check own local queue (fast path)
2. Check inbox queue (cross-worker handoffs)
3. Check global queue
4. Randomized steal from other workers' local queues
5. Spin briefly, then sleep

This enables soft affinity: tasks prefer their assigned worker but can be stolen if that worker is busy.

## Contention Map (Lock-Free vs Contended)

**Lock-free / low contention (hot paths)**:
- Global run queue (MPMC) and local queues (SPMC).
- Inbox queues for cross-worker handoffs.
- Lock-free buffered channel fast path (small elements).

**Intentionally contended / blocking**:
- Channel mutex path (buffered slow path and unbuffered rendezvous).
- Select wait lists (per-channel waiters).
- Join handshake (`join_lock`), and thread join condvar (`join_mu`/`join_cv`).

**Tradeoffs**:
- Lock-free paths favor throughput and low latency.
- Contended paths favor correctness and simplicity under high contention or complex rendezvous.

## Heartbeat Tracking

Each worker updates a heartbeat timestamp once per batch (not per fiber):

```c
// At start of each batch execution
atomic_store(&worker_heartbeat[id], rdtsc());
```

Sysmon uses heartbeats to detect stuck workers without adding per-fiber overhead.

## Performance Results

Benchmarked with `pigz` parallel compression (50MB file, 8 requested workers):

| Workload | pigz_cc (fibers) | pthread | Ratio |
|----------|------------------|---------|-------|
| Compression | ~200 MB/s | ~208 MB/s | **96%** |
| Decompression | ~617 MB/s | ~545 MB/s | **113%** |

**Key findings:**
- Fibers match pthread for CPU-bound compression (96%)
- Fibers beat pthread for I/O-heavy decompression (113%)
- **Zero user tuning required** - just use `cc_fiber_spawn_task()`
- Round-robin distribution + wake-on-spawn eliminates scheduling delays

## Usage

### Simple - Just Spawn

```c
// All workloads - scheduler handles everything
for (int i = 0; i < num_blocks; i++) {
    tasks[i] = cc_fiber_spawn_task(compress_block, blocks[i]);
}
```

The scheduler automatically:
1. Distributes tasks via round-robin to local queues
2. Wakes sleeping workers immediately
3. Scales workers if CPU-bound work detected
4. Work-steals to balance load

### @async Functions

```c
@async Response* handle_request(Request* req) {
    Data* data = await fetch_from_db(req->id);  // yields, work-stolen
    return format_response(data);               // CPU work, distributed
}
```

Both I/O-bound and CPU-bound work handled optimally with the same API.

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `CC_WORKERS=N` | Override initial worker count |
| `CC_FIBER_STATS=1` | Print scheduler statistics at exit |
| `CC_VERBOSE=1` | Print scheduler initialization info |

### Compile-Time Defines

| Define | Default | Description |
|--------|---------|-------------|
| `CC_FIBER_WORKERS` | 0 (auto-detect) | Fixed worker count |
| `CC_FIBER_STACK_SIZE` | 2MB (vmem) / 128KB | Per-fiber stack size |
| `CC_FIBER_QUEUE_SIZE` | 65536 | Slots in global/local queues |

### Debug Flags

#### Environment Variables

| Variable | Description |
|----------|-------------|
| `CC_DEBUG_JOIN=1` | Log fiber join/unpark operations to stderr |
| `CC_DEBUG_WAKE=1` | Log wake counter increments |
| `CC_DEBUG_INBOX=1` | Log inbox queue operations |
| `CC_DEBUG_INBOX_DUMP=1` | Dump inbox contents on deadlock |
| `CC_CHAN_DEBUG=1` | Channel operation logs (enqueue/dequeue/signal/close) |
| `CC_CHAN_DEBUG_VERBOSE=1` | Detailed select/wake logging |
| `CC_CHAN_NO_LOCKFREE=1` | Force mutex-based channel path (isolate lock-free bugs) |
| `CC_DEBUG_DEADLOCK_RUNTIME=1` | Parked fiber dump on deadlock detection |
| `CC_DEADLOCK_ABORT=0` | Continue after deadlock (for log capture) |
| `CC_PARK_DEBUG=1` | Log `park_if` decisions (skip reasons) |

#### Compile-Time Debug Flags

| Flag | Description |
|------|-------------|
| `-DCC_DEBUG_DEADLOCK` | Enable detailed fiber dump on deadlock detection. Shows parked fibers with park reasons and wait targets. |
| `-DCC_DEBUG_SYSMON` | Log sysmon scaling decisions to stderr |
| `-DCC_DEBUG_FIBER` | Log fiber park/unpark edge cases |

Example deadlock output with `CC_DEBUG_DEADLOCK_RUNTIME=1`:
```
╔══════════════════════════════════════════════════════════════╗
║                     DEADLOCK DETECTED                        ║
╚══════════════════════════════════════════════════════════════╝

Runtime state:
  Workers: 8 total (8 base, 0 temp), 8 unavailable
  Fibers:  2 parked, 4 completed, 6 spawned total

Parked fibers (waiting for unpark that will never come):
  [fiber 1] chan_send: buffer full, waiting for space at channel.c:2355 (obj=0x...)
  [chan 0x...] cap=8 count=8 closed=0 send_waiters=1 recv_waiters=1
  [fiber 2] chan_recv: buffer empty, waiting for data at channel.c:2798 (obj=0x...)
```

## How It Works

### The Key Insights

1. **Round-robin distribution**: Spread spawns evenly across workers, don't pile onto one queue
2. **Always wake**: After every spawn, wake a sleeping worker (critical for latency!)
3. **Auto-scale to 2x**: Sysmon detects CPU-bound work and scales up 50% at a time
4. **Work stealing**: Automatic load balancing when distribution isn't perfect
5. **Join lock protocol**: Atomic done+state updates prevent lost wakeups
6. **No gap when parking**: Channel wait nodes stay on the list through the park decision

### Why Round-Robin + Wake Wins

| Approach | Problem |
|----------|---------|
| Global queue only | All spawns contend, uneven pickup |
| Push to own queue | Spawner's queue overflows |
| Local queue, no wake | Workers sleep while tasks wait! |
| **Round-robin + wake** | Even distribution, immediate processing |

### Avoiding Deadlocks in Join

| Issue | Solution |
|-------|----------|
| Lost wakeup (done before wait) | Check done under join_lock before parking |
| Condvar race | Lock join_mu before releasing join_lock |
| Fiber blocking worker | Never sched_yield() in fiber context |
| State visibility | Set done+state atomically under join_lock |

### Avoiding Deadlocks in Channels

| Issue | Solution |
|-------|----------|
| Lost wake (node off list when sender signals) | Check count under mutex; stay on list when parking |
| Direct handoff overwrite | Check `notified == DATA` before any dequeue |
| Unbuffered nursery cancel | Only check cancellation before committing to rendezvous |
| Close during inflight enqueue | `lfqueue_inflight` counter gates drain |
| Spurious CAS failure in dequeue | Loop retry; never park after a failed dequeue |

## Implementation Files

- `cc/runtime/fiber_sched.c`: Core scheduler implementation
- `cc/runtime/channel.c`: Channel operations (buffered, unbuffered, lock-free)
- `cc/runtime/nursery.c`: Structured concurrency nurseries
- `cc/runtime/task.c`: CCTask API and spawn functions
- `cc/include/ccc/cc_sched.cch`: Public API declarations
