# Concurrent-C Fiber Scheduler v2 (Virtual Cores)

This is a minimal, complete spec for the fiber scheduler state machine, ownership rules, and park/unpark protocol.

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

Note: if the system permits unbounded growth, reuse is best-effort; otherwise impose a global cap (out of scope here).

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
- `DONE (-4)`: the fiber has completed; its stack is quiescent.  Set by the
  trampoline after `mco_resume` returns so joiners know it is safe to reclaim
  the fiber.  Fibers in the idle pool may retain `DONE`; `fiber_alloc`
  transitions them back to `IDLE` before reuse.
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

This design keeps the common case lock-free while handling arbitrary spawn bursts (e.g. 100k fibers) without blocking the caller. `fq_push_blocking` **never blocks** — it always succeeds by falling back to the overflow list.

Fairness rule: workers MUST periodically yield to a global/fair scheduling point (e.g. push current fiber to a global queue or check the global queue) after a bounded amount of local-only work, to prevent starvation under contention.

Distribution rule: new runnable work that is not a targeted wakeup SHOULD be distributed round-robin across workers (or an equivalent balancing policy). Targeted wakeups use affinity (below), with the starvation escape hatch.

### Spawn (assignment)

Spawner acquires an `IDLE` fiber (local idle stack fast-path). Then:

1. `CAS(f->control, IDLE, ASSIGNED)` to lock metadata.
2. Write runnable metadata (thunk `fn`, `arg`, etc.).
3. Publish to a worker by pushing `f` to that worker's inbox.
4. The target worker dequeues `f` and claims execution by `CAS(f->control, ASSIGNED, OWNED(worker_id))`, then runs the thunk.
   - If the CAS fails, the dequeued entry is stale and MUST be discarded (the fiber will be re-enqueued if needed).

If no `IDLE` fiber is available, the runtime MUST take a non-blocking slow path (e.g. allocate a new fiber / pull from a global pool). It must not block a worker thread waiting for an idle fiber.

## Parking and unparking (no lost wakeups)

Blocking in this runtime is expressed as:

- a **wait object** that owns a wait queue (e.g. a channel),
- a **wait node** (stack-local or intrusive) that contains:
  - `fiber`: the fiber to resume
  - `notified`: `_Atomic int` (0 means "still waiting"; non-zero means "woken with reason")
- the scheduler primitive `park_if(&notified, 0)` which commits to parking only if the flag is still 0.

The scheduler also has an internal `pending_unpark` latch per fiber: if an unpark races with a park attempt, the latch forces `park_if` to return without sleeping (so the signal is not lost). Spurious wakes are allowed; waiters must handle `notified==0`.

### `park_if(flag, expected)` contract (inlined)

`park_if(&flag, expected)` is the only scheduler primitive required by channel-style waits. It must satisfy:

- **Safety**: if a waker performs `store(flag, value!=expected)` and then calls `unpark(fiber)`, the parked fiber must eventually observe `flag != expected` without missing the wakeup.
- **Idempotence**: `park_if` may return spuriously (without `flag` changing); callers must re-check conditions under their own lock.

Minimal required behavior (conceptual):

1. If `exchange_acq_rel(pending_unpark, 0)` returns non-zero, return immediately (do not sleep). (This consumes a stale latch once; it must not cause infinite spurious returns.)
2. If `load(flag) != expected`, return immediately.
3. Yield to a scheduler/trampoline stack (the fiber stack becomes quiescent).
4. On the scheduler/trampoline stack, attempt to commit to parking:
   - publish the parked state with a `seq_cst` store (per the Dekker rule above when using a separate `pending_unpark` latch), e.g. `atomic_store_explicit(&f->control, PARKED, memory_order_seq_cst)`
   - read `pending_unpark` with a `seq_cst` read (e.g. `atomic_load_explicit(&f->pending_unpark, memory_order_seq_cst)`); if non-zero, clear it (e.g. `exchange_acq_rel(..., 0)`) and abort parking by making the fiber runnable (e.g. `CAS(PARKED→ASSIGNED)` then enqueue). (If the CAS fails, a concurrent unpark already made it runnable; no further action needed.)
   - if `load(flag) != expected`, abort parking by making the fiber runnable
5. If still parked, run another runnable fiber. When this fiber is later made runnable, it resumes and returns to the caller.

This yield-before-commit ordering is REQUIRED (it avoids needing a separate `running_lock` by ensuring `PARKED` is published only when the fiber stack is already quiescent).

Note: callers typically wrap the parking sequence in a loop, re-checking the blocking condition under the wait object's lock after each return from `park_if`.

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

### `unpark(fiber)` contract (inlined)

`unpark(fiber)` must be **idempotent** and must not lose wakeups:

- If the target fiber is sleeping/parked, `unpark` must make it runnable.
- If the target fiber is in the parking transition (between yield and the park commit), `unpark` must still make it runnable (typically by setting/retaining `pending_unpark` so the commit aborts and enqueues).
- If the target fiber is running / not parkable, `unpark` must set or retain the fiber's `pending_unpark` latch so the next `park_if` returns without sleeping.

Select-style groups require an additional rule: update the group's "winner chosen" state **before** waking any member.

Select cleanup rule: after a select wakes, the fiber must **deregister/cancel all losing wait nodes** from their respective wait lists before returning to user code (leaving a losing node linked is a correctness bug; if nodes are stack-local it is also a memory-safety bug).

## Channels (scheduler contract)

This section only describes how channel operations block/wake; it does not specify channel buffering semantics in detail.

Non-blocking channel fast paths (buffer not full/empty, direct handoff, etc.) do not involve the scheduler; only blocking paths use `park_if`/`unpark`.

### Common rules (all async channel waits)

- **Fiber context rule (required)**: if the caller is a fiber, channel operations must never block the OS thread. Even for a channel configured as "sync", blocking must be implemented by parking the fiber (`park_if`/`unpark`).
- **Wait node**: the wait node may be stack-local or **intrusive** (stored in the fiber/task struct). In all cases, it starts with `_Atomic notified = 0` and is enqueued under the channel lock.
- **Park primitive**: the waiter releases the channel lock and calls `park_if(&node.notified, 0)`.
- **Wake primitive**: the waker sets `node.notified` to a non-zero reason (DATA/CLOSE/CANCEL/SIGNAL) **then** calls `unpark(node.fiber)`.
- **Spurious wake**: if the waiter returns and `notified==0`, it must remove its node from the wait list before retrying (stack-local node must not remain linked).

### Variant differences (only what matters to the scheduler)

| Channel kind | Blocks when | Wake reason / action | Notes |
|---|---|---|---|
| **sync** (any cap) | full/empty/partner | condvar signal/broadcast | **Meaning**: thread-blocking when *not* in fiber context. In fiber context, follows the fiber context rule above (park fiber; do not block OS thread). |
| **async buffered** (cap>0) | send: full, recv: empty | set `notified=DATA` then `unpark` | May do direct handoff under lock (sender satisfies waiting receiver or vice versa). |
| **async rendezvous** (cap==0) | until a partner exists | set `notified=DATA` then `unpark` | Cancellation must be checked **before** the commit point (node enqueued under lock). After commit, the operation must complete via DATA or close/error; it must not abort “mid-wait”. |
| **ordered** | same as buffered/unbuffered | DATA wakes the recv | Payload is a `CCTask` handle (FIFO). Receiver awaits handles in FIFO order (see ordered pattern below). |
| **owned (pool)** | recv: empty pool | DATA wakes recv | recv may create (`on_create`), send may reset (`on_reset`). Typically configured as sync. |

### Ordered task-channel pattern (`chan_send_task` / ordered receive)

Ordered channels implement **FIFO on task handles**, not “FIFO on completion time.” Ordering comes from the receiver awaiting handles in the order they were received.

- **Head-of-line blocking**: if an earlier task stalls, later completed tasks are not observed until earlier ones complete.
- **Join behavior**: after receiving a `CCTask`, awaiting completion uses the standard join protocol (fiber context uses `park_if`/`unpark`; thread context may use condvar).
- **Result lifetime**: for fiber tasks, the pointer returned by `block_on` may be ephemeral (fiber can be recycled immediately). Receivers MUST copy result bytes out immediately.
- **Result layout convention (if using fiber-local result storage)**: if a task stores its result in fiber-local storage, the user-visible result MUST be at offset 0 (commonly named `__result`) so ordered recv can extract it without allocation.
- **`chan_send_task` vs `cc_chan_send_task`**: `chan_send_task(ch, () => ...)` means “spawn a task and send the `CCTask` handle.” `cc_chan_send_task(ch, &value, sizeof(value))` is a different API: it returns a poll-based task representing the *send operation* (caller must ensure `value` outlives that task).
- **Send-failure cleanup (required)**: if lowering spawns a task and then fails to send the handle (closed/error), the implementation MUST immediately **join** the spawned task (e.g. `block_on`) and then free it. (Do not detach/orphan work; `free` without join is unsafe if the task is still running.)

## Affinity and direct handoff

### Soft affinity

Wakeups target `last_worker_id` as a hint. If that worker is not available (dead/exited/generation mismatch), fall back to any worker (e.g. global queue or round-robin inbox).

Starvation escape hatch: if the affinity target is not making progress (heartbeat stale) or its inbox exceeds a configured threshold, the wakeup MUST be diverted to a global/fair queue (or another worker) so other workers can run it.

### Hard affinity (direct handoff)

If a worker completes a task and the consumer fiber is `PARKED` on that task's result, the worker may resume the consumer without enqueuing.

Safety rule: **handoff may not publish the producer fiber as `IDLE` until the worker is no longer executing on the producer fiber's stack**.

Required mechanism: perform the publish-to-idle step on a stack that is **not** the producer fiber stack (i.e. a scheduler/trampoline stack).

Minimal safe sequence:

1. `CAS(consumer->control, PARKED, OWNED(me))` to claim the consumer stack.
2. Switch from producer fiber stack to a scheduler/trampoline stack.
3. On the scheduler/trampoline stack:
   - set `producer->control = IDLE` and return it to the idle pool
4. Switch from scheduler/trampoline stack to the consumer fiber stack.

Direct producer→consumer switching is permitted only if you can prove it never publishes `producer` as `IDLE` while still executing on the producer stack (otherwise it is forbidden by this spec).

## Sleep queue (timer-based fiber parking)

`cc_sleep_ms` (and any future timed waits) use a **sleep queue** instead of busy-yielding through the run queue. This eliminates O(N) queue churn when N fibers sleep concurrently.

Design:
- A global `sleep_queue` is a mutex-protected singly-linked list of `fiber_task*` (via `next`), each with a `sleep_deadline` (`struct timespec`, `CLOCK_MONOTONIC`).
- `cc_sleep_ms` computes the deadline, transitions the fiber to `QUEUED`, pushes it to the sleep queue, and yields (`mco_yield`). The fiber is **not** on the run queue.
- `sq_drain` walks the list, removes expired fibers, and pushes them to the global run queue via `fq_push_blocking`. It then wakes idle workers.
- Draining happens in two places:
  1. **Sysmon**: every `SYSMON_CHECK_US` (~250µs). Wakes workers via `wake_one_if_sleeping_unconditional`.
  2. **Idle workers**: before checking run queues in the sleep loop, each worker calls `sq_drain` to avoid waiting for sysmon.

Latency: sleep resolution is bounded by max(`SYSMON_CHECK_US`, worker idle poll interval). For a 100ms sleep this is negligible; for sub-millisecond sleeps, resolution is ~250µs.

Future: a timer wheel (hashed by deadline bucket) would reduce `sq_drain` from O(N) to O(expired), but the mutex-list is sufficient for current workloads.

## Sysmon

Sysmon is advisory and must be bounded.

### Sleep queue draining

Sysmon calls `sq_drain` on every tick to wake fibers whose sleep deadline has passed, then signals idle workers via the wake primitive.

### Horizontal scaling

- Trigger: worker heartbeat stale and runnable work exists.
- Response: spawn a temporary worker subject to:
  - **cap**: max total workers = base + K (configurable)
  - **backoff**: global rate limit of at most 1 spawn per `SCALE_INTERVAL`
  - **retirement**: temporary workers exit after `TEMP_IDLE_TIMEOUT` of no work

Constants (must be defined in implementation/config):

- `SCALE_INTERVAL`: global (not per-worker); choose a value large enough to avoid thrash under normal descheduling.
- `TEMP_IDLE_TIMEOUT`: how long a temp worker must remain idle before exiting.

Correctness requirement: worker retirement must not drop queued work; runnable fibers must remain discoverable via surviving queues.

### Stall diagnostics

- Trigger: a fiber remains `OWNED` with `last_transition` stale for a large threshold.
- Response: emit a backtrace/diagnostic record.

## Shutdown / draining

Define `active_tasks` (not fibers):

- increment when a thunk is published to a fiber,
- decrement when the thunk completes (exactly once).

Implementation note (required for scalability): `active_tasks` may be implemented as sharded counters (per-worker/per-nursery) as long as shutdown checks the sum and the sum reaches 0 exactly when all published thunks have completed.

Shutdown sequence:

1. Stop accepting new tasks (close nurseries / mark EOF).
2. Cancel to wake parked fibers that are waiting on shutdown-sensitive resources.
3. Drain until:
   - all inboxes and run queues are empty, and
   - `active_tasks == 0`.
4. Workers terminate; sysmon stops.
