# Concurrent-C Fiber Scheduler (Virtual Cores)

Minimal, complete spec for the fiber scheduler state machine, ownership rules, and park/unpark protocol.

## Terms / Goals

- **Fiber**: a reusable stack + context that repeatedly executes assigned thunks.
- **Worker**: an OS thread that runs fibers.
- **Virtual core**: a fiber treated as a reusable execution slot.
- **Correctness goals**: no lost wakeups, no double-schedule, no concurrent use of a stack.

## Fiber lifecycle (persistent)

Fibers are reused, not destroyed. Each fiber repeatedly:

1. finishes a thunk (signal joiners/channels),
2. becomes available for reuse,
3. waits until another thunk is assigned,
4. runs the thunk.

If the system permits unbounded growth, reuse is best-effort; otherwise impose a global cap (out of scope here).

## Fiber stacks (defaults and growth)

- Fibers SHOULD use a large virtual stack reservation with demand paging (low resident memory, avoids tiny fixed stacks).
- The implementation MUST use a guard page (or equivalent) so fiber stack overflow fails deterministically (trap/crash), not via silent corruption.
- Stack size MAY be configurable (compile-time and/or env override) but should not require user tuning for common code.
- Guidance: avoid unbounded recursion and large stack allocations inside fiber code; use heap/arena for large buffers.

## Control word (state + lease)

Each fiber has `_Atomic int64_t control`:

- `IDLE (0)`: available for assignment (not currently executing on its stack).
- `ASSIGNED (-1)`: exclusive metadata lock while publishing runnable work for this fiber.
- `PARKED (-2)`: the fiber is blocked; its stack is quiescent and safe to resume.
- `DONE (-4)`: the fiber has completed; its stack is quiescent. Set by the trampoline after `mco_resume` returns so joiners know it is safe to reclaim the fiber. Fibers in the idle pool may retain `DONE`; `fiber_alloc` transitions them back to `IDLE` before reuse.
- `OWNED (>0)`: the fiber stack is currently leased to worker `id` (1-indexed).

### Invariants

- **Single stack owner**: if `control == OWNED(x)`, only worker `x` may execute on that stack.
- **No reuse while running**: a fiber may be put in an idle pool only when it is not executing on that stack.
- **No double-schedule**: a fiber is enqueued at most once per wake/spawn.

### Memory ordering

The runtime MUST use C11 atomics with at least the following orderings:

- **Wait-node notification**: waker `store_release(node.notified, reason)`; waiter `load_acquire(node.notified)`.
- **`pending_unpark` latch**: set with `store_release`; checked with `load_acquire`; cleared with `exchange_acq_rel`.
- **Ownership/state words** (e.g. `control`): transitions via `CAS_acq_rel` on success and `acquire` on failure; publishing a state other threads act on uses `store_release`; observing uses `load_acquire`.
- **Run-queue / inbox publish**: enqueue is `release`, dequeue is `acquire` (or an equivalent HB guarantee).
- **Diagnostics/counters**: `relaxed` only; must not be used for correctness.

Dekker rule: if the implementation uses a store→read pair across `control`/state and `pending_unpark` in the park/unpark critical path (two-variable handshake), that store and subsequent read (load or exchange) MUST be `seq_cst` (or separated by a `seq_cst` fence). Alternatively, fold the latch into a single atomic word to avoid cross-variable ordering.

`seq_cst` is permitted, but not required.

## Heartbeats / diagnostics

Each fiber may maintain a diagnostic `_Atomic uint64_t last_transition` updated on scheduler transitions. It is **diagnostic-only** and must not be used for correctness.

## Run queues and distribution

- Each worker has a **local run queue** (stealable).
- Each worker has an **inbox** (MPSC) for remote pushes and targeted wakeups.
- A **global run queue** holds overflow and cross-worker fibers.
- Idle workers may steal from other workers' local run queues; inboxes are not stolen from.

### Global run queue: ring + overflow list

The global run queue uses a two-tier design:

1. **Lock-free MPMC ring buffer** (`CC_FIBER_QUEUE_INITIAL` slots, default 4096): fast path for common workloads. Push and pop are lock-free CAS loops.
2. **Mutex-protected overflow linked list**: when the ring is full, `fq_push_blocking` appends to a singly-linked list (via `fiber_task.next`) under a mutex. Pop drains the overflow list when the ring is empty.

This design keeps the common case lock-free while handling arbitrary spawn bursts without blocking the caller. `fq_push_blocking` **never blocks** — it always succeeds by falling back to the overflow list.

Fairness rule: workers MUST periodically yield to a global/fair scheduling point (e.g. check the global queue) after a bounded amount of local-only work, to prevent starvation under contention.

Distribution rule: new runnable work that is not a targeted wakeup SHOULD be distributed round-robin across workers (or an equivalent balancing policy such as locality-first + work stealing). Targeted wakeups use affinity (below), with the starvation escape hatch.

### Worker main loop (priority order)

1. Check own local queue
2. Check inbox queue (cross-worker handoffs)
3. Check global queue
4. Randomized steal from other workers' local queues
5. Spin briefly, then sleep

### Spawn (assignment)

Spawner acquires an `IDLE` fiber (local idle stack fast-path). Then:

1. `CAS(f->control, IDLE, ASSIGNED)` to lock metadata.
2. Write runnable metadata (thunk `fn`, `arg`, etc.).
3. Publish to a worker by pushing `f` to that worker's inbox or local queue.
4. The target worker dequeues `f` and claims execution by `CAS(f->control, ASSIGNED, OWNED(worker_id))`, then runs the thunk.
   - If the CAS fails, the dequeued entry is stale and MUST be discarded.

If no `IDLE` fiber is available, the runtime MUST take a non-blocking slow path (e.g. allocate a new fiber / pull from a global pool). It must not block a worker thread waiting for an idle fiber.

## Parking and unparking (no lost wakeups)

Blocking in this runtime is expressed as:

- a **wait object** that owns a wait queue (e.g. a channel),
- a **wait node** (stack-local or intrusive) that contains:
  - `fiber`: the fiber to resume
  - `notified`: `_Atomic int` (0 means "still waiting"; non-zero means "woken with reason")
- the scheduler primitive `park_if(&notified, 0)` which commits to parking only if the flag is still 0.

The scheduler also has an internal `pending_unpark` latch per fiber: if an unpark races with a park attempt, the latch forces `park_if` to return without sleeping (so the signal is not lost). Spurious wakes are allowed; waiters must handle `notified==0`.

### `park_if(flag, expected)` contract

`park_if(&flag, expected)` is the only scheduler primitive required by channel-style waits. It must satisfy:

- **Safety**: if a waker performs `store(flag, value!=expected)` and then calls `unpark(fiber)`, the parked fiber must eventually observe `flag != expected` without missing the wakeup.
- **Idempotence**: `park_if` may return spuriously (without `flag` changing); callers must re-check conditions under their own lock.

Minimal required behavior (yield-before-commit):

1. If `exchange_acq_rel(pending_unpark, 0)` returns non-zero, return immediately (do not sleep).
2. If `load(flag) != expected`, return immediately.
3. Yield to a scheduler/trampoline stack (the fiber stack becomes quiescent).
4. On the scheduler/trampoline stack, attempt to commit to parking:
   - re-check `pending_unpark`; if set, abort and re-enqueue.
   - re-check `flag`; if changed, abort and re-enqueue.
   - publish the parked state: `CAS(control, OWNED, PARKED)`.
   - post-commit Dekker check: read `pending_unpark` with `seq_cst`; if non-zero, abort parking (`CAS(PARKED→ASSIGNED)` then enqueue). If the CAS fails, a concurrent unpark already made it runnable; no further action needed.
   - post-commit flag check: if `load(flag) != expected`, abort parking.
5. If still parked, run another runnable fiber. When this fiber is later made runnable, it resumes and returns to the caller.

This yield-before-commit ordering is REQUIRED (it ensures `PARKED` is published only when the fiber stack is already quiescent, preventing double-resume).

Callers typically wrap the parking sequence in a loop, re-checking the blocking condition under the wait object's lock after each return from `park_if`.

### `pending_unpark` latch

When `cc__fiber_unpark()` is called on a fiber that is `OWNED` or `ASSIGNED` (not yet parked), the unpark cannot take effect immediately. Instead, it sets `pending_unpark = 1`.

`park_if` checks `pending_unpark` before yielding, and the trampoline rechecks after publishing `PARKED`. This ensures no unpark signal is ever lost, regardless of timing.

`pending_unpark` is a *per-park-attempt* signal, not a persistent flag. It must be cleared (`cc__fiber_clear_pending_unpark()`) before any new multi-object wait (e.g. select/`@match`) to prevent a stale signal from a prior operation from causing spin-loops.

### Parking (fiber -> parked)

Parking is a 2-phase commit: publish the wait node before releasing the stack.

Commit point: the wait is committed once the wait node is **enqueued on the wait list under the wait object's lock**.

1. Under the wait object's lock:
   - initialize the wait node (`notified = 0`, `fiber = current`)
   - enqueue the node in the appropriate waiter queue
   - re-check the condition; if satisfied, dequeue the node and do not park.
2. Release the wait object's lock.
3. Call `park_if(&node.notified, 0)` (may return spuriously).
4. Reacquire the wait object's lock and observe `node.notified`:
   - if still 0, dequeue the node (it was a spurious wake / pending-unpark consumption)
   - otherwise, treat the non-zero value as the wake reason (DATA/CLOSE/CANCEL/SIGNAL).

### Unparking (someone else -> runnable)

Unparking is "set flag, then unpark":

1. Under the wait object's lock, pick a waiting node and remove it from the queue.
2. Store the wake reason into `node.notified` (release).
3. Call `unpark(node.fiber)`.

Required rule: store `node.notified` **before** calling `unpark`, otherwise a woken fiber can observe `notified==0` and immediately re-park (lost wakeup).

### `unpark(fiber)` contract

`unpark(fiber)` must be **idempotent** and must not lose wakeups:

- If the target fiber is sleeping/parked, `unpark` must make it runnable.
- If the target fiber is in the parking transition (between yield and the park commit), `unpark` must still make it runnable (by setting/retaining `pending_unpark` so the commit aborts and enqueues).
- If the target fiber is running / not parkable, `unpark` must set or retain the fiber's `pending_unpark` latch so the next `park_if` returns without sleeping.

### Lock depth, deferred wakes, and hazards

Some runtimes defer wakeups while holding internal scheduler locks; any `wake_batch`/deferred wake list MUST flush only when lock depth returns to **0**. Flushing while depth > 0 risks re-entrancy, lock inversion, or waking fibers that then try to acquire the same lock.

Parking while lock depth > 0 is a design hazard and must be treated as a bug. If a fiber parks while holding a scheduler lock, it can deadlock the system.

Diagnostics should flag:
- `WAKE-FLUSH-UNDER-LOCK`: flushing deferred wakes with lock depth > 0.
- `PARK-UNDER-LOCK`: attempting to park with lock depth > 0.

For both cases, capture the lock stack (or current lock owner chain) to aid postmortem analysis.

Additional lock-gap findings:
- Under-lock flush/park events were traced to channel send/recv/close paths calling wake/park while `ch->mu` depth was non-zero.
- Raw `pthread_mutex_unlock(&ch->mu)` in channel code can leave lock depth skewed and trigger park-under-lock even when the mutex is technically released.
- Select/match cleanup across multiple channels can leave nested lock depth > 1; wake deferral must flush only when depth returns to 0.
- Condvar waits (`pthread_cond_wait/timedwait`) release and reacquire `ch->mu`; wrappers must adjust lock depth around the wait to avoid false under-lock detection and missed flushes.

Select-style groups require an additional rule: update the group's "winner chosen" state **before** waking any member.

Select cleanup rule: after a select wakes, the fiber must **deregister/cancel all losing wait nodes** from their respective wait lists before returning to user code.

## Fiber join synchronization

Joiners observe completion by `control == CTRL_DONE`.

- **Fiber completion**: the worker trampoline publishes `CTRL_DONE` after `mco_resume` returns. `fiber_entry` signals the single fiber waiter (if present) and broadcasts the condvar.
- **Fiber-to-fiber join**: register as `join_waiter_fiber` (CAS), then park until `control == CTRL_DONE`.
- **Thread-to-fiber join**: wait on per-fiber condvar (`join_mu`/`join_cv`) until `control == CTRL_DONE`.

## Channels (scheduler contract)

This section describes how channel operations block/wake; it does not specify channel buffering semantics in detail.

Non-blocking channel fast paths (buffer not full/empty, direct handoff, etc.) do not involve the scheduler; only blocking paths use `park_if`/`unpark`.

### Common rules (all async channel waits)

- **Fiber context rule (required)**: if the caller is a fiber, channel operations must never block the OS thread. Blocking is implemented by parking the fiber.
- **Wait node**: starts with `_Atomic notified = 0` and is enqueued under the channel lock.
- **Park primitive**: the waiter releases the channel lock and calls `park_if(&node.notified, 0)`.
- **Wake primitive**: the waker sets `node.notified` to a non-zero reason **then** calls `unpark(node.fiber)`.
- **Spurious wake**: if the waiter returns and `notified==0`, it must remove its node from the wait list before retrying.

### Channel notification values

| Value | Constant | Meaning |
|-------|----------|---------|
| 0 | `CC_CHAN_NOTIFY_NONE` | Not yet notified (initial state) |
| 1 | `CC_CHAN_NOTIFY_DATA` | Direct handoff — data written to `node.data` |
| 2 | `CC_CHAN_NOTIFY_CANCEL` | Select: this node lost the race |
| 3 | `CC_CHAN_NOTIFY_CLOSE` | Channel closed while waiting |
| 4 | `CC_CHAN_NOTIFY_SIGNAL` | Buffer state changed — retry the operation |

### Variant differences (only what matters to the scheduler)

| Channel kind | Blocks when | Wake reason / action |
|---|---|---|
| **sync** (any cap) | full/empty/partner | condvar signal/broadcast (thread ctx); park fiber (fiber ctx) |
| **async buffered** (cap>0) | send: full, recv: empty | set `notified=DATA` then `unpark` |
| **async rendezvous** (cap==0) | until a partner exists | set `notified=DATA` then `unpark` |
| **ordered** | same as buffered/unbuffered | DATA wakes the recv; payload is a `CCTask` handle (FIFO) |

### No-gap invariant when parking

The receiver must be on the channel's wait list at all times when it could park:

1. Take mutex. Node is on the wait list.
2. Check `notified` under lock — handle `DATA`, `SIGNAL`, `CLOSE`.
3. If `notified == 0`: check `lfqueue_count` under lock.
   - `count <= 0`: buffer empty — **stay on wait list**, unlock, park.
   - `count > 0`: remove node under lock, unlock, try dequeue.
4. If dequeue fails (CAS contention): loop retry (running, not parking — safe).

### `lfqueue_inflight` counter

For lock-free buffered channels, an `inflight` counter tracks in-progress enqueue attempts. This prevents the close/drain path from seeing an empty queue and returning `EPIPE` while a sender is between the closed check and the actual CAS enqueue. The drain path spins until `inflight == 0`.

### Multi-channel select (@match)

1. A `select_wait_group` is created with `selected_index = -1`.
2. Wait nodes are registered on each channel, each pointing to the group.
3. Any channel activity calls `cc__chan_select_try_win(node)` which CAS's `selected_index` from `-1` to the node's index. Only one node wins.
4. Non-winners are cancelled (`notified = CANCEL`).
5. The winning node's channel operation proceeds normally.

### Unbuffered (rendezvous) channels

Unbuffered channels have `cap == 0` and require a synchronous sender-receiver handshake. Once a fiber commits to a rendezvous (adds itself to the wait list), it must see that operation through. Nursery cancellation checks are performed only **before** committing, not inside the waiting loop.

### Ordered task-channel pattern

Ordered channels implement **FIFO on task handles**, not "FIFO on completion time." Ordering comes from the receiver awaiting handles in the order they were received.

- **Head-of-line blocking**: if an earlier task stalls, later completed tasks are not observed until earlier ones complete.
- **Result lifetime**: the pointer returned by `block_on` may be ephemeral (fiber can be recycled). Receivers MUST copy result bytes out immediately.
- **Send-failure cleanup (required)**: if lowering spawns a task and then fails to send the handle, the implementation MUST immediately **join** the spawned task and then free it.

## Affinity and direct handoff

### Soft affinity

Wakeups target `last_worker_id` as a hint. If that worker is not available (dead/exited/generation mismatch), fall back to any worker (e.g. global queue).

Starvation escape hatch: if the affinity target is not making progress (heartbeat stale) or its inbox exceeds a configured threshold, the wakeup MUST be diverted to a global/fair queue so other workers can run it.

### Hard affinity (direct handoff)

If a worker completes a task and the consumer fiber is `PARKED` on that task's result, the worker may resume the consumer without enqueuing.

Safety rule: **handoff may not publish the producer fiber as `IDLE` until the worker is no longer executing on the producer fiber's stack**.

Required mechanism: perform the publish-to-idle step on a stack that is **not** the producer fiber stack (i.e. a scheduler/trampoline stack).

## Sleep queue (timer-based fiber parking)

`cc_sleep_ms` (and any future timed waits) use a **sleep queue** instead of busy-yielding through the run queue.

- A global `sleep_queue` is a mutex-protected singly-linked list of `fiber_task*`, each with a `sleep_deadline` (`CLOCK_MONOTONIC`).
- `sq_drain` walks the list, removes expired fibers, and pushes them to the global run queue.
- Draining happens in sysmon (every tick) and in idle workers (before sleeping).

## Sysmon

Sysmon is advisory and must be bounded.

### Sleep queue draining

Sysmon calls `sq_drain` on every tick to wake fibers whose sleep deadline has passed, then signals idle workers.

### Horizontal scaling

- Trigger: worker heartbeat stale and runnable work exists.
- Response: spawn a temporary worker subject to:
  - **cap**: max total workers = base + K (configurable)
  - **backoff**: global rate limit of at most 1 spawn per `SCALE_INTERVAL`
  - **retirement**: temporary workers exit after `TEMP_IDLE_TIMEOUT` of no work

Correctness requirement: worker retirement must not drop queued work; runnable fibers must remain discoverable via surviving queues.

### Stall diagnostics

- Trigger: a fiber remains `OWNED` with `last_transition` stale for a large threshold (default: ~5 seconds).
- Response: emit a diagnostic record to stderr.
- Default on; `CC_DEBUG_STALL=0` disables.

## Shutdown / draining

Define `active_tasks` (not fibers):

- increment when a thunk is published to a fiber,
- decrement when the thunk completes (exactly once).

Shutdown sequence:

1. Wait for `active_tasks == 0` (with hard timeout).
2. Stop accepting new tasks (close nurseries / mark EOF).
3. Cancel to wake parked fibers waiting on shutdown-sensitive resources.
4. Drain until all inboxes and run queues are empty.
5. Workers terminate; sysmon stops.

## Configuration

### Environment variables

| Variable | Description |
|----------|-------------|
| `CC_WORKERS=N` | Override initial worker count |
| `CC_FIBER_STATS=1` | Print scheduler statistics at exit |
| `CC_VERBOSE=1` | Print scheduler initialization info |
| `CC_DEBUG_STALL=0` | Disable stall diagnostic (default: on) |

### Compile-time defines

| Define | Default | Description |
|--------|---------|-------------|
| `CC_FIBER_WORKERS` | 0 (auto-detect) | Fixed worker count |
| `CC_FIBER_STACK_SIZE` | 2MB (vmem) / 128KB | Per-fiber stack size |
| `CC_FIBER_QUEUE_INITIAL` | 4096 | Slots in global run queue ring |

### Debug flags (environment)

| Variable | Description |
|----------|-------------|
| `CC_DEBUG_JOIN=1` | Log fiber join/unpark operations |
| `CC_DEBUG_WAKE=1` | Log wake counter increments |
| `CC_DEBUG_INBOX=1` | Log inbox queue operations |
| `CC_DEBUG_INBOX_DUMP=1` | Dump inbox contents on deadlock |
| `CC_DEBUG_SYSMON=1` | Log sysmon scaling decisions |
| `CC_CHAN_DEBUG=1` | Channel operation logs |
| `CC_CHAN_DEBUG_VERBOSE=1` | Detailed select/wake logging |
| `CC_CHAN_NO_LOCKFREE=1` | Force mutex-based channel path |
| `CC_DEBUG_DEADLOCK_RUNTIME=1` | Parked fiber dump on deadlock |
| `CC_DEADLOCK_ABORT=0` | Continue after deadlock (for log capture) |
| `CC_PARK_DEBUG=1` | Log `park_if` decisions |

## Implementation files

- `cc/runtime/fiber_sched.c`: Core scheduler
- `cc/runtime/channel.c`: Channel operations (buffered, unbuffered, lock-free)
- `cc/runtime/nursery.c`: Structured concurrency nurseries
- `cc/runtime/task.c`: CCTask API and spawn functions
- `cc/include/ccc/cc_sched.cch`: Public API declarations
