# redis_cc

This project mirrors the structure of `real_projects/pigz/`, but targets Redis.

The guiding rule is:

- tutorial = idiomatic = production

That means the smaller versions are not throwaway demos. They use the same
ownership and concurrency model as the production port, while intentionally
covering fewer features.

## Layout

- `setup.sh` fetches upstream Redis into `redis_c/`
- `redis_tutorial.ccs` is the smallest faithful version of the final model
- `redis_idiomatic.ccs` is the compact reference implementation
- `redis_cc/redis_cc.ccs` is the multi-file production port
- `bench_simple.sh` and `bench_redis.sh` will hold phased benchmark harnesses

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
make redis_tutorial redis_idiomatic redis_cc
```

For now, the `.ccs` files are scaffolds that pin down the architectural model
and build shape. The first real implementation step is `redis_tutorial.ccs`.
