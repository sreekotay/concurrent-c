# Pigz Porting Plan: From Idiomatic to Feature-Complete

This document outlines the strategy for evolving the current "idiomatic" Concurrent-C `pigz` port into a production-ready, feature-complete replacement for Mark Adler's original `pigz`.

## 1. Key Concepts: Structural Lifetimes

The core strength of the Concurrent-C implementation is that the **lifetime of memory** and the **lifetime of tasks** are explicitly bound to the **structure of the code**.

### Task Lifetime (`@nursery`)
- **Current State:** Uses nested nurseries to manage Reader, Worker, and Writer lifecycles.
- **Goal:** Maintain this structure while adding complex coordination for signals (Ctrl+C) and graceful shutdown.

### Memory Lifetime (`@arena`)
- **Current State:** Each block/result pair is backed by a discrete `CCArena`.
- **Goal:** Implement "Ownership Transfer" patterns where arenas are handed off through channels, ensuring zero-copy performance without manual `free()` management.

## 2. The "Last 10%" Features

To achieve 1:1 parity with original `pigz`, the following must be implemented:

### Dictionary Chaining (Sequential Dependency)
Original `pigz` achieves better compression by using the last 32KB of block $N$ as a dictionary for block $N+1$.
- **Implementation:** Use a "Bucket Brigade" of small channels or a chain of `Future<CCSlice>` handles.
- **Parallelism Impact:** High. Since the dependency is on the *raw input* (available immediately after reading) rather than the *compressed output*, workers can still run in parallel with only a nanosecond-scale staggered start.

### Metadata & Signal Handling
- **File Attributes:** A dedicated fiber to sync `chmod`, `chown`, and `utimes` after the Writer completes.
- **Signals:** A signal-handling fiber that triggers nursery cancellation to ensure clean exit and partial file cleanup.

## 3. Test Plan vs. Original Pigz

Verification is critical to prove that the "Idiomatic" version is both correct and faster.

### Correctness Testing
1. **Binary Identity:** For a given block size and compression level, the output should be byte-for-byte identical to `pigz --independent` (or identical to standard `pigz` once dictionary chaining is implemented).
   ```bash
   ./pigz_idiomatic input.bin -o output.ccs.gz
   pigz -c input.bin > output.orig.gz
   cmp output.ccs.gz output.orig.gz
   ```
2. **Decompression Compatibility:** Ensure standard `gzip -d` and `gunzip` can decompress the output without errors.
3. **Dictionary Validation:** Compare compression ratios. If the CC version produces larger files than the original, dictionary chaining is likely broken or missing.

### Performance Benchmarking
1. **Throughput (MB/s):** Measure against original `pigz` across varying thread counts (`-p 1`, `-p 4`, `-p 16`, `-p 64`).
2. **Memory Scaling:** Monitor RSS (Resident Set Size). The CC version should show linear memory scaling with the reorder buffer size, whereas the original may show more fragmentation over long runs.
3. **Latency:** Use `time` to measure total execution. The goal is to maintain or exceed the current **10% speedup**.

### Stress Testing
- **Large Files:** Test with files > 4GB to verify 64-bit offset handling.
- **Pipe Pressure:** Run as part of a shell pipeline: `cat giant_file | ./pigz_idiomatic | wc -c`.
- **Disk Full:** Simulate disk full errors to ensure the CC `Result` types correctly propagate errors and the `@nursery` cleans up partial files.
