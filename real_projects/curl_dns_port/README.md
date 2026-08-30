# curl DNS port — brownfield interop

Drop Concurrent-C into stock libcurl by replacing the thread queue behind
the threaded async DNS resolver (`Curl_thrdq_*` in `lib/thrdqueue.c`) and
linking that back into `libcurl.a` / the `curl` CLI.

Stock `lib/asyn-thrdd.c` stays stock. The CC file is the queue it already
talks to.

This is the DESIGN.md ladder rung after pigz and redis: compile one seam
and link it into someone else's library.

Pinned version: `CURL_VERSION` (currently **8.21.0**).

## What is installed

`out/prefix` is one curl. `make cc` and `make stock` flip it.

| Flavor | Resolver TU | Queue |
| --- | --- | --- |
| **stock** | upstream `asyn-thrdd.c` | upstream `thrdqueue.c` |
| **cc** | same stock object | `thrdqueue.ccs` (`Curl_thrdq_*`, nursery / hybrid) |

`getaddrinfo` stays blocking. The CC win is nursery-owned join and
teardown, not “fibers make DNS free.”

The queue owns a workers nursery, two retractable job bags, and `live`
(min always, max ever). It does not own curl’s `void *`: `send` carries
it; `recv` or `fn_free` returns it.

Each bag is a growable `Job[]` under one `CCExclusive` name (jobs `0`,
done `1`). Pop parks with `acquire_when` until an item exists or the bag
is closed. Idle workers wrap that wait in `cc_deadline_push` so
`idle_time_ms` is `CC_ERR_TIMEOUT`. Push signals; close broadcasts.
The empty worker wait is narrowly excluded from runtime deadlock detection:
future jobs arrive from libcurl's host event loop, outside the scheduler's
dependency graph.
`clear` retracts both bags under one sorted hold — not a channel bounce.

`destroy(join)` drains queued jobs (they do not run), closes the jobs
bag, and waits the nursery. Detach registers `on_last` and `abandon`s
the nursery; last-exit drains done and frees the queue.

`min_threads`, `max_threads`, and `idle_time_ms` are honored. `max_threads
== 0` or `min > max` is `CURLE_BAD_FUNCTION_ARGUMENT`.

Upstream under `curl_c/` is read-only. Edits live beside it.

## Quick start

```bash
./setup.sh          # → curl_c/ (gitignored)
make upstream       # stock curl → out/prefix
make smoke          # one HTTPS GET
make baseline       # DNS contract (CLI + libcurl multi harness)
make baseline-save  # → benchmarks/baseline_stock_YYYY_MM_DD.txt

make cc             # swap thrdqueue.o, relink curl
make smoke          # same smoke, now through the CC queue
make baseline-save  # → benchmarks/baseline_cc_YYYY_MM_DD.txt

make stock          # restore the snapshot
```

`make flavor` prints `stock` or `cc` (`cc_curl_thrdq` in `libcurl.a`).
Smoke and baseline always test the live prefix. `make queue-smoke` hits
the queue knobs and a blocked join/detach (no libcurl). `make queue-asan`
is the same smoke under AddressSanitizer. `make runtests-dns` runs the
curl tests that can hit this queue on a non-debug build (1515, 1516,
3301). 2103/2104 need `override-dns`; 1512 is DISABLED upstream.

Contract and numbers: [BASELINE.md](BASELINE.md).

## Current benchmark

Apple M5, Darwin arm64 25.5.0, curl 8.21.0, 2026-08-29. Each flavor ran
20 rounds of 64 concurrent transfers against the same fixed public-host list,
with per-handle DNS caching disabled. Both retained `AsynchDNS`, returned exit
6 for NXDOMAIN, completed every round without a failed lookup, and passed the
full harness.

| Metric | Stock libcurl | CC queue |
| --- | ---: | ---: |
| Concurrent wall median | 0.357 s | 0.259 s |
| Concurrent wall range | 0.257–1.296 s | 0.238–0.339 s |
| Mean DNS-time median | 0.0207 s | 0.0176 s |
| Abort median | 0.004 s | 0.002 s |
| Abort range | 0.003–0.006 s | 0.001–0.002 s |
| Failed concurrent / abort rounds | 0 / 0 | 0 / 0 |

The observed CC medians were 27% lower for concurrent wall time and 15% lower
for mean DNS time. The ranges overlap and DNS depends on the external network,
so this is a same-band regression check, not a speedup claim. Abort is the fast
teardown path, not proof of a blocked `getaddrinfo` join. The queue smoke
separately measured blocked join at 0.288 s and detach return at 0.000 s.

Receipts:
[stock](benchmarks/baseline_stock_2026_08_29.txt) ·
[CC](benchmarks/baseline_cc_2026_08_29.txt).

## Configure

Stock build uses:

- `--enable-threaded-resolver --disable-ares`
- `--disable-shared --enable-static`
- `--with-openssl=…` — Homebrew OpenSSL on macOS (`OPENSSL_PREFIX` override)
- `--disable-ldap --disable-ldaps`
- optional deps off (`libpsl`, brotli, nghttp2, …)

## Next

DoH, `--resolve` (bypasses the resolver), c-ares, and `override-dns`
builds (2103/2104). Full `runtests.pl` is not this port's gate.
