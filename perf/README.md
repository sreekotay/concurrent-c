# Concurrent-C Performance Benchmarks

Focused benchmarks that measure throughput and latency. Use these to catch performance regressions.

## Benchmarks

| Benchmark | What it measures |
|-----------|------------------|
| `perf_channel_throughput` | Channel ops/sec: buffered, unbuffered, single-thread |
| `perf_async_overhead` | Async task creation, state machine transitions, cc_block_all |
| `perf_match_select` | @match multi-channel select latency |
| `perf_zero_copy` | Slice copy throughput by payload size |
| `perf_gobench_blocking_pressure` | GoBench-derived scheduler + auto-blocking pressure |
| `spawn_nursery` | Nursery-based spawn throughput benchmark |
| `spawn_sequential` | Sequential spawn+join throughput benchmark |

### Go Benchmarks (`perf/go/`)

| Benchmark | What it measures |
|-----------|------------------|
| `spawn_nursery.go` | Nursery-based goroutine spawn throughput |
| `spawn_sequential.go` | Sequential goroutine spawn+join throughput |
| `channel_throughput.go` | Channel operations throughput (single-thread, buffered, unbuffered) |
| `perf_gobench_async_pressure` | GoBench-derived async scheduler pressure |

## Running

```bash
# Run all Concurrent-C benchmarks
./perf/run_benchmarks.sh

# Run all Go benchmarks
./perf/run_go_benchmarks.sh

# Run comparison between Concurrent-C and Go
./perf/compare_benchmarks.sh

# Run a specific benchmark
./cc/bin/ccc run perf/perf_channel_throughput.ccs

# Compare before/after a change
./cc/bin/ccc run perf/perf_channel_throughput.ccs > before.txt
# ... make changes ...
./cc/bin/ccc run perf/perf_channel_throughput.ccs > after.txt
diff before.txt after.txt
```

## Expected Baselines

These are rough baselines on typical hardware (M1/M2 Mac, Linux x64). Actual numbers vary.

### Channel Throughput
- Single-thread (no contention): **40M+** ops/sec
- Buffered (cap=1000): **20M+** ops/sec
- Unbuffered (rendezvous): **750K+** ops/sec

### Async Overhead  
- Trivial async (no yields): **15M+** tasks/sec
- Compute async (100 iter): **300K+** tasks/sec
- cc_block_all (batch=10): **100K+** tasks/sec
- spawn baseline: **15K+** spawns/sec

### Match Select
- Single channel: **10M+** ops/sec
- 2-channel select: **8M+** ops/sec
- 4-channel select: **6M+** ops/sec

### Payload Size Impact
- int (4 bytes): **18M+** ops/sec
- Small (16 bytes): **19M+** ops/sec, ~300 MB/sec
- Medium (256 bytes): **18M+** ops/sec, ~4 GB/sec
- Large (1KB): **11M+** ops/sec, ~11 GB/sec

**Note:** These numbers reflect our optimized signal-based implementation (no 1ms polling).

## Go Comparison

Use `./perf/compare_benchmarks.sh` to run equivalent benchmarks in Go and compare performance:

```bash
$ ./perf/compare_benchmarks.sh
Concurrent-C vs Go Performance Comparison
==========================================

Benchmark                  Concurrent-C   Go             Ratio
--------------------------------------------------------------------------------
spawn_nursery              468790 spawns/sec  4347042 spawns/sec   0.10
spawn_sequential           26109668 spawns/sec  3795052 spawns/sec   6.87
channel_throughput         61162080 ops/sec  57836769 ops/sec   1.05

Note: Ratio shows Concurrent-C performance relative to Go (higher = better)
```

**Current Status:**
- **Channel throughput**: Competitive with Go (1.05x = 5% faster)
- **Sequential spawns**: Much faster than Go (6.87x) - due to @async optimization
- **Nursery spawns**: Much slower than Go (0.10x) - needs fiber scheduler optimization

## Red Flags

If you see:
- **>50% drop** in ops/sec → likely regression
- **Order of magnitude slower** → something is very wrong
- **Inconsistent numbers** between runs → likely contention or timing issue

## Adding New Benchmarks

Good benchmarks should:
1. Measure ONE thing clearly
2. Report ops/sec or latency
3. Be deterministic
4. Complete in <5 seconds
5. Use `clock_gettime(CLOCK_MONOTONIC)` for timing
