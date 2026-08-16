# What we are testing

This port swaps curl's `Curl_thrdq` for a Concurrent-C queue. Stock
`asyn-thrdd.c` stays. The baseline is the same contract on whichever
flavor is installed in `out/prefix`.

Binary under test: `out/prefix/bin/curl` and `out/prefix/lib/libcurl.a`
(pin in `CURL_VERSION`). Resolver must be **POSIX threaded** / `AsynchDNS`
(not c-ares). `make flavor` prints `stock` or `cc`.

## Axes

| Axis | What it proves | Gate |
| --- | --- | --- |
| **Identity** | Build really uses threaded async DNS | `Features: … AsynchDNS …`; configure `resolver: POSIX threaded` |
| **Happy path** | Resolve + TLS GET works | HTTP 200 to a known host; `time_namelookup` reported |
| **Fail path** | NXDOMAIN / bad name surfaces cleanly | `CURLE_COULDNT_RESOLVE_HOST` (exit 6 on CLI) |
| **Concurrent** | Queue handles many in-flight lookups | N easy handles × R rounds; all DNS-ok; report per-round + **median** wall / dns_mean |
| **Abort / teardown** | Remove or destroy mid-resolve without hang/crash | Same N × R; each teardown &lt; 5s; median wall recorded. This is the fast path (lookups usually not yet in `getaddrinfo`). |
| **Blocked join** | Join waits an in-flight `process` and frees queued work. Detach `abandon`s the nursery (returns without joining); last-exit frees. No UAF. | `make queue-smoke` (`join_blocked` / `detach_blocked`); `make queue-asan` |
| **curl tests** | Stock harness agrees on the tests this build can run | `make runtests-dns` — 1515, 1516, 3301 (thrdqueue unit). 2103/2104 skip without `override-dns`; 1512 is DISABLED upstream. |
| **Perf snapshot** | Comparable wall times later | Dated `benchmarks/baseline_<flavor>_*.txt` (medians, not a single shot) |

Not in scope: DoH, `--resolve` (bypasses the resolver), c-ares, a
debug/`override-dns` curl. `CURLVERBOSE` thread-queue traces still do
not call `curl_trc`.

## How to run

```bash
make upstream          # if needed
make baseline          # N=32, 10 rounds (override: BASELINE_N=64 BASELINE_ROUNDS=20)
make baseline-save     # → benchmarks/baseline_stock_YYYY_MM_DD.txt

make cc
make baseline-save     # → benchmarks/baseline_cc_YYYY_MM_DD.txt
```

Harness sources:

- `baseline.sh` — CLI identity + happy/fail + invokes the C driver
- `baseline_dns.c` — libcurl multi concurrent / abort / sequential
- `thrdqueue_smoke.c` — knobs plus blocked join/detach (`make queue-smoke` / `queue-asan`)

Flavor is read from `libcurl.a` (`cc_curl_thrdq` → `cc`).

## Metrics recorded

From the CLI smoke:

- `http_code`, `time_namelookup`, `time_connect`, `time_total`

From `baseline_dns` (defaults: **N=32**, **R=10** rounds):

- `sequential`: one short serial pass (correctness, not the perf number)
- `concurrent`: R batches of N transfers; per-round ok/fail/wall/dns_mean;
  **`concurrent_summary`** with `wall_median`, min/max, `dns_mean_median`
- `abort`: R teardown-without-await rounds; **`abort_summary`** medians;
  any round &gt; 5s fails. Does not prove a blocked `getaddrinfo` join.

Transfers use `CURLOPT_CONNECT_ONLY` so HTTP/TLS quirks do not fail the
DNS gate. A transfer counts as DNS-ok unless the result is
`CURLE_COULDNT_RESOLVE_HOST` / `CURLE_COULDNT_RESOLVE_PROXY` (post-DNS
connect errors are noted but not failures).

Hostnames are a fixed public list (see `baseline_dns.c`) so runs are
comparable. DNS cache is disabled on each easy handle
(`CURLOPT_DNS_CACHE_TIMEOUT=0`) so concurrent work actually hits the
resolver queue. Compare flavors by **medians**, not a single noisy shot.

## Pass / fail after `make cc`

1. Same identity (`AsynchDNS`).
2. Happy + fail paths unchanged.
3. Concurrent: all N succeed (or same failure mode as stock on that network).
4. Abort: returns, no crash, no multi-second hang.
5. Perf: compare **`wall_median` / `dns_mean_median`** to the dated stock
   file on the same machine (DNS noise remains; look for regressions in
   abort median and crash/hang first).
