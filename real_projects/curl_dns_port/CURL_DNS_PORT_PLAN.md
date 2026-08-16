# Project: Porting Curl's Asynchronous DNS Resolver

Brownfield interop test: replace curl's threaded DNS resolver with a
Concurrent-C translation unit and link it into a stock libcurl build.

Working tree layout and phase status: [README.md](README.md).

## Target: `lib/asyn-thrdd.c`

`asyn-thrdd.c` implements asynchronous DNS via OS threads so `getaddrinfo`
does not block curl's event loop. It is one TU with a fixed API surface
toward the rest of libcurl (start, await, shutdown, wakeup), which makes
a surgical swap feasible.

### Why this target

1. **Surgical interop** — replace one file; leave the rest of curl as C.
2. **Build-system proof** — Autotools/CMake must accept a `ccc --compile`
   object in place of the stock `.o`.
3. **Structured teardown** — DNS cancel/join is a known UAF and leak site.
   Curl dropped `pthread_cancel` here because libc DNS is not cancel-safe.
   Nursery ownership and arena-scoped results give deterministic cleanup
   without force-killing the lookup.
4. **Honest concurrency** — `getaddrinfo` remains blocking. The win is
   ownership and join, not "fibers make DNS free." Hybrid/blocking offload
   is the right spawn path for the lookup itself.

## Implementation strategy

1. **Pin + stock baseline** — fetch a release tarball; build with
   `--enable-threaded-resolver --disable-ares`; smoke HTTPS.
2. **Build integration** — compile the CC resolver with `ccc --compile`
   and substitute that object into libcurl's archive / final link.
3. **Fiber / hybrid replacement** — map `Curl_thread_create` / join onto
   nursery spawn + wait; keep the existing socketpair/pipe wakeup so the
   multi interface still learns completion.
4. **Arena lifetime** — hold `addrinfo` results and per-request metadata
   in a request-scoped arena freed when the resolver task completes or
   the owning nursery is cancelled (after join).

## Success metrics

Defined operationally in [BASELINE.md](BASELINE.md):

- Stock identity: `AsynchDNS` / POSIX threaded resolver.
- Happy path + NXDOMAIN via CLI and libcurl.
- Concurrent multi lookups (N=32 default) all DNS-ok.
- Abort/teardown mid-resolve returns within the soft budget (no hang).
- Dated stock numbers under `benchmarks/baseline_stock_*.txt` for
  order-of-magnitude comparison after the CC object swap.

Later: curl `runtests.pl` DNS-related cases once the swap links cleanly.
