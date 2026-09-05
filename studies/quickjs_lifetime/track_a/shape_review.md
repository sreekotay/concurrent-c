# Track A — shape review (§10–11)

## Temporary vs persistent vs ended vs destroyed

| Distinction | Where stated (CC) | Where stated (Rust) |
|-------------|-------------------|---------------------|
| Temporary eval value | `HostileValue` + `hostile_value_drop` | `HostileValue` + `drop_value` |
| Persistent claim | thin `HostileClaim` → `HostileClaimBody` | `HostileClaim` / `Persistent` |
| Borrow (call-scoped) | `HostileBorrow` begin/end | `HostileBorrow` begin/end |
| Weak observation | outer arena (second lifetime) | `Persistent` WeakRef |
| Callback registration | thin `HostileRegistration` → `HostileRegBody` | same semantics |
| Gen reclaim | `ep_rotate` / `ep_wheel_rotate` + park/destroy | n/a (study-local CC) |
| Mid-gen densify | wheel: compact when live &lt; ⅔ born; patch handles | n/a |
| Realm destroyed | store/wheel destroy then `cc_qjs_close` | `destroy` |

## Wheel indirection

- **Handle:** `sizeof(HostileClaim) == 16` (`body*` + sticky `freed`; pool freelist
  overwrites only the first word).
- **Body:** `HostileClaimBody` (~80 B) in the current wheel gen arena.
- **Release:** body unlinked; handle returned to `CCArenaPool` freelist.
- **Compact:** densify live bodies into a fresh gen arena; patch
  `handle->body`; destroy the sparse arena. External `HostileClaim*` stay stable.

## Infection radius

| Path | Knew about Claim / EpStore? | Notes |
|------|----------------------------|-------|
| `track_a/cc/epoch_store.ccs` | EpStore only | wstore5 kernel; hoist candidate |
| `track_a/cc/epoch_wheel.ccs` | wheel + pools | ring + freelist handles |
| `track_a/cc/hostile.ccs` | yes | contract + compact |
| `track_a/cc/driver.ccs` | Claim + epoch boundary | |
| `ccc/script/quickjs.cch` | **no** | bootstrap only |
| other stdlib | **no** | |

## Preregistered outcome (A–E)

- **Chosen:** **A** (containment) with a study-local path toward reclaim form.
- **Rationale:** Frozen contract unchanged; claim/reg bodies moved into
  epoch/wheel without leaking into stdlib. Weaks stay on the outer arena.

## Scale notes (`HOSTILE_SCALE=x10` / `x100`, pinned ng `df836d1f`)

| | epoch | wheel (thin + mid-gen compact) |
|--|-------|--------------------------------|
| retain ns/op | ~13–14 | ~14–15 |
| release ns/op | ~14–15 | ~15 |
| reclaim META | parks/destroys | +compacts / compact_moved |
| oracle | PASS | PASS |

RSS after teardown is a poor reclaim oracle (OS retention + JS heap). Prefer
`parks` / `destroys` / `compacts` / `compact_moved` and arena gross when available.

## Receipts

- See `results/20260904_*_{epoch,wheel}_*.txt` after local runs.
