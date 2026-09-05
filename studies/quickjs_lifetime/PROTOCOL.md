# Protocol — checkpoints, oracle, receipts

Language-neutral. Both Track A drivers emit the same line format.

## Checkpoint lines (stdout)

One JSON object per line, prefix `CHK ` so scripts can filter:

```text
CHK {"epoch":0,"phase":"after_retain","claims":100,"weaks":100,"regs":0,"rss":12345678,"ok":1}
```

| Field | Meaning |
|-------|---------|
| `epoch` | Generation index (0-based) |
| `phase` | `after_create` / `after_retain` / `after_drop_roots` / `after_gc` / `after_use` / `after_release_half` / `after_gc2` / `after_weak_check` / `after_callback` / `after_teardown` |
| `claims` | Outstanding host claims |
| `weaks` | Outstanding weak handles (alive or dead — count of Weak objects) |
| `regs` | Outstanding callback registrations |
| `rss` | Process RSS bytes from `stress_mem_sample` (0 if unavailable) |
| `ok` | `1` if oracle checks for this phase passed, else `0` |
| `detail` | Optional string on failure |

## Oracle (pass/fail)

At every explicit checkpoint:

1. A live claim must keep its JS value usable (callable / readable).
2. Releasing one of several claims on the same value must not destroy access via remaining claims.
3. After the final host claim is released and JS roots dropped, weak upgrade must report expiry (once GC has run / after pressure).
4. Weak handles must not themselves keep values alive across that final release.
5. Callbacks must never execute through an expired registration.
6. Realm teardown must leave no host claim capable of reaching the dead realm (`STALE_REALM` or equivalent).
7. No use-after-free, double-free, or silently retained claim (ASan / exit code).

Drivers exit non-zero on any oracle failure.

## Perf lines (stdout)

Prefix `PERF `:

```text
PERF {"op":"retain","n":10000,"ns_total":123456789,"ns_per_op":12345}
```

Ops: `retain`, `release`, `borrow`, `weak_create`, `weak_upgrade`, `register`, `invoke`, `unregister`, `workload_wall`.

## Receipts

`scripts/run_track_a.sh` writes under `track_a/results/`:

| File | Content |
|------|---------|
| `YYYYMMDD_cc.txt` | Summary: version, pin, oracle PASS/FAIL, key PERF lines |
| `YYYYMMDD_rust.txt` | Same for Rust |
| `YYYYMMDD_compare.txt` | Oracle stream match + wall-time note |
| `*.jsonl` / `*.csv` | Optional raw streams (gitignored) |

Small `.txt` summaries are commit-friendly.
