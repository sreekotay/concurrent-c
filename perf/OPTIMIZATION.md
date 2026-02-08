# Performance Optimization: Fiber Scheduler

This document captures the performance work done to close the gap with Go's goroutine scheduler for spawn/channel operations.

## Overview

**Goal**: Close the performance gap with Go for spawn/channel operations.

**Approach**: Replace per-task pthread mutex/cond synchronization with a lightweight fiber scheduler using:
- Lock-free task queues
- Task pooling (avoid malloc/free per spawn)
- Atomic completion flags with spin-wait

**Results**: 7-10x improvement in spawn throughput, reaching 86% of Go's performance.

## Architecture

The fiber scheduler consists of three main components:

### 1. `fiber_sched.c` - Control Word States

Defines the fiber lifecycle control word:

```c
// control word values
CTRL_IDLE     // Available for assignment
CTRL_ASSIGNED // Enqueued/assigned to a worker
CTRL_OWNED    // Actively executing on a worker
CTRL_PARKED   // Blocked (e.g., waiting on channel)
CTRL_DONE     // Completed
```

### 2. `fiber.c` - Cross-Platform Fiber Primitives

Platform-specific stackful coroutine implementations:

| Platform | Implementation | Notes |
|----------|----------------|-------|
| Windows | Fibers API | Native `CreateFiber`/`SwitchToFiber` |
| Linux/macOS x86_64 | ucontext | `makecontext`/`swapcontext` |
| ARM64 (Apple Silicon) | Cooperative | Run-to-completion (no stack switching) |

Key primitives:
- `cc__fiber_create()` - Allocate fiber with 64KB stack
- `cc__fiber_switch_to()` - Context switch to fiber
- `cc__fiber_yield()` - Return to scheduler
- `cc__fiber_park()`/`cc__fiber_unpark()` - Block/wake for channel ops

### 3. `fiber_sched.c` - Work-Stealing Task Scheduler

The heart of the optimization:

```
┌─────────────────────────────────────────────────────────┐
│                    Fiber Scheduler                       │
├─────────────────────────────────────────────────────────┤
│  ┌──────────┐   ┌──────────┐       ┌──────────┐        │
│  │ Worker 0 │   │ Worker 1 │  ...  │ Worker N │        │
│  └────┬─────┘   └────┬─────┘       └────┬─────┘        │
│       │              │                  │              │
│       └──────────────┼──────────────────┘              │
│                      ▼                                 │
│            ┌─────────────────┐                         │
│            │  MPMC Queue     │ Lock-free, 4096 slots   │
│            └─────────────────┘                         │
│                      │                                 │
│            ┌─────────┴─────────┐                       │
│            ▼                   ▼                       │
│    ┌─────────────┐     ┌─────────────┐                │
│    │  Task Pool  │     │  Free List  │                │
│    │  (active)   │     │  (recycle)  │                │
│    └─────────────┘     └─────────────┘                │
└─────────────────────────────────────────────────────────┘
```

**Key optimizations**:

1. **Lock-free MPMC Queue**: Atomic head/tail with CAS operations
   ```c
   static inline fiber_task* queue_pop(task_queue* q) {
       size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
       // ... CAS to claim slot ...
   }
   ```

2. **Task Pooling**: Reuse task structs via atomic free list
   ```c
   static fiber_task* task_alloc(void) {
       // Try free list first (lock-free)
       fiber_task* t = atomic_load(&g_sched.free_list);
       while (t) {
           if (atomic_compare_exchange_weak(&g_sched.free_list, &t, t->next)) {
               return t;  // Reused!
           }
       }
       return calloc(1, sizeof(fiber_task));  // Fallback
   }
   ```

3. **Atomic Completion Flags**: No per-task mutex/condvar
   ```c
   typedef struct fiber_task {
       void* (*fn)(void*);
       void* arg;
       void* result;
       _Atomic int done;      // ← Replaces pthread_mutex_t + pthread_cond_t
       _Atomic int waiting;
       struct fiber_task* next;
   } fiber_task;
   ```

4. **Spin-Wait with Exponential Backoff**: Efficient join
   ```c
   int cc_fiber_join(fiber_task* t, void** out_result) {
       // Fast path
       if (atomic_load_explicit(&t->done, memory_order_acquire))
           return 0;
       
       // Spin with backoff
       for (int spins = 0; spins < 1000; spins++) {
           if (atomic_load_explicit(&t->done, memory_order_acquire))
               return 0;
           for (volatile int i = 0; i < (1 << (spins / 100)); i++) {}
       }
       
       // Yield loop for long waits
       while (!atomic_load_explicit(&t->done, memory_order_acquire))
           sched_yield();
   }
   ```

## Benchmark Results

### Before (pthread mutex/cond per task)

| Benchmark | Throughput | Notes |
|-----------|------------|-------|
| spawn_nursery | 368 K/s | Per-task mutex acquisition |
| spawn_sequential | 58 K/s | Mutex + condvar wait |
| chan_single_thread | 11.9 M/s | Channel ops only |

### After (fiber scheduler) - Current Results

| Benchmark | Throughput | Improvement | vs Go | Status |
|-----------|------------|-------------|-------|--------|
| spawn_nursery | 455 K/s | **1.2x** vs baseline | 7% | Fiber scheduler |
| spawn_sequential | 15.8 M/s | **38x** vs baseline | - | Recreated (@async) |
| chan_single_thread | 42.8 M/s | **3.6x** | - | Updated |
| spawn_baseline | 408 K/s | - | - | Current |
| trivial_async | 10.5 M/s | - | - | Current |

**Note**: Implemented true M:N model by switching nursery from thread-per-task (`cc_thread_spawn`) to fiber scheduler (`cc_fiber_spawn_task`). This eliminates per-task mutex/cond overhead and provides work-stealing task queues. The remaining 93% gap to Go is due to Go's highly optimized runtime with true goroutine context switching.

### What Changed

| Component | Before | After |
|-----------|--------|-------|
| Task allocation | `malloc()` per spawn | Lock-free pool |
| Completion sync | `pthread_mutex_t` + `pthread_cond_t` | `_Atomic int` |
| Join wait | `pthread_cond_wait()` | Spin + `sched_yield()` |
| Queue | Mutex-protected deque | Lock-free MPMC |

## Implemented Optimizations (Phase 2)

All high and medium impact optimizations have been implemented:

### 1. Fiber-Aware Channel Blocking ✅

**Status**: Implemented in `channel.c`

- Channels now detect fiber context via `cc__fiber_in_context()`
- When blocking in fiber context: `cc__fiber_park()` instead of condvar wait
- Fiber wait queues added to channel struct
- Wake operations use `cc__fiber_unpark()` to resume blocked fibers
- Falls back to condvar for non-fiber contexts and timed waits

### 2. True Context Switching on ARM64 ✅

**Status**: Implemented in `fiber.c`

- Full inline assembly for ARM64 context switching
- Saves/restores all callee-saved registers (x19-x28, x29, x30, SP)
- Saves/restores SIMD callee-saved registers (d8-d15)
- Stack allocated via `mmap` with guard page protection
- Proper 16-byte stack alignment per ARM64 ABI

### 3. SPSC Channel Fast Path ✅

**Status**: Re-enabled in `channel.c`

- Lock-free send/recv for 1:1 topology channels
- Brief spin with CPU pause hints before falling back to mutex
- Atomic head/tail with proper memory ordering
- Wakes fiber waiters when data available

### 4. Per-Worker Local Queues ✅

**Status**: Implemented in `fiber_sched.c`

- Chase-Lev work-stealing deque per worker
- Owner pushes/pops from bottom (LIFO for cache locality)
- Thieves steal from top (FIFO)
- Priority: local queue → global queue → steal from others
- Random victim selection to avoid herding

### 5. Adaptive Spinning ✅

**Status**: Implemented in `fiber_sched.c`

- Per-thread spin state tracking (EMA of spins to completion)
- Dynamic max spin count adjustment (64-2000 range)
- Spin budget increases when tasks complete quickly
- Spin budget decreases when tasks take too long
- Pause instruction hints with increasing backoff

### 6. Batch Wake Operations ✅

**Status**: Implemented in `channel.c`

- Thread-local wake batch buffer (16 fibers)
- Accumulates wake requests during channel operations
- Flushes batch after send/recv completes
- Reduces scheduler enqueue overhead for fan-out patterns

## Remaining Work

### High Priority - Performance Investigation

1. **Performance Regression Investigation** 🔴
   - Current `perf_gobench_blocking_pressure` shows only 1032/5000 tasks completing in 4s
   - Unbuffered channel throughput at 340K ops/sec (expected much higher)
   - Nursery spawn at 388K/s (expected 3.8M/s based on original 10x improvement)
   - Investigate potential issues with fiber scheduling or channel implementation

2. **Benchmark Validation** ⚠️
   - Verify recreated benchmarks match original test patterns
   - The 15.8M/s sequential result uses @async functions (different from raw spawn)
   - Need to create raw spawn sequential benchmark for accurate comparison

### Low Impact / Future

1. **io_uring Integration** (Linux)
   - Kernel bypass for I/O operations
   - True async I/O without blocking worker threads

2. **Coroutine-Based Async Runtime**
   - Replace thread-per-task with stackless coroutines
   - Lower memory footprint for high fan-out

## Technical Details

### Memory Layout

```
fiber_task (32 bytes):
  ├── fn:      8 bytes  (function pointer)
  ├── arg:     8 bytes  (argument)
  ├── result:  8 bytes  (return value)
  ├── done:    4 bytes  (atomic completion flag)
  ├── waiting: 4 bytes  (waiter count)
  └── next:    8 bytes  (free list link)

cc__fiber (platform-dependent):
  ├── ctx:        8 bytes   (platform handle)
  ├── stack:      8 bytes   (stack pointer, POSIX only)
  ├── stack_size: 8 bytes
  ├── fn:         8 bytes
  ├── arg:        8 bytes
  ├── result:     8 bytes
  ├── state:      4 bytes
  └── next:       8 bytes   (ready queue link)
```

### Thread Safety

- Task pool: Lock-free CAS on free list head
- Task queue: Lock-free MPMC with atomic head/tail
- Completion: Release/acquire semantics on `done` flag
- Wake mechanism: Single condvar for idle workers only

### Configuration

```c
#define FIBER_SCHED_MAX_WORKERS 64      // Max worker threads
#define FIBER_SCHED_QUEUE_SIZE 4096     // Task queue capacity
#define CC__FIBER_STACK_SIZE (64*1024)  // 64KB per fiber stack
```

Workers auto-scale to `sysconf(_SC_NPROCESSORS_ONLN)`.

## Appendix: Go Comparison

Go's scheduler achieves ~4.4 M spawns/sec on similar hardware. Key differences:

| Aspect | Go | Concurrent-C |
|--------|-----|--------------|
| Context switch | Custom assembly | ucontext/Fibers API |
| Stack growth | Segmented/copyable | Fixed 64KB |
| Preemption | Async (Go 1.14+) | Cooperative |
| Work stealing | Per-P local queues | Global MPMC |

The 14% gap is primarily due to:
1. Go's hand-tuned assembly context switch
2. Segmented stacks allowing more goroutines
3. Integrated runtime with compiler cooperation

## Final Assessment: M:N Fiber Scheduler Attempt

**Attempted**: Implement true M:N threading with fiber scheduler to replace thread-per-task model.

**Result**: Failed due to excessive overhead and implementation complexity.

**Key Issues**:
- Fiber scheduler caused test timeouts and hangs
- 16x performance penalty vs simple thread-per-task
- Complex work-stealing and atomic operations added too much overhead
- Nursery reverted to working thread-per-task executor

**Conclusion**: Achieving Go-like M:N performance requires:
- Assembly-level context switching (not C function calls)
- Runtime deeply integrated with compiler
- Years of performance tuning and optimization
- Fundamental architectural changes beyond what can be easily implemented in C

**Current State**: Nursery uses reliable thread-per-task executor. Fiber scheduler code preserved for future advanced implementations.
