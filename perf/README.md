# Concurrent-C Performance & Robustness Benchmarks

This directory contains the runnable throughput benchmarks, scheduler diagnostics, and runtime stress-style microbenchmarks for Concurrent-C.

## Running The Suite

From the repo root:

```bash
./cc/bin/ccc run tools/run_all.ccs -- perf
./cc/bin/ccc run tools/run_all.ccs -- all
```

Run an individual benchmark directly:

```bash
./cc/bin/ccc run perf/perf_channel_throughput.ccs
./cc/bin/ccc run perf/channel_contention.ccs
./cc/bin/ccc run perf/work_stealing_efficiency.ccs
```

## Benchmark Inventory

### Channels And Message Passing

| Benchmark | What it measures |
|-----------|------------------|
| `perf_channel_throughput.ccs` | Buffered, unbuffered, and single-thread channel ops/sec. |
| `perf_buffered_base.ccs` | Minimal buffered 1 producer / 1 consumer throughput. |
| `perf_buffered_core.ccs` | Buffered throughput using low-level public runtime APIs. |
| `perf_buffered_ladder.ccs` | Loop vs fiber vs buffered-channel overhead ladder. |
| `perf_unbuffered_rendezvous_base.ccs` | Contended unbuffered rendezvous throughput and waiter churn. |
| `perf_zero_copy.ccs` | Payload-size sensitivity from `int` to large structs. |
| `perf_match_select.ccs` | `@match` / multi-channel select overhead. |
| `channel_contention.ccs` | Cross-channel interference between independent pipelines. |
| `channel_wake_wave.ccs` | Wake-to-run latency for one parked receiver per worker. |
| `thundering_herd.ccs` | Latency to wake a single waiter from a large herd. |
| `channel_fairness.ccs` | Distribution skew diagnostic for buffered wake behavior. |

### Spawn, Async, And Scheduler Overhead

| Benchmark | What it measures |
|-----------|------------------|
| `spawn_simple.ccs` | Minimal nursery spawn/join throughput. |
| `spawn_sequential.ccs` | Sequential spawn + join cost via `@async`. |
| `spawn_nursery.ccs` | Batched nursery spawn throughput. |
| `spawn_nursery_simple.ccs` | Nursery throughput with simpler task bodies. |
| `spawn_nursery_direct.ccs` | Nursery throughput with direct function calls. |
| `spawn_fiber_direct.ccs` | Raw internal fiber spawn/join throughput. |
| `perf_spawn_ladder.ccs` | Nursery vs `block_all` vs direct fiber overhead breakdown. |
| `perf_async_overhead.ccs` | Async task creation, execution, and blocking overhead. |
| `work_stealing_efficiency.ccs` | Cost of load balancing when work starts localized. |
| `perf_gobench_async_pressure.ccs` | Go-bench-style pressure from many parked async recv tasks. |
| `perf_gobench_blocking_pressure.ccs` | Parked waiters plus blocking-task scheduler pressure. |
| `fiber_overhead_profile.ccs` | Fiber vs thread overhead for heavy and minimal tasks. |

### Runtime Stress And Application Patterns

| Benchmark | What it measures |
|-----------|------------------|
| `arena_contention_storm.ccs` | Per-fiber private-arena allocation throughput. |
| `cancellation_avalanche.ccs` | Teardown speed and cleanup correctness for blocked task trees. |
| `mpmc_worker_pool.ccs` | Buffered producer -> worker-pool throughput and work distribution. |

## Scheduler And Robustness Comparisons

These compare Concurrent-C against pthread and Go baselines on scheduler fairness and robustness under adversarial workloads.

| Comparison | Script | What it measures | What the result highlights |
|-----------|--------|------------------|----------------------------|
| **Syscall Kidnapping** | `compare_syscall.sh` | Scheduler responsiveness when many OS workers are trapped in blocking syscalls. | Replacement workers keep the runtime making progress. |
| **Thundering Herd** | `compare_herd.sh` | Wake-up efficiency when many parked waiters are contending for one event. | Wake exactly one waiter instead of stampeding the herd. |
| **Channel Isolation** | `compare_contention.sh` | Cross-channel interference when independent pipelines are hammered concurrently. | Low coupling across wake/sleep, scheduler, and allocator paths. |
| **Channel Stability (4 workers)** | `contention_workers4_stability.sh` | Outlier frequency in the 4-worker channel-isolation case. | Tracks how often trials drift toward serial-like placement. |
| **Noisy Neighbor** | `compare_preemption.sh` | Scheduler fairness when one heartbeat task competes with CPU hogs that never yield. | Whether latency-sensitive work stays responsive under CPU pressure. |
| **Arena Allocation** | `compare_arena.sh` | Pure bump-pointer allocation throughput with private arenas and no shared allocator contention. | Measures the per-fiber arena strategy directly. |

Run the comparison suite:

```bash
./perf/run_neckbeard_challenges.sh
```

Latest results from the comparison suite:

```text
=================================================================
CONCURRENT-C: SCHEDULER AND ROBUSTNESS COMPARISONS
=================================================================
Running all robustness and fairness comparisons...

[1/5] Syscall Kidnapping Challenge...
-----------------------------------------------------------------
Implementation       Heartbeats
Pthread              54
Concurrent-C         55
Go                   54
-----------------------------------------------------------------

[2/5] Thundering Herd Challenge...
-----------------------------------------------------------------
Implementation       Avg Latency (ms)
Pthread              3.7678
Concurrent-C         0.0144
Go                   0.0136
-----------------------------------------------------------------

[3/5] Channel Isolation Challenge...
-----------------------------------------------------------------
Implementation       Interference
Pthread              41.20%
Concurrent-C         9.10%
Go                   -14.16%
-----------------------------------------------------------------

[4/5] Noisy Neighbor Challenge...
-----------------------------------------------------------------
Implementation       Heartbeats
Pthread              59
Concurrent-C         55
Go                   48
-----------------------------------------------------------------

[5/5] Arena Contention Challenge...
-----------------------------------------------------------------
Implementation       Throughput (M/sec)
Pthread (Arena)      699.79
Concurrent-C (Arena) 1011.12
Go (mcache)          4752.49
-----------------------------------------------------------------

=================================================================
ALL CHALLENGES COMPLETED
=================================================================
```

## Go Comparison

Use these scripts to compare against the Go runtime directly.

| Script | Description |
|--------|-------------|
| `compare_benchmarks.sh` | Runs equivalent CC and Go benchmarks and reports the performance ratio. |
| `run_go_benchmarks.sh` | Runs only the Go benchmarks under `perf/go/`. |

## Benchmarking Notes

1. Use release builds for meaningful numbers:
   ```bash
   ./cc/bin/ccc build --release my_test.ccs
   ```
2. Strip binaries when comparing footprint.
3. Keep the machine quiet when collecting timing numbers.
4. Tune worker count with `CC_WORKERS` when exploring scheduler behavior.

## Interpreting Results

- Negative interference in Channel Isolation means concurrent load did not slow the independent channel pairs down.
- High jitter in herd tests usually points to OS scheduling overhead rather than channel semantics.
- The Noisy Neighbor score is just total heartbeat ticks over the fixed run window, so higher is better.
