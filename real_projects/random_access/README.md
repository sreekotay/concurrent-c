# RandomAccess (single-locale Chapel / HPCC GUPS)

HPC Challenge RandomAccess: a table of `2^n` 64-bit words, `T[i] = i`,
then `N_U = 4 · 2^n` updates `T[ran & mask] ^= ran` from the HPCC LFSR
(`POLY 0x7`). GUPS is `N_U / seconds / 1e9`. Verify replays the stream
and counts `T[i] != i`. Unlocked writers may lose a XOR (pass if that
is ≤1% of `N_U`); atomic writers must land at 0.

This folder is the **one-locale** race against Chapel's release RA
(`-nl 1`). It is not MPI and not multi-locale Chapel.

| Chapel | Concurrent-C | work |
|---|---|---|
| `ra.chpl` `-nl 1` | `smp` | one table, many writers, unlocked `T[i] ^= ran` |
| `ra-atomics.chpl` `-nl 1` | `smp-atomic` | same, atomic xor |

Same stream (`-suseLCG=false` / [`ra.h`](ra.h) LFSR), same `n`, same
`N_U`, same 4 tasks. Clock is the update loop only. On one locale
Chapel's `on T[…]` does nothing; both sides partition the stream and
race XORs into one shared table. CC verify inverses sequentially
(same question, not on the clock).

The table is a `Vec::[uint64_t]` on a named arena. A slice is a view;
the vec is the owner.

## Run

Needs `ccc` (this repo's `./cc/bin/ccc`) and, for the Chapel rows,
`chpl` (`brew install chapel`). Compare always runs Chapel with `-nl 1`.

```bash
cd real_projects/random_access
./setup.sh                 # once: fetch Chapel .chpl (gitignored)
./compare.sh               # default: n=26, 512 MiB table, 256M updates
./compare.sh --smoke       # n=16; Chapel optional (identity only)
```

From the repo root:

```bash
./real_projects/random_access/compare.sh
./real_projects/random_access/compare.sh --smoke
```

Override size or width with env (still one locale):

```bash
TABLE_BITS=20 WORKERS=4 ./real_projects/random_access/compare.sh   # 8 MiB, L3
TABLE_BITS=24 ./real_projects/random_access/compare.sh
```

`n=26` is ~0.5 GB plus Chapel's copy. `--smoke` is the cheap check:
unlocked errors ≤1% of `N_U`, atomic errors 0. Do not read GUPS from
smoke.

CC binary alone (after `compare.sh` has built `out/ra_cc`):

```bash
MODE=smp ./out/ra_cc              # same default n=26
RA_SMOKE=1 MODE=smp-atomic ./out/ra_cc
```

## Results (this machine)

Default `compare.sh` is `TABLE_BITS=26`, 64M words (512 MiB), 256M
updates, 4 workers, Chapel `-nl 1`. The table misses cache.

| row | GUPS | errors | vs Chapel |
|---|---|---|---|
| chapel ra | 0.385 | 355 | — |
| cc smp | 0.423 | 638 | **1.10×** |
| chapel ra-atomics | 0.360 | 0 | — |
| cc smp-atomic | 0.397 | 0 | **1.10×** |

Unlocked and atomic collapse to ~0.4 GUPS on both sides (DRAM latency
eats the RMW tax). CC is slightly ahead. Unlocked error rates stay
tiny (well under 1%); atomic is identity.

`TABLE_BITS=20` (8 MiB) is L3-resident — a correctness race, not HPCC's
working set. Last `n=20` on this machine: chapel ra 2.49 GUPS vs cc smp
1.97 (0.79×); chapel ra-atomics 0.330 vs cc smp-atomic 0.278 (0.84×).

## Multi-locale

[`ra_dist.ccs`](ra_dist.ccs) is the other specimen: two placement
facts, not a distributed array. `update_owner(u)` generates
`stream[u]`; `table_owner(i)` owns `T[i]`. They are not the same.
A remote update is an N:1 send of `r`; the owner applies. That is
Chapel's `on T[r & mask]`.

| Chapel | Concurrent-C | work |
|---|---|---|
| `ra.chpl` `-nl N` | `dist` | hop, then unlocked XOR |
| `ra-atomics.chpl` `-nl N` | `dist-atomic` | hop, then fetch_xor |

CC hops on in-process channels. Chapel hops on `CHPL_COMM`. Same
sentence; not the same wire. Homebrew Chapel is usually
`CHPL_COMM=none`, so `-nl N` is not a locale grid — `compare_dist.sh`
skips those rows and still runs CC.

```bash
./compare_dist.sh --smoke          # n=16, LOCALES=2, identity
TABLE_BITS=20 LOCALES=2 WORKERS=1 ./compare_dist.sh
MODE=dist LOCALES=2 ./out/ra_cc_dist
```

Default dist size is `n=20` (channel hops are the clock, not DRAM).
`LOCALES` must be a power of 2. `WORKERS` is tasks per locale
(Chapel `--dataParTasksPerLocale`). `WORKERS=1` is one writer per
shard — unlocked is identity too. `WORKERS>=2` races.

This machine, `n=20`, `LOCALES=2`, `WORKERS=1`, Chapel skipped
(`CHPL_COMM=none`):

| row | GUPS | errors |
|---|---|---|
| cc dist | 0.048 | 0 |
| cc dist-atomic | 0.047 | 0 |

~0.05 GUPS vs ~2 GUPS on the one-locale `n=20` smp row. The hop is
the clock. That is the point of the specimen, not a GUPS race against
`-nl 1`.

We looked at [Chapel_QGN](https://github.com/sdbachman/Chapel_QGN)
(Grooms' N-layer QG, Bachman port) and left it. Production domains
are not `dmapped`; the FFT is serial FFTW through C pointers. A
science port, not a locale specimen.

## Layout

| file | role |
|---|---|
| `ra_idiomatic.ccs` | CC `smp` / `smp-atomic` (`-nl 1`) |
| `ra_dist.ccs` | CC `dist` / `dist-atomic` (`-nl N`) |
| `ra.h` | shared LFSR (`step` / `starts` / index) |
| `ra.c` | sequential C kernel (not a race row) |
| `compare.sh` | Chapel `-nl 1` vs CC (default `n=26`) |
| `compare_dist.sh` | Chapel `-nl N` vs CC dist (default `n=20`) |
| `chapel/` | fetched Chapel sources (`VENDOR.txt` is committed) |
