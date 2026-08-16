# curl DNS port — brownfield interop

Drop Concurrent-C into stock libcurl by replacing the threaded async DNS
resolver (`lib/asyn-thrdd.c`) with a CC translation unit, then linking that
object back into `libcurl.a` / the `curl` CLI.

This is the DESIGN.md ladder rung after pigz and redis: not a from-scratch
rewrite, but compile-one-file-and-link-into-someone-else's-library.

## Status

**Phase 1 (this tree):** CC resolve queue + object swap; baseline green.

| Piece | State |
| --- | --- |
| Pin + `setup.sh` | done |
| Stock threaded-resolver build | `make upstream` |
| Baseline contract + harness | `BASELINE.md`, `make baseline` / `baseline-save` |
| `cc_resolv_q.ccs` (nursery/hybrid queue) | done |
| `asyn_thrdd.c` overlay → swap into `libcurl.a` | `make cc-swap` |
| CC baseline | `make baseline-cc` / `baseline-cc-save` |
| curl `runtests.pl` DNS series | not started |

Pinned version: see `CURL_VERSION` (currently **8.21.0**).

What “good” means after a CC swap is spelled out in [BASELINE.md](BASELINE.md).
Stock vs CC medians land in `benchmarks/baseline_stock_*.txt` and
`benchmarks/baseline_cc_*.txt`.

## Why this file

`asyn-thrdd.c` runs `getaddrinfo` off curl's event loop on OS threads. Curl
dropped `pthread_cancel` there because cancelling inside libc DNS leaks and
hangs. A CC port does **not** make `getaddrinfo` free on a fiber — it still
needs a hybrid / blocking offload — but it *does* give nursery-owned join
and arena-scoped result lifetime for the teardown path curl can no longer
force-kill.

## Quick start

```bash
./setup.sh          # → curl_c/ (gitignored)
make upstream       # → out/prefix/bin/curl (static, threaded resolver)
make smoke          # one HTTPS GET
make baseline       # full DNS contract (CLI + libcurl multi harness)
make baseline-save  # same + write benchmarks/baseline_stock_YYYY_MM_DD.txt
make cc-swap        # CcResolvQ + asyn_thrdd.o into libcurl.a
make baseline-cc-save
```

## Concurrent-C swap (phase 1)

- `cc_resolv_q.ccs` — nursery-owned hybrid workers replacing `Curl_thrdq` for DNS
- `asyn_thrdd.c` — stock `asyn-thrdd.c` overlay calling `CcResolvQ_*`
- `make cc-swap` removes `libcurl_la-asyn-thrdd.o` from `libcurl.a` and inserts
  our overlay; `baseline_dns_cc` also links `cc_resolv_q.o` + the CC runtime

`getaddrinfo` remains blocking (honest). The win is structured join/teardown
via the nursery, not “fibers make DNS free.”

Upstream under `curl_c/` is read-only reference. Edits for the port live
beside it (CC sources, Makefile swap rules), never as permanent patches
inside the tarball tree.

## Configure choices

Stock build uses:

- `--enable-threaded-resolver --disable-ares` — the target backend
- `--disable-shared --enable-static` — one binary, easy object swap later
- `--with-openssl=…` — Homebrew OpenSSL on macOS (`OPENSSL_PREFIX` override)
- `--disable-ldap --disable-ldaps` — avoid link failures without OpenLDAP
- optional deps off (`libpsl`, brotli, nghttp2, …) — fewer host packages

OpenSSL comes from Homebrew (`openssl@3`) when present; override with
`make OPENSSL_PREFIX=/path upstream` if needed.

## Next (phase 1)

1. Read `curl_c/lib/asyn-thrdd.c` + thread/wakeup helpers; list the
   symbols the rest of libcurl expects from that TU.
2. Author `asyn_thrdd.ccs` that exports the same C API, using
   `n->spawnhybrid` (or equivalent blocking offload) for `getaddrinfo`
   and a nursery for ownership.
3. `ccc --compile -o out/asyn-thrdd.o …` and replace the stock object in
   the libcurl archive / relink `curl`.
4. Re-run `make smoke`, then curl's DNS-related tests.

See `CURL_DNS_PORT_PLAN.md` for the original proposal and success metrics.
