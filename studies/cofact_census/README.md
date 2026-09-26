# Co-fact census: real_projects (seed 0.4.0-419)

Four readers read every source file in `real_projects/` (redis, staticd,
curl_dns_port, pigz, levenshtein, raytracer, parallel_storm,
random_access) and the std server library (`cc/include/ccc/std/server*`),
looking for facts stored in more than one place and kept in sync by
hand. Counts are per reader and approximate; the server library was read
twice, so a few items are counted in two rows.

| Kind | redis | pigz, lev, rt | storm, ra, server | staticd, curl | Total |
|------|------:|--------------:|------------------:|--------------:|------:|
| tag beside payload | 3 | 8 | 7 | 8 | ~26 |
| derivation with a source | 4 | 8 | 6 | 6 | ~24 |
| count shadowing a set | 2 | 5 | 6 | 7 | ~20 |
| validation at a seam | 6 | 4 | 5 | 5 | ~20 |
| scalar published to another fiber | 3 | 4 | 5 | 6 | ~18 |
| record fields that move together | 4 | 4 | 3 | 4 | ~15 |
| promise and fulfillment | 4 | 2 | 1 | 1 | 8 |
| push invalidation | 1 | 2 | 1 | 3 | 7 |
| loop-carried chain | 0 | 6 | 0 | 0 | 6 |
| schedule equivalence | 0 | 4 | 2 | 0 | 6 |
| external source (kernel, filesystem, clock) | 0 | 0 | 1 | 4 | 5 |
| slot generation | 0 | 0 | 2 | 1 | 3 |
| delta or journal | 1 weak | 0 | 1 weak | 0 | ~2 |
| deep graph | 0 | 0 | 0 | 0 | 0 |

## Defects, with how each was checked

| Defect | Where | Checked |
|--------|-------|---------|
| `--rsyncable` drops input and exits 0: the rolling hash can hit every 5 bytes, the segment array holds `block/5`, a full array leaves no slot for the tail, and `compress_block` never checks `total_in == data.len` | `pigz/pigz_cc/pigz_cc.ccs:174,231-245,440-465` | **reproduced**: `pigz_rsync_repro.sh`, 393,215 bytes in, CRC and length errors, 393,204 back |
| Empty stdin writes an invalid gzip | `pigz_cc.ccs:619,694-716` | **refuted**: 20 bytes, `gzip -t` passes |
| Dump prints the key arena root as 1024 B; the code uses `REDIS_DB_KEY_ARENA_ROOT` = 1 MiB, under a "must match db_init()" comment | `redis/redis_mem.cch:316-324`, `redis_db.cch:60,178` | read |
| The client resets `conn->reply_arena` with `@defer` on every exit of the drain loop, including early exits with replies pending, while the owner fiber builds replies into it through `try_send_into` | `redis/redis_owner.ccs:922-937`, owner loop | read (race, not run) |
| `db_insert_new_entry` reuses a key word that `db_insert_value` frees on its replace path; safe only because callers arrive after a lookup miss | `redis/redis_db.cch:511-527` | read (latent) |
| A worker exits on a non-timeout `take_job` error without decrementing `live` | `curl_dns_port/thrdqueue.ccs:426-427` | read |
| `want_out` stores `act == 1 \|\| pend`, folding the engine's pending bit into the page's, against the header's "two independent facts, do not collapse them" | `std/server_serve.ccs:689,822`, `server.cch:42-46` | read |
| A thief increments `inflight` after popping and unlocking, so the owner's exit can see zero and free the row it is about to step | `std/server_serve.ccs:588-592,622,1314-1322` | reader-traced, not verified here |
| `tape_slots` is never decremented; tapes after the 64th get slot -1 | `std/server_poll.ccs:365` | reader-traced |
| Grid clamp to 1..16 makes the march clamp 8..512 unreachable above 16 | `parallel_storm/storm.ccs:174-177`, `storm_world.cch:434-438` | reader-traced |
| An N:1 channel is drained from every generator | `random_access/ra_dist.ccs:121-129` | reader-traced |
| A plain `int failed` written by many tasks | `pigz/pigz_parallel.ccs:138-173` | reader-traced |

## What the census says about the model

- **The zero-cost tools cover the bulk.** Variants for a tag beside a
  payload, a conditional verb for a count beside a set, a read hook for a
  stored derivation, and one check at the seam account for about 90 of
  roughly 170 findings, with no run-time cost and often less.
- **Deltas are rare.** Outside cctext's history, no project keeps an edit
  journal; two weak cursor cases exist. `@patch` and the log ring stay a
  cctext-shaped idea until a second specimen needs them.
- **`seq (cond)` exists and is never compared.** pigz has the switch and
  no test compares the two schedules; storm keeps four hand copies of
  one recursion; the raytracer duplicates its loop by hand and is the only
  project whose smoke test compares sequential and parallel output.
- **The counterexamples cluster in five places**: scalars published to
  other threads that are meant to be stale (`qlen`, `tape_conns`,
  `live`); external sources with no version to bump (the kernel's
  interest set, the filesystem, the clock); hot loops where any added
  store shows (levenshtein's bit kernels, random_access's table); pinned
  snapshots that must not resync (staticd's `FileHold`, redis's MGET
  clone); and deliberate second storage (redis's sparse expires table).
- **Concepts the model lacks, found by the census**: frozen after
  construction, which would also let a compiler hoist through aliasing
  (`ra_dist.ccs:64`); a pinned version, a borrow of version v; time as a
  source; code duplicated across variants as a co-fact (seven copies of
  `compress_block`); and drift in comments, constants, and write-only
  fields, which a lint catches more cheaply than any construct.
- **The most dangerous facts are not stored values.** Who guards a shard,
  who owns a reply arena while replies are pending, who holds a row a
  thief is stepping: these are section 2 and 3 facts, and the census
  keeps them out of section 4.
