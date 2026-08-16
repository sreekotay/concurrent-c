# pigz_cc: Parallel Gzip in Concurrent-C

This project provides two implementations of parallel gzip compression:

1. **Original pigz** - Mark Adler's pthread-based implementation
2. **pigz_cc** - Native Concurrent-C rewrite using structured concurrency

## Quick Start

```bash
# 1. Download original pigz source
chmod +x setup.sh
./setup.sh

# 2. (Optional) Download Silesia benchmark corpus (~65 MB zip)
chmod +x download_silesia.sh
./download_silesia.sh

# 3. Build both versions
make pigz      # Original (requires zlib, pthreads)
make pigz_cc   # CC version (requires CC compiler + zlib)

# 4. Run benchmark
./benchmark.sh 200 8 3   # <size_mb> <workers> <runs>

# 5. Linux i386 (Docker) — build + correctness + compare vs original pigz
../../scripts/pigz_i386.sh
# Optional: PIGZ_BENCH_MB=50 PIGZ_BENCH_WORKERS=8 PIGZ_BENCH_RUNS=3 ../../scripts/pigz_i386.sh
```

## Benchmark Data (auto-downloaded, not checked in)

`benchmark.sh` uses a **real multi-file corpus** (Silesia corpus) and generates a single input file by concatenating corpus files until it reaches the requested size.

- **Download manually**: `./download_silesia.sh` (or let `benchmark.sh` fetch on first run)
- **Source URL**: http://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
- **Downloaded/extracted to**: `testdata/silesia/` (and `testdata/silesia.zip`)
- **Generated input**: `testdata/text_<size_mb>mb.bin`
- **Not checked in**: benchmark inputs are ignored via `.gitignore`

## Architecture Comparison

### Original pigz (pthread)
```
                    ┌─────────────┐
                    │   Reader    │
                    │  (main)     │
                    └──────┬──────┘
                           │ job list
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ Worker 1 │ │ Worker 2 │ │ Worker N │
        │(pthread) │ │(pthread) │ │(pthread) │
        └────┬─────┘ └────┬─────┘ └────┬─────┘
             │            │            │
             └────────────┼────────────┘
                          │ sorted results
                    ┌─────▼─────┐
                    │  Writer   │
                    │ (pthread) │
                    └───────────┘
```

Uses locks (mutex + condvar) via yarn.h for synchronization.

### pigz_cc (Concurrent-C)
```
                    ┌─────────────┐
                    │   Reader    │
                    │  (spawn)    │
                    └──────┬──────┘
                           │ blocks channel
              ┌────────────┼────────────┐
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │ Worker 1 │ │ Worker 2 │ │ Worker N │
        │ (spawn)  │ │ (spawn)  │ │ (spawn)  │
        └────┬─────┘ └────┬─────┘ └────┬─────┘
             │            │            │
             └────────────┼────────────┘
                          │ results channel
                    ┌─────▼─────┐
                    │  Writer   │
                    │  (spawn)  │
                    └───────────┘
```

Uses channels and explicit ownership scopes for clean structured concurrency:

```c
CCNursery* writer@(NULL, writer_task) @destroy;
CCNursery* pipeline@(writer) @destroy {
    results_tx.close();
};

pipeline->spawn(() => { compress_block(); });
pipeline->spawn(() => { read_blocks(); });
```

## CC Patterns Demonstrated

### 1. Channel Pipeline
Blocks flow through typed channels - no shared mutable state:
```c
Block[~4 >] blocks_tx;    // Send handle
Block[~4 <] blocks_rx;    // Receive handle
CCChan* ch = cc_channel_pair(&blocks_tx, &blocks_rx);
```

### 2. Nested Ownership with Explicit Close
Close channels when producer-owned work finishes:
```c
CCNursery* producer@(consumer) @destroy {
    results_tx.close();
};
// Producer-owned work runs here.
// Consumer drains results_rx after close and then exits.
```

### 3. Parallel Workers via `n->spawn()`
No thread management - structured lifetime:
```c
for (int w = 0; w < num_workers; w++) {
    workers->spawn(() => [level, blocks_rx, results_tx] {
        Block blk;
        while (cc_io_avail(blocks_rx.recv(&blk))) {
            Result res = compress_block(&blk, level);
            (void)results_tx.send(res);
        }
    });
}
```

## Files

| File | Description |
|------|-------------|
| `setup.sh` | Downloads original pigz source |
| `download_silesia.sh` | Downloads/extracts Silesia corpus to `testdata/silesia/` |
| `Makefile` | Builds all versions |
| `benchmark.sh` | Main benchmark (auto-downloads corpus + prints summary table) |
| `bench_go_vs_cc.sh` | Cross-language comparison: CC vs Go vs Zig vs original |
| `bench_compress_only.sh` | Compression-only timing |
| `pigz_idiomatic.ccs` | **Idiomatic CC pipeline — read this first** (ordered channel + send_task) |
| `pigz_parallel.ccs` | CCTurnstileRW(n, cap) + spawn |
| `CCTurnstile.cch` | pigz-local gate: one core (depth + stages[]); RW bind is aliases |
| `pigz_cc/pigz_cc.ccs` | Feature-complete CC port (parity binary) |
| `pigz_hybrid.ccs` | Idiomatic pipeline on the V2 hybrid scheduler |
| `pigz_pthread.ccs` | Same pipeline on OS threads (`cc_thread_spawn`) |
| `pigz_fiber_directjoin.ccs` / `pigz_thread_directjoin.ccs` | Ladder variants: direct FIFO joins, no channel |
| `pigz_unordered.ccs` | Benchmark-only control: no ordered delivery (output corrupt by design) |
| `pigz_profile.ccs` / `pigz_pthread_profile.ccs` | Profiling variants |
| `pigz_go.go` | Go port (CGO + zlib) for comparison |
| `pigz_zig.zig` | Zig port for comparison |
| `pigz_c/pigz.c` | Original (downloaded by `setup.sh`) |
| `pigz_c/yarn.c/h` | Original thread layer |
| `pigz_c/try.c/h` | Original error handling |

## Usage

Both versions support similar options:

```bash
# Compress file
./pigz -p 4 file.txt        # -> file.txt.gz
./pigz_cc -p 4 file.txt     # -> file.txt.gz

# To stdout
./pigz -c file.txt > out.gz
./pigz_cc -c file.txt > out.gz

# Keep original
./pigz -k file.txt
./pigz_cc -k file.txt
```

## Building

### Prerequisites
- GCC with pthread support
- zlib development headers (`zlib.h`)
- zopfli development headers (`zopfli.h`) for the full `pigz_cc` port
- CC compiler (`ccc`) for pigz_cc

### macOS
```bash
brew install zlib zopfli
make
```

### Linux
```bash
sudo apt-get install zlib1g-dev zopfli libzopfli-dev
make
```

If zopfli is installed outside the default compiler search paths, pass
`ZOPFLI_CFLAGS` and `ZOPFLI_LDFLAGS` to `make`.

## License

- Original pigz: zlib license (Mark Adler)
- pigz_cc: Same terms as Concurrent-C project
