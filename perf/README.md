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
| **Syscall Kidnapping** | `compare_syscall.sh` | Scheduler responsiveness when OS threads are blocked by raw C syscalls (like `usleep`). | `sysmon` detects stalled workers and spawns replacement threads on the fly. |
| **Thundering Herd** | `compare_herd.sh` | Wake-up efficiency when 1,000+ fibers are waiting on a single event. | Surgical `wake_batch` wakes exactly one fiber, avoiding OS context-switch storms. |
| **Cache-Line Contention** | `compare_contention.sh` | Memory layout efficiency and false sharing between adjacent channel structs. | Beefy 592-byte `CCChan` struct naturally spans ~10 cache lines, making false sharing impossible. |
| **Noisy Neighbor** | `compare_preemption.sh` | Scheduler fairness when "greedy" fibers run tight CPU loops without yielding. | `sysmon` enforces fairness by ensuring latency-sensitive fibers always get an OS thread. |
| **Arena Contention** | `compare_arena.sh` | Allocation throughput of the atomic `CCArena` CAS loop vs. system `malloc`. | Idiomatic per-fiber arenas use simple bump-pointer math that consistently beats `malloc`. |
| **Work Stealing** | `work_stealing_efficiency.ccs` | Load balancing overhead when 100,000 tasks are queued on a single worker. | Measures how fast idle workers can "steal" work to keep all cores saturated. |
| **Binary Bloat** | (Manual) | Executable size of a "Hello World" with a full fiber runtime. | `--release` triggers `-Wl,-dead_strip`, resulting in a tiny **12.8 KB** (compressed) binary. |

**Run a specific challenge:**
```bash
./perf/compare_syscall.sh
./perf/compare_herd.sh
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

*   **Negative Throughput Drop:** In contention tests, a negative drop means the system scaled well across multiple cores (total throughput increased).
*   **Latency Jitter:** High variance in `compare_herd.sh` usually indicates OS-level scheduling overhead (which CC aims to minimize).
*   **Binary Size:** A release-mode "Hello World" should be around **12-13KB** (compressed), proving the efficiency of the dead-code stripper.
