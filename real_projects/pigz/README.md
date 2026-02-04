# pigz_cc: Parallel Gzip in Concurrent-C

This project provides two implementations of parallel gzip compression:

1. **Original pigz** - Mark Adler's pthread-based implementation
2. **pigz_cc** - Native Concurrent-C rewrite using structured concurrency

## Quick Start

```bash
# 1. Download original pigz source
chmod +x setup.sh
./setup.sh

# 2. Build both versions
make pigz      # Original (requires zlib, pthreads)
make pigz_cc   # CC version (requires CC compiler + zlib)

# 3. Run benchmark
./benchmark.sh 200 8 3   # <size_mb> <workers> <runs>
```

## Benchmark Data (auto-downloaded, not checked in)

`benchmark.sh` uses a **real multi-file corpus** (Silesia corpus) and generates a single input file by concatenating corpus files until it reaches the requested size.

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

Uses channels and nested nurseries for clean structured concurrency:

```c
@nursery {
    spawn(writer);               // Collects results
    
    @nursery closing(results_tx) {
        for (N workers)
            spawn(compressor);   // Compress blocks
        
        @nursery closing(blocks_tx) {
            spawn(reader);       // Produce blocks
        }
    }
}
```

## CC Patterns Demonstrated

### 1. Channel Pipeline
Blocks flow through typed channels - no shared mutable state:
```c
Block[~4 >] blocks_tx;    // Send handle
Block[~4 <] blocks_rx;    // Receive handle
CCChan* ch = channel_pair(&blocks_tx, &blocks_rx);
```

### 2. Nested Nurseries with `closing()`
Auto-close channels when producers finish:
```c
@nursery closing(results_tx) {
    // Workers here...
    @nursery closing(blocks_tx) {
        // Reader here - when done, blocks_tx closes
    }
    // Workers drain blocks_rx, then exit
}
// Writer drains results_rx, then exits
```

### 3. Parallel Workers via `spawn()`
No thread management - structured lifetime:
```c
for (int w = 0; w < num_workers; w++) {
    spawn(() => [level] {
        Block blk;
        while (chan_recv(blocks_rx, &blk) == 0) {
            Result res = compress_block(&blk, level);
            chan_send(results_tx, res);
        }
    });
}
```

## Files

| File | Description |
|------|-------------|
| `setup.sh` | Downloads original pigz source |
| `Makefile` | Builds both versions |
| `benchmark.sh` | Main benchmark (auto-downloads corpus + prints summary table) |
| `pigz_cc.ccs` | Legacy prototype (not built by the Makefile) |
| `pigz_cc/pigz_cc.ccs` | **CC implementation used by the Makefile** |
| `pigz.c` | Original (downloaded) |
| `yarn.c/h` | Original thread layer |
| `try.c/h` | Original error handling |

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
- CC compiler (`ccc`) for pigz_cc

### macOS
```bash
brew install zlib
make
```

### Linux
```bash
sudo apt-get install zlib1g-dev
make
```

## License

- Original pigz: zlib license (Mark Adler)
- pigz_cc: Same terms as Concurrent-C project
