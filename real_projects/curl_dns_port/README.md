# curl DNS port — brownfield interop

Drop Concurrent-C into stock libcurl by replacing the threaded async DNS
resolver (`lib/asyn-thrdd.c`) and linking that back into `libcurl.a` / the
`curl` CLI.

This is the DESIGN.md ladder rung after pigz and redis: compile one seam
and link it into someone else's library.

Pinned version: `CURL_VERSION` (currently **8.21.0**).

## What is installed

`out/prefix` is one curl. `make cc` and `make stock` flip it.

| Flavor | `libcurl.a` DNS object | Queue |
| --- | --- | --- |
| **stock** | upstream `asyn-thrdd.c` | `Curl_thrdq` |
| **cc** | `asyn_thrdd.c` lowered as a `.ccs` TU (same `Curl_async*` ABI) | `cc_resolv_q.ccs` (nursery / hybrid) |

`getaddrinfo` stays blocking. The CC win is nursery-owned join and
teardown, not “fibers make DNS free.”

`make cc` copies the overlay to `.ccs` and runs it through `ccc` so the
`#ifdef` tree is a clean copy for host-cc. The queue is a Concurrent-C TU.

Upstream under `curl_c/` is read-only. Edits live beside it.

## Quick start

```bash
./setup.sh          # → curl_c/ (gitignored)
make upstream       # stock curl → out/prefix
make smoke          # one HTTPS GET
make baseline       # DNS contract (CLI + libcurl multi harness)
make baseline-save  # → benchmarks/baseline_stock_YYYY_MM_DD.txt

make cc             # overlay + queue, relink curl
make smoke          # same smoke, now through CcResolvQ
make baseline-save  # → benchmarks/baseline_cc_YYYY_MM_DD.txt

make stock          # restore the snapshot
```

`make flavor` prints `stock` or `cc`. Smoke and baseline always test the
live prefix.

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
