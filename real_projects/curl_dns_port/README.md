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
