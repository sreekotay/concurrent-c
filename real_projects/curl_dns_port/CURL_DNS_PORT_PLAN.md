# Project: Porting Curl's Asynchronous DNS Resolver

Brownfield interop: replace curl's `Curl_thrdq` (the queue `asyn-thrdd.c`
already calls) with a Concurrent-C implementation, and link that into a
stock libcurl build. Stock `lib/asyn-thrdd.c` is not forked.

Working tree and commands: [README.md](README.md).

## Target: `lib/thrdqueue.c`

`asyn-thrdd.c` implements asynchronous DNS via OS threads so `getaddrinfo`
does not block curl's event loop. It talks to a small queue ABI:
`Curl_thrdq_create` / `send` / `recv` / `clear` / `set_props` / `destroy`.

### Why this target

1. **Surgical interop** — replace the queue object; leave the resolver TU
   and the rest of curl as C.
2. **Build-system proof** — the installed `libcurl.a` / `curl` CLI accept
   a swapped `thrdqueue.o` plus the CC runtime.
3. **Structured teardown** — DNS cancel/join is a known UAF and leak site.
   Curl dropped `pthread_cancel` here because libc DNS is not cancel-safe.
   Nursery ownership gives deterministic cleanup without force-killing the
   lookup.
4. **Honest concurrency** — `getaddrinfo` remains blocking. The win is
   ownership and join, not "fibers make DNS free." Hybrid/blocking offload
   is the spawn path for the lookup itself.
5. **Honest knobs** — `min_threads` / `max_threads` / `idle_time_ms` are
   the pool curl thinks it configured. Reject `max_threads == 0` or
   `min > max`; do not ignore them.

## Layout

- `thrdqueue.ccs` — nursery-owned hybrid pool implementing `Curl_thrdq_*`.
- `cc_thrdqueue.h` — C ABI matching `lib/thrdqueue.h` (CURLcode as int).
- `thrdqueue_smoke.c` — knob/ABI check without libcurl.
- `make cc` / `make stock` — flip `out/prefix` between flavors.

## Success metrics

Defined in [BASELINE.md](BASELINE.md): identity, happy/fail CLI paths,
concurrent multi lookups, fast-path abort, blocked join/detach on the
queue itself (`make queue-smoke`), dated medians under `benchmarks/`.
