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
2. Check global queue
3. Randomized steal from other workers' local queues
4. Spin briefly, then sleep

This enables soft affinity: tasks prefer their assigned worker but can be stolen if that worker is busy.

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

- `CC_WORKERS=N`: Override initial worker count
- `CC_FIBER_STATS=1`: Print scheduler statistics at exit

### Compile-Time

- `CC_FIBER_WORKERS`: Fixed worker count (disables auto-detection)
- `CC_FIBER_STACK_SIZE`: Per-fiber stack size (default: platform-dependent)

### Debug Flags

- `CC_DEBUG_DEADLOCK`: Enable detailed fiber dump on deadlock detection
- `CC_DEBUG_SYSMON`: Log sysmon scaling decisions to stderr

## How It Works

### The Key Insights

1. **Round-robin distribution**: Spread spawns evenly across workers, don't pile onto one queue
2. **Always wake**: After every spawn, wake a sleeping worker (critical for latency!)
3. **Auto-scale to 2x**: Sysmon detects CPU-bound work and scales up 50% at a time
4. **Work stealing**: Automatic load balancing when distribution isn't perfect

### Why Round-Robin + Wake Wins

| Approach | Problem |
|----------|---------|
| Global queue only | All spawns contend, uneven pickup |
| Push to own queue | Spawner's queue overflows |
| Local queue, no wake | Workers sleep while tasks wait! |
| **Round-robin + wake** | Even distribution, immediate processing |

## Implementation Files

- `cc/runtime/fiber_sched.c`: Core scheduler implementation
- `cc/runtime/task.c`: CCTask API and spawn functions
- `cc/include/ccc/cc_sched.cch`: Public API declarations
