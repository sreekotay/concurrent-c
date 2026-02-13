# Fiber Runtime Lifecycle Spec — In‑Flight States (Draft)

## 0. Scope and Design Principle

### Principle of Orthogonal Concerns

- **Skeleton (Structure):** Nurseries + Arenas form a lexical **ownership tree**. Lifetimes are hierarchical, compiler-enforced.
- **Circulatory System (Flow):** Channels (and other Waitables) form a dynamic **communication graph**. Lifetimes are independent and runtime-enforced.

**Hard rule:** Channel lifetime / close is independent of nursery cancellation and arena lifetime.

### Runtime Modeling Decision (Recorded)

- `§4` park/wake protocol is the correctness contract for wait transitions and
  wake ownership (`PARKING/PARKED/WAKING`, publish/unpublish, wake-claim rules).
- The existing control-word scheduler machinery may remain as an implementation
  substrate for hot-path execution ownership, provided it preserves all `§4`
  externally observable guarantees.
- This is an intentional hybrid: explicit wait correctness model + optimized
  execution-state implementation, not competing state machines.

### Goals

- Cooperative fibers (MUST yield/park cooperatively).
- Up to `2*M` workers (elastic pool); unlimited fibers.
- Single scheduling mechanism: per-worker work-stealing deque + global MPMC injection.
- 1:1, 1\:N, M:1 emerge as fast paths, not different queue types.
- **Lifecycle rigor:** explicit in-flight states for parking/waking to eliminate lost-wake, double-enqueue, and “run while wait-listed” classes.

---

## 1. Entities

### 1.1 Fiber

A fiber is a resumable closure + bookkeeping.

**Memory model:**

- Fiber *frame* (`Fiber` struct) is pooled (per-worker cache + global fallback).
- Captured closure / continuation data is allocated from the owning nursery’s arena.
- Fiber frame lifetime (MUST): once allocated, fiber frames MUST NOT be returned
  to the system allocator until runtime shutdown. Pool recycling is permitted.

### 1.2 Nursery

Nursery is a lexical scope object with:

- `live_fibers` counter
- `closing` flag (cooperative cancellation)

A nursery completes when `live_fibers == 0`.

Cancellation model (SHOULD):

- Nursery cancellation is lazy/cooperative. Setting `closing = true` does not
  require immediate wake of all parked fibers.
- Fibers observe cancellation at their next resume or cooperative yield point.
- Nursery join waits for `live_fibers == 0`.
- Implementations MAY add an optional per-nursery parked-fiber index for eager
  cancellation sweeps, but any wake MUST use the standard wake protocol (§4.3).

### 1.3 Waitable

Any park point (channel send/recv, timer, join, signal, etc.) implements **WaitableOps**.

### 1.4 Workers

Workers run fibers, steal work, and MAY sleep only when there is truly no work.

---

## 2. Fiber State Machine (Normative)

### 2.1 States

```
INIT
RUNNABLE        // may appear in run queues
RUNNING         // owned by exactly one worker (on-CPU)
PARKING         // in-flight RUNNING -> PARKED
PARKED          // may appear in wait-lists
WAKING          // logical wake-claim phase; may be fused in substrate
DONE
CANCELLED
```

### 2.2 Global Invariants (MUST)

**F1 Single on-CPU owner:**

- If `state ∈ {RUNNING, PARKING}`, exactly one worker owns the fiber.

**F2 Run-queue eligibility:**

- Only `RUNNABLE` fibers may appear in scheduler run queues (local deque, global MPMC). *(Assertable in debug builds.)*

**F3 Wait-list eligibility:**

- Only `PARKED` fibers may be removed and woken by a waker.
- Wait structures may temporarily reference `PARKING` fibers *only* for the purpose of race-free publication; such entries are observable but not claimable.
- Wakers MUST NOT remove/enqueue unless they first claim `PARKED`.

**F4 Single enqueue owner:**

- Only the thread that wins the wake claim (`PARKED -> WAKING` logical edge, or fused equivalent) may enqueue the fiber.

**F5 Post-park immediate yield:**

- After committing `PARKED`, the fiber MUST immediately return to the scheduler loop (i.e., no further user code).

---

## 3. Waitable Abstraction (Normative)

All parking logic goes through this interface.

```c
typedef struct WaitableOps {
  // Fast path: completes without parking
  bool (*try_complete)(void *W, Fiber *f, void *io);

  // Publish: makes f discoverable by wakers
  // If returns true, a wake after this call MUST be observable.
  // If returns true, waiter discoverability MUST be maintained until either:
  // (a) waker wins wake-claim on PARKED (logical WAKING edge), or
  // (b) parker unpublishes while still PARKING.
  bool (*publish)(void *W, Fiber *f, void *io);

  // Unpublish: remove f if parking is aborted while still on-CPU
  // MUST be idempotent and safe if waiter was concurrently removed/tombstoned.
  void (*unpublish)(void *W, Fiber *f);
} WaitableOps;
```

### 3.1 Internal Scheduler/Channel/Waitable Contract (Normative)

The runtime boundary for scheduler v3 is intentionally small and stable:

```c
void schedule(Fiber *f);                 // enqueue RUNNABLE exactly once
Fiber *worker_next(void);                // local -> global -> steal -> idle progression
WaitResult fiber_wait(void *W, void *io, const WaitableOps *ops);
void fiber_wake(Fiber *f);               // wake protocol entry point for waitables
```

Contract requirements:

- `schedule(f)` MUST only accept `f.state == RUNNABLE` (F2) and MUST publish
  runnable visibility before any wake signal/generation bump.
- `worker_next()` MUST return either a fiber transitioned to `RUNNING` or `NULL`
  when entering idle transitions; it MUST NOT return non-runnable ownership.
- `fiber_wait()` owns the full `RUNNING -> PARKING -> {RUNNING|PARKED}` protocol
  and is the only legal path that publishes/removes waiters on behalf of fibers.
- `fiber_wake()` owns the `PARKED -> {wake-claim} -> RUNNABLE` claim/enqueue protocol
  and MUST enforce single enqueue owner (F4).
- Channel code MUST implement admission and close behavior via `WaitableOps`
  and MUST NOT directly mutate scheduler queues or worker ownership state.

---

## 4. Parking & Waking Protocol (Normative)

### 4.1 Fields

- `wake_pending : atomic<u8>` — set by wakers if they observe a fiber in `PARKING`.
- `wait_ticket` — required for ABA defense (see §8).

### 4.2 Parking Protocol (RUNNING -> PARKING -> {RUNNING or PARKED})

**fiber\_wait(W, io)** (executed by the RUNNING fiber):

1. **Fast path:**

- If `W.try_complete(W,f,io)` succeeds → return `WAIT_OK` (remain RUNNING).

2. **Enter PARKING:**

- `wake_pending = 0`
- `store_release(f.state, PARKING)`
- Assign `f.waitable = W`, assign fresh `f.wait_ticket` (see §8).

3. **Publish waiter:**

- If `W.publish(W,f,io)` fails → `store_release(f.state, RUNNING)` and return (e.g. `WAIT_CLOSED`).

4. **Commit park first (MUST):**

- `CAS_release(f.state, PARKING, PARKED)` MUST succeed.

5. **Lost-wake recovery check (MUST):**

- If `exchange_acq_rel(f.wake_pending, 0) == 1`:
  - `W.unpublish(W,f)` (idempotent)
  - `CAS_acq_rel` wake-claim on `PARKED` MUST succeed
    (`PARKED -> WAKING` logical edge, or fused substrate equivalent).
  - `store_release(f.state, RUNNABLE)`
  - return `WAIT_OK` (self-recovered wake before scheduler yield)

6. **Yield parked fiber:**

- Return `F_PARKED` to the worker loop immediately (F5).

### 4.3 Waking Protocol (PARKED -> RUNNABLE)

When a wake condition occurs, the waker:

1. Reads `s = load_acquire(f.state)`.

2. Case split:

- If `s == PARKING`:
  - `store_release(f.wake_pending, 1)` and return. (No unpublish, no enqueue.)
  - Cancellation targeting a PARKING fiber MUST use this same wake-pending path.
- If `s == PARKED`:
  - MUST validate ABA ticket (see §8).
  - `CAS_acq_rel` wake-claim on `PARKED`; only winner MAY proceed
    (`PARKED -> WAKING` logical edge, or fused substrate equivalent).
  - Remove `f` from wait structure (or tombstone).
  - `store_release(f.state, RUNNABLE)`
  - Enqueue via scheduler exactly once (F4).
- Else: return.

---

## 5. Channel Semantics (Direct Handoff First)

Channels are a Waitable. They MUST attempt direct rendezvous before buffering/parking.

### 5.1 Send Admission / Close Definition (Precise)

Let channel state be `OPEN` or `CLOSED`.

**Send(x) admission rule (MUST):** A send is **admitted** iff it observes the channel as `OPEN` at its admission linearization point (defined below). If admitted, it MUST either:

- transfer the element to a receiver (direct handoff or buffer), or
- park as a sender waiter if the channel remains OPEN and cannot complete immediately.

If not admitted (channel observed CLOSED at admission LP), send MUST fail with `CLOSED` and MUST NOT transfer data.

**Send admission linearization point (LP):**

- For direct handoff: the moment the sender successfully claims a receiver waiter (or matches) while channel is OPEN.
- For buffered send: the moment the sender successfully reserves a buffer slot while channel is OPEN.
- For parking send: the moment `publish()` returns true *and* the channel is still OPEN under the channel’s internal publish rule.

### 5.2 Receive Admission / Close Definition (Precise)

**Recv admission rule (MUST):** A receive is **admitted** iff it can obtain an element by:

- popping from buffer, or
- direct handoff from a sender waiter, OR if channel is OPEN and it successfully publishes itself as a receiver waiter.

If channel is CLOSED and no buffered element / sender handoff exists, receive MUST complete with `CLOSED` and MUST NOT park.

A CLOSED channel with remaining buffered data or admitted parked senders MUST
still deliver that data to receivers. CLOSED prevents new send admissions; it
does not discard in-flight data.

**Recv admission LP:**

- Buffered recv: successful buffer pop.
- Direct handoff: successful claim of a sender waiter.
- Parking recv: `publish()` success under OPEN.

### 5.3 Close Semantics

**Close() LP:** the atomic transition `OPEN -> CLOSED`.

After Close LP:

- No new send/recv may be admitted via parking.
- Sends not yet admitted MUST fail.
- Recvs with no available data MUST fail.
- All published waiters MUST be woken with `CLOSED`.

Admission note (testable):

- A sender/receiver that was already admitted before Close LP MAY still complete per §5.1/§5.2 admission rules.
- A sender/receiver not admitted before Close LP MUST fail with `CLOSED` and MUST NOT transfer data.

### 5.4 Queue Admission Rules After Close (Normative)

After Close LP, waitable publish rules MUST enforce:

- `publish()` for new send/recv admissions returns failure (or equivalent) once
  `CLOSED` is observed at the publish LP.
- A waiter published before Close LP remains valid in-flight work and MAY
  complete if subsequently claimed by a matching operation.
- Close wake-all MUST be deterministic/idempotent: repeated close wake scans
  must not cause duplicate scheduler enqueues of the same waiter.
- Wakers processing close-induced wakeups MUST use standard wake-claim semantics
  (`PARKED -> WAKING -> RUNNABLE` logical path, or fused equivalent) and MUST NOT bypass F4.

### 5.5 Rendezvous-first Algorithm (Non-normative sketch)

**send(x):**

1. if CLOSED → fail
2. if receiver waiter exists → memcpy/move into receiver slot; wake receiver; success
3. else if buffer space → push; success
4. else park sender via fiber\_wait

**recv():**

1. if buffer has value → pop; success
2. else if sender waiter exists → memcpy/move from sender; wake sender; success
3. else if CLOSED → CLOSED
4. else park receiver via fiber\_wait

---

## 6. Worker Lifecycle (Normative)

### 6.1 Worker States

```
ACTIVE
IDLE_SPIN         // bounded probing/steal while still hot
SLEEP             // parked wait; only when no runnable work is visible
DRAINING          // no new work; drain local->global; then retire
DEAD
```

### 6.2 DRAINING Rules (MUST)

- A worker entering `DRAINING` MUST NOT steal or accept new work.
- It MUST drain local deque to global MPMC.
- It MUST exit after its current scheduler slice.

`DRAINING` trigger guidance:

- Implementations SHOULD use **no-progress** thresholds (stalled worker) rather than raw CPU busy duration.

---

## 7. Elastic Pool Scaling (Pressure + Active)

### 7.1 Counters

- `active_count`: approx # workers executing fibers
- `pressure`: approx backlog (# RUNNABLE enqueued minus dequeued)

**pressure accounting (SHOULD):**

- increment when enqueuing RUNNABLE
- decrement when dequeuing RUNNABLE for execution

`pressure` safety rule:

- Implementations MUST use signed or saturating arithmetic for `pressure`.
- Transient negative values are acceptable under concurrent races.
- Debug builds SHOULD assert that `pressure` does not remain negative over sustained intervals.

### 7.2 Scale-up heuristic (Non-normative, recommended)

Spawn new worker (up to `2*M`) when:

- no sleepers to wake, and
- either `(active_count ≈ worker_count AND pressure is rising)` or `pressure` exceeds threshold.

### 7.3 Runtime Explainability Notes (Current implementation)

The active runtime emits optional pressure telemetry (`CC_V3_PRESSURE_STATS_DUMP=1`)
that includes:

- sample split (`positive_samples`, `negative_samples`) and `current_pressure`
- promotion gate passes and promoted worker count
- max sustained negative-pressure streak (`neg_streak_max`)
- gate-block reasons (`no_work`, `nonpositive`, `idle_capacity`, `not_stuck`)

This keeps scale decisions auditable and helps detect prolonged negative drift.

---

## 8. Waiter ABA Defense (Normative)

### 8.1 Problem

Wait lists store pointers to fibers. Because fiber frames are pooled/reused, a waker can observe a stale waiter node referencing a fiber frame that has been recycled for a different fiber (“ABA”).

### 8.2 Requirement: Ticket

Each wait publication MUST carry an ABA-resistant `wait_ticket`.

Per-fiber monotonic wait ticket (required):

- `f.wait_ticket` increments (wrap-safe 64-bit) each time the fiber publishes to any waitable.
- Waiter nodes store `(fiber*, wait_ticket)`.
- Wakers MUST validate the ticket matches before attempting wake.

### 8.3 Ticket Validation Rule (MUST)

A waker may only attempt wake-claim on `PARKED` (`PARKED -> WAKING` logical edge, or fused equivalent) if:

- it reads the waiter node ticket `t`, and
- it reads `f.wait_ticket == t` (acquire), else it MUST treat the waiter as stale and ignore/remove it.

Stale waiter handling:

- On ticket mismatch, the waker MUST NOT perform wake-claim and MUST NOT enqueue `f`.
- A waiter node MUST NOT be reclaimed or reused until no concurrent waker can still observe it (e.g., via ticket/epoch/hazard discipline).

---

## 9. Transition Legality Matrix (1-page)

**Legend**

- Actor: `F` = running fiber, `Wk` = worker loop, `W` = waker (waitable), `S` = spawner, `N` = cancellation source.
- CAS: required atomic transition.
- Enqueue: only permitted if resulting state is RUNNABLE.

| From     | To        | Actor | CAS required | Notes                                                         |
| -------- | --------- | ----- | ------------ | ------------------------------------------------------------- |
| INIT     | RUNNABLE  | S     | no           | spawn increments nursery live\_fibers; enqueue RUNNABLE       |
| RUNNABLE | RUNNING   | Wk    | queue-ownership required | dequeue winner sets RUNNING; extra CAS only if queue cannot guarantee single-consumer ownership |
| RUNNING  | RUNNABLE  | F/Wk  | no           | cooperative yield: return RUNNABLE; worker enqueues           |
| RUNNING  | PARKING   | F     | no           | store\_release; only running fiber may enter PARKING          |
| PARKING  | RUNNING   | F/N   | no           | cancellation/wake-pending while still on-CPU; MUST unpublish |
| PARKING  | PARKED    | F     | yes          | CAS PARKING->PARKED commit phase                              |
| PARKED   | WAKING    | F     | yes          | self-claim lost-wake recovery before yielding                 |
| PARKED   | WAKING    | W     | yes          | CAS PARKED->WAKING; cancellation wake uses same claim protocol |
| WAKING   | RUNNABLE  | W     | no           | store\_release then enqueue exactly once                      |
| RUNNING  | DONE      | F     | no           | completion; decrement nursery live\_fibers; recycle           |
| RUNNING  | CANCELLED | F     | no           | self-cancel at cooperative yield point                        |
| PARKED   | RUNNABLE  | W     | via WAKING   | must go through WAKING; never direct PARKED->RUNNABLE         |

Cancellation path note:

- A parked fiber is cancelled via the standard wake path (`PARKED -> WAKING ->
  RUNNABLE` logical path, or fused equivalent) and then self-cancels at a cooperative yield/resume point.

**MUST NOT transitions:**

- PARKED -> RUNNABLE by parker
- Any non-RUNNABLE state enqueued into run queues
- Waker enqueues fiber observed in PARKING

### 9.1 Current Runtime Coverage Notes

Current matrix coverage in the active runtime is split between runtime asserts and
static/protocol guarantees:

- **Runtime asserts (`CC_V3_SPEC_ASSERT=1`):**
  - `RUNNABLE -> RUNNING` claim in `cc/runtime/fiber_sched.c` `worker_run_fiber()`.
  - enqueue legality (`RUNNABLE`/`CTRL_QUEUED` only) on scheduler queue publication paths.
  - labeled wake-path matrix checks at non-racy claim points for `PARKED -> RUNNABLE`
    in `worker_commit_park()` self-recovery paths and `cc__fiber_unpark()` waker path.

- **Static/protocol guarantees (documented + code anchored):**
  - `PARKING -> PARKED` commit and `wake_pending` recovery are serialized by
    `worker_commit_park()` protocol and LP ordering in §10.
  - Waker on `PARKING` never enqueues; it only publishes wake-pending
    (`cc__fiber_unpark()` PARKING branch), satisfying MUST NOT rows.
  - Channel wake/claim paths enforce waiter-ticket validity before wake/enqueue
    (`cc/runtime/channel.c` waiter helpers), preventing stale-node enqueue.

---

## 10. Memory-Order Table (Linearization Points)

This table defines minimal ordering for correctness. Stronger orderings are allowed.

| Event / LP         | Operation                               | Required order                                                                | Why                                                             |
| ------------------ | --------------------------------------- | ----------------------------------------------------------------------------- | --------------------------------------------------------------- |
| Enqueue RUNNABLE   | publish run-queue node + state RUNNABLE | state store\_release before enqueue publish OR combined under queue’s release | ensures dequeuer sees RUNNABLE and initialized closure pointers |
| Dequeue to RUNNING | take from queue                         | acquire on queue pop; then store\_release RUNNING                             | ensures visibility of fiber fields and owner handoff           |
| Enter PARKING      | state = PARKING                         | store\_release                                                                | wakers that see waiter must not reorder before this             |
| Waiter publish LP  | publish into wait structure             | release (within publish)                                                      | ensures wakers see waiter node contents and ticket              |
| Commit PARKED      | CAS PARKING->PARKED                     | CAS with release on success                                                   | establishes parked-ness before yielding                         |
| Post-commit check  | exchange wake\_pending,0                | exchange acq\_rel                                                             | closes PARKING->PARKED wake race window                         |
| Self-claim wake    | CAS PARKED->WAKING (self)               | CAS acq\_rel                                                                  | lost-wake recovery uses same single-owner wake claim            |
| Waker sees PARKING | set wake\_pending=1                     | store\_release                                                                | ensures parker’s acquire sees it                                |
| Waker claim        | CAS PARKED->WAKING                      | CAS acq\_rel                                                                  | single winner; orders removal/enqueue                           |
| Wake enqueue       | state RUNNABLE + enqueue                | store\_release then enqueue publish                                           | dequeuer sees RUNNABLE                                          |
| Close LP           | OPEN->CLOSED                            | exchange acq\_rel                                                             | synchronizes admission/close; prevents new publishes            |

Notes:

- Relaxing any ordering in this table is only permitted if equivalent happens-before edges are proven and documented.

### 10.1 Current Runtime Code Anchors (Hybrid substrate)

The current runtime uses control-word transitions as the implementation substrate
for LPs while preserving the contract above. LP/code anchors:

| LP | Current code anchor(s) | Substrate note |
| -- | -- | -- |
| Enqueue RUNNABLE | `cc/runtime/fiber_sched.c`: `cc_fiber_spawn()` (`CTRL_IDLE -> CTRL_QUEUED` + queue push), `cc__fiber_unpark()` queued path, `worker_commit_park()` self-recovery enqueue | RUNNABLE publication is represented by `CTRL_QUEUED` plus queue visibility |
| Dequeue to RUNNING | `cc/runtime/fiber_sched.c`: `worker_run_fiber()` CAS `CTRL_QUEUED -> CTRL_OWNED(...)` | Ownership claim and RUNNING handoff are combined in control-word CAS |
| Waiter publish LP | `cc/runtime/channel.c`: `cc__chan_add_waiter()` (`node->in_wait_list = 1` after list link under `ch->mu`); boundary path `cc/runtime/fiber_sched_boundary.c`: `cc_sched_fiber_wait()` publish call | Channel wait-list linkage under mutex is publish LP for channel waiters |
| Commit PARKED | `cc/runtime/fiber_sched.c`: `worker_commit_park()` CAS `CTRL_OWNED -> CTRL_PARKED` | Park commit occurs in trampoline after stack quiescence |
| Post-commit check | `cc/runtime/fiber_sched.c`: `worker_commit_park()` `atomic_exchange(pending_unpark, 0, seq_cst)` | Implements wake-pending recovery after park commit |
| Waker sees PARKING | `cc/runtime/fiber_sched.c`: `cc__fiber_unpark()` branch `expected == CTRL_PARKING` then `pending_unpark=1` | No enqueue allowed while stack is still live |
| Waker claim + wake enqueue | `cc/runtime/fiber_sched.c`: `cc__fiber_unpark()` CAS `CTRL_PARKED -> CTRL_QUEUED` then affinity/global push | In substrate, claim and wake-enqueue are fused by `PARKED -> QUEUED` |
| Close LP | `cc/runtime/channel.c`: `cc_chan_close()` / `cc_chan_close_err()` set `ch->closed = 1` under `ch->mu` | Admission observes CLOSED via channel lock/publish rules; close wake-all follows |

The fused `PARKED -> QUEUED` wake path is acceptable under this spec because it
preserves single-owner wake claim and exactly-once enqueue semantics.

---

## 11. Notes on Implementation Choices (Non-normative)

- Fiber frame pooling: per-worker cache reduces contention.
- Wait lists: lock-free stacks are acceptable, but MUST handle ABA (tickets/epochs) and removal strategy (tombstone or list splice under CAS).
- Debug builds SHOULD validate F2/F4 aggressively.
- Debug builds SHOULD assert that any waiter removed for wake has validated ticket and a successful wake-claim on `PARKED` (logical `PARKED -> WAKING` edge, or fused equivalent).

---

## 12. Appendix: Worker Scheduling Loop (Non-normative)

- Prefer local deque pop, then global MPMC, then steal.
- Sleep only after confirming no local, no global, steal failed, and no pending wakeups.
- DRAINING: no stealing/accepting; drain local->global; exit after slice.

