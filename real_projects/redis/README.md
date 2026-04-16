# redis_cc

This project mirrors the structure of `real_projects/pigz/`, but targets Redis.

The supported Concurrent-C Redis variants are:

- `redis_idiomatic.ccs` as the compact reference implementation
- `redis_hybrid.ccs` as the active hybrid/runtime integration target
- `redis_cc/redis_cc.ccs` as the future modular production port

## Layout

- `setup.sh` fetches upstream Redis into `redis_c/`
- `redis_idiomatic.ccs` is the compact reference implementation
- `redis_hybrid.ccs` is the active hybrid implementation and default benchmark target
- `redis_cc/redis_cc.ccs` is the multi-file production port
- `reply_path_bench.ccs` and `reply_path_threaded_bench.ccs` are explicit reply-path microbench experiments, not server variants
- `bench_simple.sh` compares **upstream `redis-server`** vs **`out/redis_hybrid`** by default (set `BENCH_IDIOMATIC=1` to also run `redis_idiomatic` on port 6393). Set `HYBRID_CC_V2_THREADS=<n>` to pin the hybrid benchmark to a specific V2 worker count without hard-coding that policy into the script.
- `bench_redis.sh` is reserved for a broader phased suite

## Upstream Redis Policy

Upstream Redis is treated as read-only reference material:

- fetch it locally
- build it locally
- benchmark against it locally
- do not patch it
- do not commit it to git

The intended git boundary is:

- commit: Concurrent-C sources, scripts, docs, and reproducibility metadata
- ignore: `redis_c/`, tarballs, binaries, local benchmark artifacts, `.rdb`, `.aof`

## Shared Architecture

All versions should preserve the same shape:

1. accept loop
2. one fiber per client connection
3. RESP decode on the connection side
4. command messages routed over channels to shard-owner fibers
5. replies routed back to the connection fiber
6. RESP encode on the way out

The main scaling knob is shard count, not a different programming model.

## Bootstrap

```bash
./setup.sh
make upstream
make redis_idiomatic redis_hybrid redis_cc
```

Quick comparison runs:

```bash
cd real_projects/redis
./bench_simple.sh
PIPELINE=16 ./bench_simple.sh
CLIENTS=1 PIPELINE=1 ./bench_simple.sh
REPEATS=5 PIPELINE=16 ./bench_simple.sh
BENCH_IDIOMATIC=1 ./bench_simple.sh   # upstream + hybrid + idiomatic
HYBRID_CC_V2_THREADS=8 ./bench_simple.sh
```

`CLIENTS` controls connection concurrency (`redis-benchmark -c`).
`PIPELINE` controls pipeline depth (`redis-benchmark -P`).
`REPEATS` runs the suite multiple times and reports median/range summaries.
`HYBRID_CC_V2_THREADS` optionally exports `CC_V2_THREADS` only for `redis_hybrid`, which is useful when you want reproducible apples-to-apples comparisons while the runtime default is still changing.

`redis_cc` remains a scaffold for the eventual modular port. `redis_idiomatic`
and `redis_hybrid` are the current runnable implementations.
