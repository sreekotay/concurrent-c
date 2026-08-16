# Project: Porting Curl's Asynchronous DNS Resolver

Brownfield interop: replace curl's threaded DNS resolver with a
Concurrent-C queue behind a host-C overlay, and link that into a stock
libcurl build.

Working tree and commands: [README.md](README.md).

## Target: `lib/asyn-thrdd.c`

`asyn-thrdd.c` implements asynchronous DNS via OS threads so `getaddrinfo`
does not block curl's event loop. It is one TU with a fixed API surface
toward the rest of libcurl (start, await, shutdown, wakeup).

### Why this target

1. **Surgical interop** — replace one file; leave the rest of curl as C.
2. **Build-system proof** — the installed `libcurl.a` / `curl` CLI accept
   a swapped object plus a `ccc --compile` queue.
3. **Structured teardown** — DNS cancel/join is a known UAF and leak site.
   Curl dropped `pthread_cancel` here because libc DNS is not cancel-safe.
   Nursery ownership gives deterministic cleanup without force-killing the
   lookup.
4. **Honest concurrency** — `getaddrinfo` remains blocking. The win is
   ownership and join, not "fibers make DNS free." Hybrid/blocking offload
   is the spawn path for the lookup itself.

## Layout

- `asyn_thrdd.c` — overlay source; `make cc` lowers it as a `.ccs` TU
  (same `Curl_async*` exports; calls `CcResolvQ_*`).
- `cc_resolv_q.ccs` — nursery-owned hybrid workers replacing `Curl_thrdq`
  on the DNS path.
- `make cc` / `make stock` — flip `out/prefix` between flavors.

## Success metrics

Defined in [BASELINE.md](BASELINE.md): identity, happy/fail CLI paths,
concurrent multi lookups, abort/teardown budget, dated medians under
`benchmarks/`.
