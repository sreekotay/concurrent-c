# CCTaskIntptr-Native Channel Operations Design

## Overview

This document describes the target architecture for channel operations in `@async` functions, where channel ops return `CCTaskIntptr` directly instead of being wrapped by the autoblock pass.

## Current State

### How Channel Ops Work Today

1. **User writes** (inside `@async`):
   ```c
   @async int producer(CCChanTx tx) {
       int rc = tx.send(42);
       return rc;
   }
   ```

2. **UFCS rewrites** to:
   ```c
   @async int producer(CCChanTx tx) {
       int rc = chan_send(tx, 42);
       return rc;
   }
   ```

3. **Autoblock pass wraps** (because `chan_send` is blocking):
   ```c
   @async int producer(CCChanTx tx) {
       int rc = (int)await cc_run_blocking_task_intptr(
           () => (void*)(intptr_t)chan_send(tx, 42)
       );
       return rc;
   }
   ```

4. **Async lowering** converts the function to a poll-based state machine.

### Problems with Current Approach

1. **Overhead**: Each channel op creates a closure, dispatches to threadpool, blocks an OS thread
2. **Complexity**: Autoblock has special-casing for channel ops (`CC_AB_REWRITE_AWAIT_OPERAND_CALL`)
3. **Generated code bloat**: Nested closure wrappers
4. **Semantic mismatch**: Sync-blocking impl forced into async via threadpool

## Target State

### User Experience

In `@async` functions, channel ops require explicit `await`:

```c
@async int producer(CCChanTx tx, CCChanRx rx) {
    // Send returns int (errno)
    int rc = await tx.send(42);
    if (rc != 0) return -1;
    
    // Recv also returns int
    int val;
    rc = await rx.recv(&val);
    if (rc != 0) return -2;
    
    return val;
}
```

In sync functions (non-`@async`), channel ops remain blocking:

```c
int sync_producer(CCChanTx tx) {
    int rc = tx.send(42);  // blocks OS thread
    return rc;
}
```

### Compiler Transformation

Inside `@async` functions, UFCS emits task-returning variants:

```c
// tx.send(42) inside @async becomes:
await cc_chan_send_task(tx.raw, &(int){42}, sizeof(int))

// rx.recv(&val) inside @async becomes:
await cc_chan_recv_task(rx.raw, &val, sizeof(val))
```

## Runtime API

### New Task-Returning Channel Operations

```c
// ---- cc_channel.cch additions ----

// Returns poll-based task; result is errno (0=success)
CCTaskIntptr cc_chan_send_task(CCChan* ch, const void* value, size_t value_size);
CCTaskIntptr cc_chan_recv_task(CCChan* ch, void* out_value, size_t value_size);

// With deadline support
CCTaskIntptr cc_chan_send_task_deadline(CCChan* ch, const void* value, size_t value_size, 
                                         const CCDeadline* deadline);
CCTaskIntptr cc_chan_recv_task_deadline(CCChan* ch, void* out_value, size_t value_size,
                                         const CCDeadline* deadline);
```

### Poll-Based Implementation

```c
// ---- channel.c additions ----

typedef struct {
    CCChan* ch;
    void* buf;           // for send: source; for recv: dest
    size_t elem_size;
    const CCDeadline* deadline;
    int is_send;         // 1=send, 0=recv
    int completed;
    int result;          // 0 success, errno on error
} CCChanTaskFrame;

static CCFutureStatus cc_chan_task_poll(void* frame, intptr_t* out_val, int* out_err) {
    CCChanTaskFrame* f = (CCChanTaskFrame*)frame;
    if (f->completed) {
        if (out_val) *out_val = (intptr_t)f->result;
        if (out_err) *out_err = f->result;
        return CC_FUTURE_READY;
    }
    
    // Check deadline
    if (f->deadline && cc_deadline_expired(f->deadline)) {
        f->completed = 1;
        f->result = ETIMEDOUT;
        if (out_val) *out_val = ETIMEDOUT;
        if (out_err) *out_err = ETIMEDOUT;
        return CC_FUTURE_READY;
    }
    
    int rc;
    if (f->is_send) {
        rc = cc_chan_try_send(f->ch, f->buf, f->elem_size);
    } else {
        rc = cc_chan_try_recv(f->ch, f->buf, f->elem_size);
    }
    
    if (rc == EAGAIN) {
        // Would block - return pending, let scheduler poll again
        return CC_FUTURE_PENDING;
    }
    
    // Completed (success or error)
    f->completed = 1;
    f->result = rc;
    if (out_val) *out_val = (intptr_t)rc;
    if (out_err) *out_err = rc;
    return CC_FUTURE_READY;
}

static int cc_chan_task_wait(void* frame) {
    // Block until the channel can make progress (for block_on from sync context)
    CCChanTaskFrame* f = (CCChanTaskFrame*)frame;
    // Use existing timed_send/recv with short timeout for non-busy waiting
    // Or use pthread_cond_timedwait on channel's condvar
    return cc_sleep_ms(1);  // Simple fallback
}

static void cc_chan_task_drop(void* frame) {
    free(frame);
}

CCTaskIntptr cc_chan_send_task(CCChan* ch, const void* value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !value || value_size == 0) return invalid;
    
    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;
    
    f->ch = ch;
    f->buf = (void*)value;  // Note: caller must ensure value outlives task
    f->elem_size = value_size;
    f->deadline = cc_current_deadline();
    f->is_send = 1;
    
    return cc_task_intptr_make_poll_ex(cc_chan_task_poll, cc_chan_task_wait, f, cc_chan_task_drop);
}

CCTaskIntptr cc_chan_recv_task(CCChan* ch, void* out_value, size_t value_size) {
    CCTaskIntptr invalid = {0};
    if (!ch || !out_value || value_size == 0) return invalid;
    
    CCChanTaskFrame* f = (CCChanTaskFrame*)calloc(1, sizeof(CCChanTaskFrame));
    if (!f) return invalid;
    
    f->ch = ch;
    f->buf = out_value;
    f->elem_size = value_size;
    f->deadline = cc_current_deadline();
    f->is_send = 0;
    
    return cc_task_intptr_make_poll_ex(cc_chan_task_poll, cc_chan_task_wait, f, cc_chan_task_drop);
}
```

## Compiler Changes

### UFCS Pass Changes (`pass_ufcs.c`)

When rewriting `tx.send(expr)` or `rx.recv(&expr)`, check if we're inside an `@async` function:

```c
// Pseudocode
if (inside_async_function && is_channel_method(method_name)) {
    if (method_name == "send") {
        // tx.send(val) -> cc_chan_send_task(tx.raw, &(__typeof__(val)){val}, sizeof(val))
        emit_task_variant();
    } else if (method_name == "recv") {
        // rx.recv(&out) -> cc_chan_recv_task(rx.raw, out, sizeof(*out))
        emit_task_variant();
    }
} else {
    // Normal blocking variant
    emit_chan_send_or_recv();
}
```

### Await Requirement

The task-returning variants mean `await` is now semantically meaningful:
- `tx.send(v)` in `@async` returns `CCTaskIntptr`, not `int`
- User must write `await tx.send(v)` to get the `int` result
- Without `await`, they'd be discarding the task (compile error or warning)

### Autoblock Simplification

With native task-returning channel ops:
- Remove `CC_AB_REWRITE_AWAIT_OPERAND_CALL` special-casing
- Remove channel op detection in autoblock
- Autoblock only wraps truly unknown blocking calls (e.g., `fread`, `sleep`)

## Frame Lifetime Considerations

### Problem: Value Lifetime

For `cc_chan_send_task`, the value pointer must remain valid until the task completes:

```c
@async int f(CCChanTx tx) {
    int x = 42;
    await tx.send(x);  // x must live until send completes
    return 0;
}
```

After async lowering, `x` lives in the state machine frame, so this works naturally.

### Alternative: Copy-into-Frame

For robustness, the task frame could copy small values:

```c
typedef struct {
    CCChan* ch;
    char value_storage[sizeof(intptr_t)];  // inline storage for small values
    void* value_ptr;                        // points to storage or external
    size_t elem_size;
    // ...
} CCChanTaskFrame;
```

For values <= sizeof(intptr_t), copy into frame. For larger, require caller guarantee.

## Migration Plan

### Phase 1: Add Runtime APIs
- Add `cc_chan_send_task` / `cc_chan_recv_task` to `channel.c`
- Add declarations to `cc_channel.cch`
- Add tests for new APIs

### Phase 2: UFCS Integration
- Add `inside_async_function` detection to UFCS pass
- Emit task-returning variants when inside `@async`
- Initially gate behind `CC_CHAN_TASK_NATIVE` flag

### Phase 3: Simplify Autoblock
- Remove channel op special-casing from `pass_autoblock.c`
- Remove `CC_AB_REWRITE_AWAIT_OPERAND_CALL`
- Ensure tests pass

### Phase 4: Default On
- Remove `CC_CHAN_TASK_NATIVE` flag
- Update all documentation
- Update recipes

## Benefits

1. **Performance**: No closure allocation, no threadpool dispatch, cooperative polling
2. **Simpler codegen**: Direct task returns instead of wrapped closures
3. **Cleaner architecture**: Channel ops naturally async, autoblock only for legacy blocking
4. **Better diagnostics**: Clear error if `await` missing on task-returning call

## Open Questions

1. **Backward compatibility**: Old code without `await` on channel ops would break. Need migration period or auto-await in UFCS?

2. **Sync fallback**: When `cc_block_on_intptr` is called on a channel task, should it:
   - Busy-poll with sleep (current approach)
   - Use channel's condvar directly (more efficient but couples impl)

3. **Buffer management**: For values > sizeof(intptr_t), who ensures lifetime?
   - Option A: Always copy into frame (memory overhead)
   - Option B: Require stable storage (user responsibility)
   - Option C: Use arena-allocated frame storage

4. **Select/match integration**: Does `@match` emit task-returning variants or blocking?
