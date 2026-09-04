# Tickets, the sequential loop, and what stays a nursery

Record of the explore: reduce user concepts so each tool has one job, and list
what still belongs on a nursery or channel. The product may adopt the drawing
or back away; the gaps stay either way.

## One job each

| Tool | Job |
|---|---|
| **Join** — `@parallel { … }` | Named siblings. First arm may be the kick (caller). Dest = do not wait here. `@serial` is how an arm is written. |
| **Range** — `@parallel for` / `wait` + `@stage` | Cut and name tiles. Bare `for`: disjoint slots. `wait` + `@stage`: the names serialize at that gate. `n` is tiles, not results. |
| **Stream** | Consumer never had `n`. Produce until the source dies; consume until `close`. On-page: two `@parallel spawn` arms and `tx.close()` in produce (`recipe_parallel_stream.ccs`). Ordered FIFO: `send_task`. |
| **Bag** — nursery | Set is not on the page: late `spawn`, host `send` / retract / `AGAIN`, never-deny fiber. Dest now has the same `leave` / EMPTY pair (`h.close(tx)`, `h.leave()`). The bag remains late admit / host ABI / never-deny. |

Dest, pause, cancel, turnstile cap are knobs, not extra tools.

There is no “ordered parallel.” Sequential ticket names (`i` in `0..n`) plus a
stage *are* publish order. `recipe_ordered_parallel` is a stream (`send_task` +
FIFO recv). Order on that file lives on the channel end.

## The drawing

One sequential program, N deep, stalled only at named lines.

```ccs
@parallel wait (t) for (i in 0..n) {
    @stage (t, 0, i) { /* admit / hop — optional */ }
    work();            /* overlap; cap is enter */
    @stage (t, k, i) { /* N:1 publish — optional */ }
} !>.wait()!>;
```

- **N:1 is the write gate.** Only one name sits in that stage, in `i` order.
  Pigz emit, find append, redis owner execute. Not a second fiber.
- **Ordered read is orthogonal.** Dict hop / `accept` in name order is another
  gate, or none.
- **A conn just loops.** Ticket = accept (or not even that). `handle` is a C
  `while (recv)`. Keep-alive does not need 400 execute stages.
- **Stateful middle is another gate.** Inflate cannot live in the gap (it needs
  `i-1`’s inflate). It can live on `k` with one shared `z_stream`, like
  `dictbuf`. Decompress of one gzip is three stages, `cap >= 3`: read-ahead /
  inflate / write-behind. Channel reader/inflater/writer is the dedicated-role
  spelling of the same concurrency.
- **Later units, not later names.** The next name already waits on this stage.
  `accept` / `read` / “any children?” in the stage *is* admit. `fail` / `break`
  is EOF. Depth cap is in-flight; `n` is “enough names” or a known cut.
- **`CCTurnstile t@(cap, n_stages, arena)`** is N gates, index `k`.
  `CCTurnstileRW` is only `read`/`write` aliases on `stages[0]`/`[1]`. There is
  no `ts.inflate` spelling; `n_stages == 3` is enough.
- **Prepare-commit** is a *set* of names (two join arms, then `hold_sorted`),
  not a pipeline. A line is what you write in the body, not a law of names.

## Same machine (already or easily)

| | Admit / read | Middle | Publish / write |
|---|---|---|---|
| Seekable pigz | `pread` + dict hop | deflate | CRC + `.gz` |
| Find | tile = unit | `memmem` | hit list |
| Island | 2 MiB span | count LFs | `isle_before` |
| Turnstile recipe | name check | — | name check |
| Accept / echo / redis-idiomatic | `accept` in a stage, **or** kick `while` | `handle` / `while (recv)` | none (sock is the sink), or a short ordered log |
| Stdin pigz | unit born in `read` | compress | emit |
| Decompress (one gzip) | chunk | — | inflate on a gate, then write |
| Owner execute | optional | decode / conn loop | `@stage (t, exec, i)` |

Find / browse / island already dest-live wrap so the frame is not the loop:
`{ @serial { noop }; run }` and wait-for inside `run`. Browse adds **waves**
(stage grows jobs; next `0..n` is a new construct). A directory tree is not
one planted range; each wave is the drawing.

`pigz_channel` / hybrid / pthread and `recipe_ordered_parallel` are the
reader / `send_task` / writer spelling of seekable pigz — historical, not a
different law.

## What does not need `0..MAX`

A long-lived accept is the **kick’s inner loop**, not an infinite wait-for:

```ccs
CCParallel h = @parallel {
    @serial {
        for (;;) {
            sock = ln.accept() !>(e) { break; };
            handle(sock);
        }
    }
} !>;
```

Tickets stay where there is a **cut** (`0..n` tiles, `0..cap` runners, a batch).
An open-range `for (i in ..)` is a small language fix if we insist every unit
is a planted name. We do not have to. `int` overflow is the same fork.

Wait-for dest joins before the statement ends. The frame that must return
keeps find’s wrapper. That is not a nursery.

## Gaps that stay a nursery or channel

These are unfinished faces or real limits — not “use a bag because tickets
cannot N:1.”

### 1. Host object (curl `Curl_thrdq`)

`send` / `recv` / `AGAIN` / `clear` / `leave` are libcurl’s C ABI. Produce is
off this `for`. Tickets can park on an exclusive bag the host fills; we never
spelled that object as wait-for.

Still on the bag until a face exists:

- **Retract** — take a queued job back (`clear`). Not `fail` the next name.
- **Detach** — `leave` + leftover; do not join in-flight `getaddrinfo`.
  Dest `.wait()` joins.
- **Poll empty** — `AGAIN`, not a parked ticket.
- **Grow/shrink runners** — `min` / `max` / idle after plant. Tickets `0..max`
  are a fixed runner set.

Work: a host-queue spelling (or document “this ABI is the bag”) — gap, not a
mismatch.

### 2. Denial / same stack

Spawned `@parallel` arms are fibers. `adopt` is cancel-only, not join. The
**first** arm is the caller (kick). Lowering order: **spawn arms 1..n**, then
run arm 0 on the caller, then join (inline denied arms at the label). Wait-for
is not adaptively denied.

`seq`, `#pragma(@parallel) off`, spawn failure, and `cc_parallel_deny_fast`
can run siblings on that stack instead of as fibers. Nursery `spawn` does not
take the adaptive deny path — same scheduler, different contract.

**Loud dead OK; silent dead not.** Spec §8.11.7: a hang from cross-arm
rendezvous should be diagnosed (park reason, deadlock watchdog). That is
acceptable. What is not acceptable is a mute hang or a path that looks like
success when concurrency was required:

| Outcome | OK? |
|---|---|
| Deny, no rendezvous — same result, slower | yes |
| Rendezvous needed, sibling denied, caller parks — detector fires | yes (loud) |
| Same hang, caller not in the graph / suppressed / no verdict | no (silent) |
| Wrong result because deny serialized a pipe | no (silent correctness) |

**Policy:** `@parallel { }` may deny. `@parallel spawn { }` never denies
spawned arms. A brace join that names a blocking channel op, or captures
a channel (helpers, dest-live consume), is ill-formed unless it is
`spawn` or `#pragma(@parallel) off`. Hidden channel leftover: abort if
that join parks, else the detector. Meeting admit does not take
`deny_fast` / flood-deny; a failed admit aborts (`cc_parallel_die`) —
it does not inline the sibling after the kick. `n.spawn` is the bag.

Do **not** need “always spawn 0.” Arm 0 on the caller is kick / dest-live; when
siblings are spawned first, a parked `send` on the caller can still schedule
the consumer fiber. The silent case is **deny arm 1+** while arm 0 is mid-park.

A sloppy but loud escape hatch: `@serial { noop = 0; }` as first arm so kick
semantics stay, real work is arm 1+ — if rendezvous still deadlocks, the
detector should say so. Prefer detect→REAL over relying on that shape.

**Ambiguous for detection:** proving same channel pair, buffer cap vs send
count, `try_send`, 3+ arm pairwise analysis, `#pragma off` / `seq` (user chose
sequential).

### 3. One `@stage` per planted name — not a gap

`@stage` is once per `(name, gate)` in that ticket’s body. A new unit is a
new name. Keep-alive is a C `while (recv)` — it does not restage the same
name. `i-1` then `i` is publish order when you plant a range, not a
hardcoded name table.

### 4. Graph, not a gate — not a gap

A mesh is a mesh (`random_access` hops, shard exclusives, prepare-commit
holds). It is not owner-N:1 and does not need a `k+1` face. Channels and
named exclusives stay the spelling.

### 5. EMPTY-close as a registration

On-page stream is closed: produce calls `tx.close()`; `.wait()` joins both
(`recipe_parallel_stream.ccs`). That is not a registration.

Nursery `n.close(tx)` arms **EMPTY**: last child dead, possibly on a
worker, on both `wait` and `leave`, without the owner sitting in `wait` to
close by hand. Spec §8.1.4. Dest `h.close(tx)` is the same pair on a dest
(§8.11). `h.wait()` joins this dest’s `n` + `tasks[]` only. Adopt is
cancel-only: `h1.adopt(h2)` does not make `h1.wait()` wait `h2`.

| Spelling | Status |
|---|---|
| Produce arm `tx.close()` | Taught default. No dest API. |
| `n.close(tx)` + EMPTY | Bag. Dest-live / leave / leftover. |
| `h.close(tx)` + dest EMPTY / `.leave()` | Same pair. Consumer already in `recv`; producer set is another dest (`recipe_parallel_empty.ccs`). Leftover LEFT-only. No `.abandon`. Wait-for dest refuses `leave` (construct joins). |

EMPTY closes **this** dest’s join set. A sibling consumer on the same dest
does not unblock there. Waiting a dest that still owns the parked consumer
does not fire that close.

### 6. Dedicated prefetch — not a gap

`CCTurnstileRW` is only `read`/`write` names on `stages[0]`/`[1]`. The
turnstile is `n_stages` gates. Read-ahead is a ticket at the read gate while
another name is in compute or write (`cap >= 2`, or `n_stages >= 3`). Whoever
sits in that gate *is* the reader. A slave that reads when **no** name is
there (pigz `load_read` double buffer) is another person — optional host
IO, not a missing face.

## What the consumer still does not know

The bound, when it exists, cuts **work**. The consumer of hits, bytes, or
conns only knows until stop (`close`, `done`, listener down, `destroy`).
Units ≠ tickets. Find’s listing is a stream even when tiles are `0..n`.

## Optional follow-ups (if we do not back away)

Do not do these unless we choose the drawing as the taught server/pigz story.

- Teach the sandwich in `docs/cheatsheet.md` / `recipe_parallel.ccs`: gap =
  overlap, stage = loop-carried, N:1 = write, read optional.
- On-page stream taught: `recipe_parallel_stream.ccs`, cheatsheet `@parallel`
  + Channels. Dest EMPTY: `recipe_parallel_empty.ccs`. Nursery twin:
  `recipe_channel_pipeline.ccs`.
- Dest `.leave` / EMPTY are the nursery pair on a dest (`h.close` /
  `h.leave`). Adopt is still cancel-only. The bag keeps late admit /
  host ABI / never-deny.
- Pigz decompress: try `n_stages == 3` + shared `zs` on inflate; keep the
  channel pipeline as the dedicated-role twin until numbers exist.
- Stdin compress: admit in `read`, same helpers as seekable; or leave serial.
- Redis owner: execute as write stage; measure vs owner fiber.
- Echo: dest-live kick `while` vs accept-in-`read` + `handle` in the gap —
  a toy next to `recipe_tcp_echo`, not a silent rewrite.
- Open range / wider index only if we plant names for process-lifetime units.
- Host-queue face for `Curl_thrdq` (1) if we want tickets on that ABI.

## Do not

- Add `@parallel while` to fake a bag.
- Put `handle` in `write` unless the sink is shared (serial server).
- Rewrite `thrdqueue` into `@parallel` without retract/leave/`AGAIN`.
- Treat nursery-vs-`@parallel` as the concurrency choice. Teach **cut /
  named gate / who owns the set**.
- Edit `spec/` with this archaeology; the spec stays present-tense normative.
  This file and git hold the reasoning.

## Tests and specimens to keep honest

- Wait-for cancel / `fail` / `break` / dest: `tests/parallel_wait_*.ccs`,
  `parallel_cancel_wake_smoke.ccs`
- Two-stage line: `examples/recipe_turnstile.ccs`, `pigz_idiomatic.ccs`
- Dest-live kick: raytext `find.ccs` / `browse.ccs` / `piece_tree.ccs`
- Bag + host: `real_projects/curl_dns_port/thrdqueue.ccs`
- Mesh: `real_projects/random_access/ra_dist.ccs`
- On-page stream: `examples/recipe_parallel_stream.ccs`
- Dest EMPTY-close: `examples/recipe_parallel_empty.ccs`,
  `tests/parallel_dest_empty_recv_smoke.ccs`
- Nursery EMPTY-close: `examples/recipe_channel_pipeline.ccs`, `pigz_channel.ccs`
- Join deny / serial schedule: spec §8.11.7; a visible channel on a
  brace join is `@parallel spawn` (`tests/parallel_rendezvous_unbuf_smoke.ccs`,
  `parallel_rendezvous_helper_smoke.ccs`,
  `parallel_rendezvous_dest_live_smoke.ccs`). Unmarked is
  `parallel_chan_needs_spawn_fail.ccs`. Denied join + channel park
  aborts (`parallel_deny_park_abort_smoke.ccs`). Meeting admit does
  not inline (`tests/parallel_spawn_admit_shape_smoke.c`). Ungated recursive
  `@parallel { }` stays CHURN (`parallel_adapt_churn_smoke.ccs`); join
  must not pin REAL.
- Dest leave / EMPTY: `tests/parallel_dest_leave_smoke.ccs`
