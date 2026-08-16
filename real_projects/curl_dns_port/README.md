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
is the same smoke under AddressSanitizer.

Contract and numbers: [BASELINE.md](BASELINE.md).

## Configure

Stock build uses:

- `--enable-threaded-resolver --disable-ares`
- `--disable-shared --enable-static`
- `--with-openssl=…` — Homebrew OpenSSL on macOS (`OPENSSL_PREFIX` override)
- `--disable-ldap --disable-ldaps`
- optional deps off (`libpsl`, brotli, nghttp2, …)

## Next

curl `runtests.pl` DNS series.
