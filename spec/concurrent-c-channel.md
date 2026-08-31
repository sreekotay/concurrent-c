# Concurrent-C Channel

Minimal, complete spec for channel state machine, blocking/wake protocol, and correctness invariants.

## Terms

- **Channel**: a typed, bounded MPMC queue connecting producers and consumers.
- **Buffered channel** (`cap > 0`): has a ring buffer; send blocks when full, recv blocks when empty.
- **Unbuffered channel** (`cap == 0`): rendezvous; send blocks until a receiver arrives (and vice versa).
- **Wait node**: stack-allocated record placed on a channel's waiter list when a fiber/thread blocks.
- **Direct handoff**: sender copies data directly to a parked receiver's buffer (or vice versa), bypassing the ring buffer.
- **Fast path**: the current backend's lock-free enqueue/dequeue path.
- **Slow path**: the current backend's blocking loop, which combines
  mutex-protected waiter lists with lock-free queue probes.

## Correctness goals

- No lost wakeups (a parked waiter must eventually be woken when its condition becomes true).
- No double-remove (a wait node is removed from the list exactly once).
- No use-after-free (a node must be off the list before its stack frame returns).
- No data loss on close (in-flight enqueues complete before drain returns EPIPE).

## Channel state

Each channel has:

- `int closed`: the send side is closed. It changes from 0 to 1 under `mu` and never changes back.
- `int rx_error_closed`: the receive side rejected further sends.
- `int tx_error_code` / `int rx_error_code`: errno-style downstream and upstream close errors.
- Optional `CCIoError` values preserve typed close errors in each direction.
- `pthread_mutex_t mu`: protects waiter lists, close transitions, and slow-path predicates.

### Current buffered backend (`cap > 0`)

The current runtime uses either its native bounded MPMC ring queue or, when
available and suitable for the element representation, a liblfds711 bounded
MPMC queue. These choices and their layout are backend descriptions, not user
guarantees.

- `_Atomic int lfqueue_count`: approximate item count. Incremented after successful enqueue (release), decremented after successful dequeue (release). Used for fast full/empty checks in the slow path. **Approximate**: may lag behind the actual queue state; the CAS on the queue itself is authoritative.
- `_Atomic int lfqueue_inflight`: count of in-progress lock-free enqueue attempts. Incremented before the CAS, decremented after. Required for close-drain correctness (see Invariant 6).

### Current branded fast path

A channel sets `fast_path_ok = 1` when all of:

- `CC_CHAN_MINIMAL_FAST_PATH` is enabled (the default; `0` disables it)
- `use_lockfree && cap > 0 && buf != NULL`
- `elem_size <= sizeof(void*) || use_ring_queue`
- `!is_owned && !is_ordered && !is_sync`

The current backend uses this brand to skip general-path bookkeeping on the
hot path. The brand and backend selection are implementation details, not
channel API guarantees.

## Notification values

| Value | Name | Meaning |
|-------|------|---------|
| 0 | `CC_CHAN_NOTIFY_NONE` | Not yet notified (initial state) |
| 1 | `CC_CHAN_NOTIFY_DATA` | Direct handoff — data written to `node->data` |
| 2 | `CC_CHAN_NOTIFY_CANCEL` | Select case lost — node should be skipped |
| 3 | `CC_CHAN_NOTIFY_CLOSE` | Channel closed — waiter should check `ch->closed` |
| 4 | `CC_CHAN_NOTIFY_SIGNAL` | Woken — retry the operation |

`CC_CHAN_NOTIFY_SIGNAL` is the common wake. Space-available, data-available — all produce `SIGNAL`. The waiter retries and re-checks channel state.

`CC_CHAN_NOTIFY_DATA` means the waker already copied data into `node->data`. The waiter returns immediately without retrying or touching the ring buffer.

`CC_CHAN_NOTIFY_CANCEL` is used by select: when a channel tries to wake a select waiter but another case already won, the losing node is marked `CANCEL`.

`CC_CHAN_NOTIFY_CLOSE` is used by the close path to wake all parked waiters.

## Wait node

```
typedef struct cc__fiber_wait_node {
    cc__fiber*  fiber;        // fiber to wake (NULL = OS thread)
    void*       data;         // buffer pointer for direct handoff
    _Atomic int notified;     // NONE / DATA / CANCEL / CLOSE / SIGNAL
    int         in_wait_list; // 1 if on a waiter list, 0 if removed (plain int, mu-only)
    // select fields:
    void*       select_group;
    size_t      select_index;
    int         is_select;
    // linked list:
    struct cc__fiber_wait_node* next;
    struct cc__fiber_wait_node* prev;
} cc__fiber_wait_node;
```

`in_wait_list` is a plain `int`, not atomic. It is only read and written under `ch->mu`.

## Send path

### Current backend fast-path summary

```
cc_chan_send(ch, value, size):
    if (fast_path_ok && size == elem_size) {
        if (lockfree_enqueue(value) == OK) {
            fence(seq_cst)
            if (atomic_load(&ch->has_recv_waiters, acquire))
                signal_recv_waiter(ch);   // takes mu internally
            if (atomic_load(&ch->thread_recv_waiters, acquire))
                signal(not_empty);        // under mu
            return 0;
        }
        // buffer full — fall through to slow path
    }
    return chan_send_slow(ch, value, size);
```

Here `lockfree_enqueue` denotes the selected lock-free queue backend. The
`has_recv_waiters` check and the receive waiter's post-publication queue probe
have a `seq_cst` fence on each side:

```
Sender (fast path):                  Receiver (slow path):
  store(data→queue, release)           store(has_recv_waiters=1, release)
  fence(seq_cst)                       fence(seq_cst)
  load(has_recv_waiters, acquire)      try_dequeue(acquire)
```

The paired fences are required for lost-wake freedom on weakly ordered
machines including ARM64. If the sender sees a waiter, it signals; otherwise
the receiver's mandatory post-publication probe observes the queue transition
and does not park.

A false-positive (stale non-zero) just takes the lock unnecessarily — harmless.

`has_recv_waiters` counts linked receive waiters whose notification is `NONE`. Registration increments it; signaling, direct handoff, cancellation, close, and removal of an unnotified node decrement it. A signaled receive node remains linked until the receiver removes or rearms it, so list length is not the counter value.

`signal_recv_waiter(ch)` runs under `ch->mu`, changes one `NONE` receiver to `SIGNAL`, decrements `has_recv_waiters`, and unparks it without removing it. The receiver removes itself or resets to `NONE` and rearms.

### Current backend slow-path summary

```
chan_send_slow(ch, value, size):
    while (1) {
        // --- Attempt phase (no mutex) ---
        // An unlocked early close observation may avoid work, but is not the
        // admission linearization point.

        if (cap > 0) {
            inflight_inc(ch);
            if (try_enqueue(ch, value) == 0) {
                inflight_dec(ch);
                wake_recv_waiter(ch);
                return 0;
            }
            inflight_dec(ch);
        }

        if (mode == DROP_NEW) return EAGAIN;
        if (mode == DROP_OLD) { drop_oldest_and_enqueue(...); return 0; }

        // --- Blocking phase (under mu) ---
        lock(mu);

        if (ch->closed) { unlock(mu); return EPIPE; }       // Invariant 3

        // Direct handoff to parked receiver
        rnode = pop_recv_waiter(ch);                          // Invariant 1: sets in_wait_list=0
        if (rnode) {
            memcpy(rnode->data, value, elem_size);
            atomic_store(&rnode->notified, NOTIFY_DATA, release);
            unpark_or_signal(rnode);
            unlock(mu);
            return 0;
        }

        // Retry enqueue under mu (closes race window)
        if (cap > 0) {
            inflight_inc(ch);
            int rc = try_enqueue(ch, value);
            inflight_dec(ch);
            if (rc == 0) {
                signal_recv_waiter(ch);
                unlock(mu);
                return 0;
            }
        }

        // Publish waiter, unlock, then perform the mandatory Dekker re-check.
        cc__fiber_wait_node node = { .fiber=current, .data=value, .notified=0 };
        list_append(&send_waiters, &node);
        unlock(mu);

        fence(seq_cst);
        if (try_enqueue_with_admission(ch, value) == 0) {
            lock(mu);
            if (node.in_wait_list) list_remove(&send_waiters, &node);
            unlock(mu);
            wake_recv_waiter(ch);
            return 0;
        }

        // Park only while the published notification remains NONE.
        wait while node.notified == NONE;

        // Post-wake
        lock(mu);
        if (node.in_wait_list) list_remove(&send_waiters, &node); // Invariant 1
        unlock(mu);

        if (atomic_load(&node.notified, acquire) == NOTIFY_DATA) return 0;
        // SIGNAL, CLOSE, or spurious wake: loop and re-check under mu.
    }
```

This pseudocode is an abstract summary of the current backend;
`try_enqueue_with_admission` includes close revalidation and in-flight
accounting, and queue probes, direct handoff, and notification cleanup have
additional race checks. The mandatory invariant is publish waiter, execute
the paired `seq_cst` fence, authoritatively re-check the queue predicate, and
only then conditionally park. Fiber waits use the scheduler conditional-wait contract in
`spec/concurrent-c-scheduler.md`; the notification is checked immediately
before parking and observed with acquire ordering. OS threads use the condvar
protocol in Invariant 5.

## Recv path

The current backend mirrors send: lock-free dequeue, a `seq_cst` fence, then an
acquire observation of `has_send_waiters` before deciding whether to wake a
sender. A blocking receiver publishes `has_recv_waiters`, executes its paired
`seq_cst` fence, performs an authoritative dequeue probe, and only parks if
the queue remains empty and its notification remains `NONE`. A blocking
sender uses the symmetric publication/fence/enqueue-probe sequence. These
paired sender/receiver fences are mandatory for ARM64 lost-wake freedom.

For recv, `NOTIFY_DATA` means the sender copied data directly to the receiver's `node->data` buffer.

**Asymmetry: signal vs pop.** `signal_recv_waiter` signals a receiver in-place (sets `notified=SIGNAL`, does NOT remove the node). The receiver self-removes after waking and retrying the dequeue. `wake_one_send_waiter` pops a sender from the list (removes node, sets `in_wait_list=0`). The asymmetry is intentional: multiple senders can enqueue concurrently and all call `signal_recv_waiter` — signaling in-place naturally deduplicates (a second signal on an already-notified node is a no-op). For send waiters, each dequeue frees exactly one slot, so exactly one sender should be popped and woken.

## Unbuffered (rendezvous) channels

Unbuffered channels have `cap == 0` and `fast_path_ok = 0`. All operations go through the slow path.

### Send

```
lock(mu);
if (closed) { unlock(mu); return EPIPE; }

rnode = pop_recv_waiter(ch);
if (rnode) {
    memcpy(rnode->data, value, elem_size);
    atomic_store(&rnode->notified, NOTIFY_DATA, release);
    unpark_or_signal(rnode);
    unlock(mu);
    return 0;
}

// No receiver — park as sender
cc__fiber_wait_node node = { .fiber=current, .data=(void*)value, .notified=0 };
list_append(&send_waiters, &node);
unlock(mu);
wait while node.notified == NONE;

lock(mu);
if (node.in_wait_list) list_remove(&send_waiters, &node);
unlock(mu);

if (atomic_load(&node.notified, acquire) == NOTIFY_DATA) return 0;
lock(mu);
is_closed = ch->closed;
unlock(mu);
if (is_closed) return EPIPE;
// Spurious: retry (goto top of loop)
```

Symmetric for recv: check `send_waiters`, pop, copy from `snode->data`, set `NOTIFY_DATA`, unpark.

The mutex is held for the partner check and waiter insertion — no gap where a partner can arrive unseen.

## Close

The following is an abstract close algorithm. The current backend implements
the waiter traversal with dedicated wake-all helpers that remove each node,
perform select arbitration, publish its notification, and batch its wake:

```
cc_chan_close(ch):
    lock(mu);
    ch->closed = 1;
    cc__chan_wake_all_waiters(ch);
    pthread_cond_broadcast(&not_full);
    pthread_cond_broadcast(&not_empty);
    unlock(mu);
    wake_batch_flush();
```

For a select node, each dedicated helper first tries to claim
`selected_index`. The winner publishes `CLOSE`; a loser publishes `CANCEL`.
Both outcomes increment the group's `signaled` sequence before waking so the
select loop can observe progress and clean up every node.

### Close-send ordering (public guarantee)

Sends already in the lock-free CAS pipeline when close begins may complete. After `cc_chan_close` returns, no new send succeeds. Items admitted by in-flight senders remain visible to receivers because `lfqueue_inflight` keeps the close-drain path active until those enqueue attempts finish.

### Close forms and typed results

- `cc_chan_close` closes the transmit side gracefully. Receivers drain
  admitted values, then observe `EPIPE`; typed receive returns `Ok(false)`.
  Further sends fail with `EPIPE`.
- `cc_chan_close_err` has the same admission, drain, and wake behavior, but
  receivers observe the supplied errno-style error after draining. Blocked and
  future sends return `EPIPE`; this error closes only the receiver-facing error
  axis.
- `cc_chan_rx_close_err` is receive-side-only close. It sets `rx_error_closed`, wakes send waiters with `CLOSE`, and makes pending and future sends return the supplied error. It does not set `closed`, wake receive waiters, or discard queued values; receivers may continue draining.
- `cc_chan_close_with` closes both sides with one structured `CCIoError`. Non-select waiters and the winning select case wake with `CLOSE`; losing select nodes wake with `CANCEL`. Low-level integer operations return `e.os_code` or `ECANCELED`; typed send and receive operations return the original `Err(e)`, preserving both error kind and OS code.
- `cc_chan_cancel` is `cc_chan_close_with` using `CC_IO_CANCELLED`.

Handle `.close()` invokes the transmit-side graceful close, including when
reached through a generic handle wrapper; it is not bilateral. Bilateral close
occurs only through `.close_with(e)` or `.cancel()`. Receive-side-only
rejection is the explicit `cc_chan_rx_close_err` operation.

## Select / match

1. A `select_wait_group` is created with `_Atomic int selected_index = -1`
   and `_Atomic int signaled = 0`.
2. For each case, a `cc__fiber_wait_node` is registered on that channel's waiter list (under that channel's mu), with `is_select = 1`, `select_group = &group`, `select_index = i`.
3. When a channel claims a select waiter, it does `CAS(group->selected_index, -1, node->select_index)`.
   - CAS succeeds: this case won. Set `notified` to `SIGNAL`, `DATA`, or `CLOSE` as appropriate and unpark.
   - CAS fails: another case already won. Set the losing node to `CANCEL`, account the signal in its group, and unpark it so cleanup can finish.
4. The select loop snapshots `group->signaled`, rechecks `selected_index`, and
   conditionally parks while `signaled` is unchanged. The sequence closes the
   wake-before-park window; `selected_index` remains the sole winner
   arbitration.
5. After waking, the loop checks `selected_index` and per-node notification.
   `SIGNAL` and losing `CANCEL` notifications may cause rearm; `DATA` or
   `CLOSE` resolves the winning case.
6. The select loop removes **all** nodes from **all** channels (under each channel's mu, checking `in_wait_list`).
7. The winning case's operation proceeds normally (dequeue, handoff, etc.).

`CANCEL` never denotes a selected case. Cleanup removes every still-linked node under its channel mutex; `in_wait_list` makes cleanup idempotent when a channel wake path already unlinked a node.

## Owned channels (pool pattern)

Owned channels (`is_owned = 1`) have lifecycle callbacks:

- `on_create()`: called when recv finds an empty pool and `items_created < capacity`. Returns a new item.
- `on_destroy(item)`: called for each item when the channel is freed.
- `on_reset(item)`: called on send, before the item is returned to the pool.

Recv on an owned channel: try non-blocking dequeue; if empty and under capacity, call `on_create`; otherwise block normally.

## Delivery order

Channels without the `ordered` attribute make **no** delivery-order promise. The current backend preserves per-sender FIFO on buffered channels — the buffered direct-handoff path is gated on an empty ring (`cc__chan_buffered_handoff_would_reorder`), so a newly sent item never overtakes items already queued — but that is an implementation property of this backend, not a contract; the default backend may relax or stripe delivery. Code that relies on delivery order must declare the channel `ordered`.

## Ordered channels

`ordered` (`is_ordered = 1`) is the delivery-order guarantee. It is one property — the order of what flows through the channel is preserved — applied to the channel's payload kind:

- **Data channels:** per-sender FIFO. Items from a given sender are received in the order that sender sent them; interleaving between different senders is unspecified.
- **Task-handle channels:** the channel carries `CCTask` handles; ordering is FIFO on handles, not on completion time. The receiver awaits handles in receive order.

Head-of-line blocking (task-handle channels): if an earlier task stalls, later completed tasks are not observed until earlier ones complete.

## Build-into send operations

`send_into` / `try_send_into` are the default idiom for data carrying payloads —
reserve a channel slot, then write the element into that slot (same family as
`send_task`). Ordinary `send` / `try_send` suit already-stable values.

`cc_chan_try_send_into` and `cc_chan_send_into` invoke a `CCClosure2` builder
with `(slot, arena)`. On a typed `T[~ … >]` handle, an untyped builder infers
`slot` as `T*` and `arena` as `CCArena*`. `slot` is storage for one channel
element; `arena` is optional element payload backing supplied by the caller
(a write buffer for variable-sized bytes, not a runtime-owned batch pool). The
runtime does not own, pin, reset, or otherwise extend the arena lifetime.

`try_send_into` commits capacity before invoking the builder:

- Direct handoff: build into the parked receiver's output buffer.
- Buffered lock-free path: reserve a ring cell, build, then publish the cell.
- Buffered mutex path: build at the current tail, then advance the tail.
- Full buffer, missing rendezvous partner, or pre-admission close/error:
  return the corresponding errno without invoking the builder.

The buffered direct-handoff path uses
`cc__chan_buffered_handoff_would_reorder`; it may bypass the ring only when no
older buffered item would be overtaken.

`send_into` first attempts `try_send_into`. On `EAGAIN`, it allocates hidden
staging storage, invokes the builder exactly once into that storage, and calls
ordinary blocking `cc_chan_send`. The final slot address is therefore not part
of the observable contract: builders must not retain or compare it after
return.

The typed `CCChanTx` surface wraps errno with `cc_chan_result_with`:

- committed element: `ok(true)`;
- graceful close (`EPIPE`): `ok(false)`;
- full `try_send_into`: `err(CC_IO_BUSY)` with `os_code == EAGAIN`;
- structured close or other runtime failure: `err(e)`.

Data channels marked `ordered` accept build-into sends and retain their
per-sender FIFO contract. Owned channels and ordered task-handle channels
reject them with `EINVAL`.

## Channel modes

| Mode | Behavior when buffer full |
|------|---------------------------|
| `CC_CHAN_MODE_BLOCK` | Block sender (default) |
| `CC_CHAN_MODE_DROP_NEW` | Return `EAGAIN`, drop the new item |
| `CC_CHAN_MODE_DROP_OLD` | Drop the oldest item, enqueue the new one |

Modes are checked in the slow path before blocking. The fast path always attempts enqueue; if the buffer is full, it falls through to the slow path where the mode is applied.

## Autoclose

A channel may have an `autoclose_owner` (a nursery). Registration is
`n.close(tx)` (deprecated: `close_on`); at EMPTY the nursery calls `cc_chan_close`
on all registered channels — on both the waited (JOINING) and LEFT paths.
This is a convenience; explicit close works identically.

When `CC_NURSERY_CLOSING_RUNTIME_GUARD=1`, a blocking receive on an empty,
open autoclose channel from the same owning nursery returns `EDEADLK`. This
optional runtime diagnostic catches a receive-until-close cycle in which the
nursery cannot reach EMPTY to perform its own autoclose. It does not change
normal autoclose semantics.

## Invariants

### Invariant 1: Node list ownership

**Waker pops (removes from list). Waiter checks `in_wait_list` before removing.**

- All list mutations happen under `ch->mu`.
- `pop_recv_waiter()` / `pop_send_waiter()` remove the node and set `node->in_wait_list = 0`.
- After waking, the waiter does: `lock(mu); if (node.in_wait_list) list_remove(&node); unlock(mu);`
- Idempotent: if the waker already popped, `in_wait_list == 0` and the waiter does nothing.
- `in_wait_list` is a plain `int`. It is only read and written under `ch->mu`.
- Corollary: a popped node's `data`/`notified` fields remain valid because the node is stack-allocated on the parked fiber's coroutine stack, which is frozen until the fiber resumes.

### Invariant 2: No missed wakeups

**Correctness relies on the paired-fence Dekker protocol between the lock-free queue and the `has_send_waiters` / `has_recv_waiters` atomics, plus the scheduler's conditional-wait contract.**

The blocking phase pattern:

```
lock(mu);
// 1. Check closed → return EPIPE
// 2. Try direct handoff (pop partner waiter) → return 0
// 3. Try enqueue/dequeue under mu → return 0 (optimization: avoids append)
// 4. Steps 1–3 all failed: append waiter to list
//    — publishes has_send_waiters or increments has_recv_waiters (release)
// 5. unlock(mu)
// 6. seq_cst fence, then Dekker re-check: try enqueue/dequeue once more
//    — if succeeds: lock, remove self, signal partner, unlock, return 0
// 7. conditionally wait while node.notified == NONE
```

Steps 1–3 check the predicate under mu. Step 3 (retry-enqueue-under-mu) is an optimization that avoids the append+remove cost when space/data is immediately available.

Step 4 makes the waiter visible via the atomic flag. Step 6 is the critical
Dekker re-check: after the `release` update of `has_waiters`, the waiter
executes a `seq_cst` fence and probes the queue. The successful lock-free peer
operation executes the matching `seq_cst` fence before loading
`has_waiters`. Acquire/release operations alone do not prevent the store/load
reordering that permits both sides to miss on ARM64.

Step 7 uses the scheduler conditional-wait primitive specified in `spec/concurrent-c-scheduler.md`. If a waker changes `notified` between unlock and park, the wait does not sleep.

### Invariant 3: Close ordering

**`ch->closed` is checked under mu in the blocking path.**

- `cc_chan_close()` sets `ch->closed = 1` under mu, then wakes all waiters.
- Fast paths make an early `closed` check, but admission is resolved by revalidation and the in-flight enqueue protocol. A send already past the early check may complete and is then drained normally. Once set, `closed` stays set.
- Slow path: re-checks `ch->closed` under mu before appending waiter. Prevents parking after close has already woken everyone.

### Invariant 4: Timed cancellation and node lifetime

**A timed-out waiter must remove its node from the list before its stack frame returns.**

- Nodes are stack-allocated on the fiber's coroutine stack.
- On timeout: `lock(mu); if (node.in_wait_list) list_remove(&node); unlock(mu);` then return `ETIMEDOUT`.
- Race with concurrent waker: waker holds mu to pop and set `notified`. If waker wins, `in_wait_list == 0` and waiter's remove is a no-op; waiter sees `notified != 0` and handles normally. If waiter wins (removes self), waker's pop will not find the node.
- **Critical**: the waiter must not return (freeing the stack frame) until the node is off the list. The `lock(mu); if (in_wait_list) remove; unlock(mu)` guarantees this because the waker also holds mu.

### Invariant 5: OS-thread condvar protocol

**OS threads use `pthread_cond_wait` with the same predicate-under-mu pattern.**

- OS-thread callers do not use `FIBER_PARK_IF` or register a fiber wait node.
  Their blocking loop holds `mu`, checks the predicate, and calls
  `pthread_cond_wait(&condvar, &mu)`, which atomically releases `mu` and
  sleeps.
- On wake, the loop re-acquires mu and retries (handles spurious wakes).
- OS threads do not go on the fiber waiter lists. Receive-side lock-free
  condvar waiters publish `thread_recv_waiters` around the `not_empty` wait;
  successful enqueues observe that counter and signal `not_empty`. Senders use
  `not_full`.
- Waker calls `pthread_cond_signal` for single wake. Close calls `pthread_cond_broadcast` to wake all.

### Invariant 6: `lfqueue_inflight` and close-drain

**The close-drain path must not return EPIPE while any enqueue is in progress.**

- `lfqueue_inflight` is incremented before `lfds_enqueue` (relaxed) and decremented after (relaxed).
- Close-drain spins on: `while (lfqueue_count > 0 || lfqueue_inflight > 0) { try_dequeue; yield; }`
- Without `lfqueue_inflight`, a sender between the closed check and the CAS can complete after drain returns EPIPE, orphaning an item.

## Scheduler dependency

Channel blocking uses the conditional-wait and unpark contract in `spec/concurrent-c-scheduler.md`: pre-park predicate recheck, acquire observation of the wake publication, spurious-wake tolerance, and idempotent unpark. Channel loops always recheck their predicates after the wait returns.

## Memory ordering

- **`notified`**: waker `store_release`; waiter `load_acquire`.
- **Close state**: transitions occur under `ch->mu`; blocking paths recheck under that mutex. Lock-free admission races are resolved by `lfqueue_inflight` and close-drain rather than by treating the early `closed` check as the linearization point.
- **`lfqueue_count`**: `fetch_add/sub` with `release` on update; `load_acquire` on read.
- **`lfqueue_inflight`**: `fetch_add/sub` with `relaxed` (only used under the close-drain spin, which has its own ordering via `lfqueue_count`).
- **`has_send_waiters`**: boolean publication of an unnotified send waiter.
  **`has_recv_waiters`**: count of linked receive waiters with
  `notified == NONE`. Both use release updates under `mu`, paired `seq_cst`
  fences, and acquire fast-path reads as specified by Invariant 2. Wait-list
  pointers are plain and mutex-protected.
- **`thread_recv_waiters`**: OS-thread receivers currently waiting on
  `not_empty`; release counter updates and acquire sender reads avoid taking
  `mu` when no thread receiver needs a condvar signal.
- **`in_wait_list`**: plain `int`, only accessed under `ch->mu`. NOT atomic.
- **`select_group.selected_index`**: transitions via `CAS_acq_rel`.
- **`select_group.signaled`**: release-incremented wake sequence, observed with
  acquire ordering by conditional park.

## Implementation files

- `cc/runtime/channel.c`: Channel operations (buffered, unbuffered, lock-free, select)
- `cc/runtime/fiber_internal.h`: `cc__fiber_wait_node` definition
- `cc/include/ccc/cc_channel.cch`: Public API declarations
- `cc/include/ccc/cc_chan_handle.cch`: Directional handle wrappers (CCChanTx, CCChanRx)
