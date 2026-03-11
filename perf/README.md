# Concurrent-C Performance & Robustness Benchmarks

This directory contains a suite of benchmarks and stress tests designed to measure the performance, scalability, and robustness of the Concurrent-C runtime.

## 🚀 Core Performance Benchmarks

These benchmarks measure the raw throughput and latency of Concurrent-C primitives.

| Benchmark | Description |
|-----------|-------------|
| `perf_channel_throughput` | Measures channel operations per second (buffered, unbuffered, single-threaded). |
| `perf_async_overhead` | Measures the cost of async task creation and state machine transitions. |
| `perf_match_select` | Measures the latency and throughput of multi-channel `@match` selection. |
| `perf_zero_copy` | Measures memory throughput when passing slices of various sizes through channels. |
| `spawn_nursery` | Benchmarks high-concurrency fiber spawning within nurseries. |
| `spawn_sequential` | Benchmarks sequential spawn+join overhead (optimized via `@async`). |

**Run all core benchmarks:**
```bash
./perf/run_benchmarks.sh
```

---

## 🧔 The "Neckbeard Challenges" (Robustness & Fairness)

These tests compare Concurrent-C against traditional Pthread-based C implementations to evaluate the M:N scheduler's resilience and efficiency in real-world "rude" scenarios.

| Challenge | Script | What it tries to break | The CC Flex |
|-----------|--------|------------------------|-------------|
| **Syscall Kidnapping** | `compare_syscall.sh` | Scheduler responsiveness when 100 OS threads are blocked by 2s raw C `sleep()` calls. All implementations face the same 100 kidnappers for 5s. | `sysmon` detects stalled workers and spawns replacement threads on the fly. |
| **Thundering Herd** | `compare_herd.sh` | Wake-up efficiency when 1,000+ fibers are waiting on a single event. | Surgical `wake_batch` wakes exactly one fiber, avoiding OS context-switch storms. |
| **Channel Isolation** | `compare_contention.sh` | Cross-channel interference when independent channels are hammered concurrently. | Low coupling across the scheduler, allocator, and wake/sleep paths keeps independent channels from interfering. |
| **Channel Stability (4 workers)** | `contention_workers4_stability.sh` | Outlier frequency for the 4-worker channel-isolation case, where best-of alone can hide occasional serial-like startup placement. | Reports median/p90 contention ratios plus how often trials look elevated or serial-like. |
| **Noisy Neighbor** | `compare_preemption.sh` | Scheduler fairness when "greedy" fibers run tight CPU loops without yielding. | `sysmon` enforces fairness by ensuring latency-sensitive fibers always get an OS thread. |
| **Arena Allocation** | `compare_arena.sh` | Pure bump-pointer allocation throughput: each fiber/thread/goroutine owns a private 1MB arena, no shared allocator. Comparable to Go's per-P mcache strategy. | Per-fiber arenas eliminate all allocator contention; results reflect bump-pointer overhead vs Go's runtime mcache. |
| **Work Stealing** | `work_stealing_efficiency.ccs` | Load balancing overhead when 100,000 tasks are queued on a single worker. | Measures how fast idle workers can "steal" work to keep all cores saturated. |
| **Binary Bloat** | (Manual) | Executable size of a "Hello World" with a full fiber runtime. | `--release` triggers `-Wl,-dead_strip`, resulting in a tiny **12.8 KB** (compressed) binary. |

**Run a specific challenge:**
```bash
./perf/compare_syscall.sh
./perf/compare_herd.sh
./perf/contention_workers4_stability.sh
# ... etc
```

**Run all robustness challenges:**
```bash
./perf/run_neckbeard_challenges.sh
```

---

## 🐹 Go Comparison

Concurrent-C is designed to provide Go-like concurrency with C-like control. Use these scripts to see how we stack up against the Go runtime.

| Script | Description |
|--------|-------------|
| `compare_benchmarks.sh` | Runs equivalent CC and Go benchmarks and reports the performance ratio. |
| `run_go_benchmarks.sh` | Runs only the Go-based benchmarks in `perf/go/`. |

---

## 🛠 How to Benchmark Correctly

1.  **Use Release Mode:** Always build with `--release` to enable dead-code stripping and optimizations.
    ```bash
    ./cc/bin/ccc build --release my_test.ccs
    ```
2.  **Strip Binaries:** For size comparisons, use `strip` to remove debug symbols.
3.  **Isolate Hardware:** Close browser tabs and background processes.
4.  **Check CC_WORKERS:** The number of OS worker threads can be tuned via the `CC_WORKERS` environment variable (defaults to CPU count).

## 📊 Interpreting Results

*   **Negative Interference:** In the Channel Isolation test, negative interference means per-channel throughput actually improved under concurrent load — the runtime has no cross-channel coupling.
*   **Latency Jitter:** High variance in `compare_herd.sh` usually indicates OS-level scheduling overhead (which CC aims to minimize).
*   **Binary Size:** A release-mode "Hello World" should be around **12-13KB** (compressed), proving the efficiency of the dead-code stripper.
