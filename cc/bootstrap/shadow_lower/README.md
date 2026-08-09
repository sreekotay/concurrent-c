# shadow_lower bootstrap snapshots

Committed seed for the **default native front**. Deliberate freezes of the
lowered C for `cc/shadow/shadow_lower.ccs` plus its lowered
local-header tree. Stage-0 builds the lowerer with host `cc` without
re-lowering that `.ccs`.

| Path | Role | Git |
|------|------|-----|
| `latest/` | Scratch emit from the current tree | Ignored |
| `vN/` | Promoted snapshot | Committed when you choose |
| `last-good` | Pointer to the active `vN` | Committed |

Keep `last-good` plus one or two prior `vN` for rollback. Older seeds are
safe to delete once cold smoke is green — they are not needed to build.

**Source of truth is only `cc/shadow/*.ccs` / `*.cch`.** Every lowerer
behavior fix (parse, emit, UFCS, closures, …) is edited there first. Snapshot
`vN/` trees and `out/include/cc/shadow/*.h` are regenerate-only products —
never the place you land a fix.

### Never (common failure modes)

| Don't | Why | Do instead |
|-------|-----|------------|
| Edit `out/include/cc/shadow/*.h` to fix a bug | Bootstrap `make -C cc` recopies `last-good` over those headers; the bug returns and the `.cch` never changed | Edit `cc/shadow/*.cch`, then ccs rebuild → snapshot → promote |
| Hand-edit `vN/include/*.h` or `vN/shadow_lower.c` | Committed seeds are regenerate-only; in-place patches skip promote and diverge from `.cch` | Edit `cc/shadow/*.cch`, then snapshot → promote |
| Patch `last-good`'s tree to "land" a fix | Leaves `last-good` pointing at a mutated seed; next cold clone rebuild is wrong | Promote a new `vN` and flip `last-good` |
| `cp` raw `cc/shadow/*.cch` → `out/include/cc/shadow/*.h` | Relative includes and snapshot path-rewriting disagree; looks synced, tree is broken | Rebuild via `SHADOW_LOWER_SOURCE=ccs`, then snapshot |
| Assume default `make -C cc` picked up `.cch` edits | Default builds from `last-good` (`SHADOW_LOWER_SOURCE=bootstrap`) | `SHADOW_LOWER_SOURCE=ccs` while iterating; promote when the face should stick |
| Lower a single face `.cch` with `shadow_lower …/pp_emit_ufcs.cch -o …` | Those files are include fragments, not standalone TUs; the lowerer can hang | Change the `.cch`, rebuild the whole lowerer, snapshot |

`out/include/cc/shadow/*.h` is a **build product** (seeded from `last-good`
on bootstrap builds, copied into `latest/` by snapshot). It is not a second
source tree.

## Iterate → ship

Repo-wide “when to run what” (stdlib vs lowerer vs cold smoke):
[docs/build-when.md](../../../docs/build-when.md).

This directory is only about freezing `shadow_lower` seeds:

```bash
./scripts/iterate_shadow_lower.sh                 # edit-loop: ccs rebuild only
./scripts/iterate_shadow_lower.sh --ship --smoke  # freeze: ccs → snapshot → promote → bootstrap build
```

| Step | Command | Effect |
|------|---------|--------|
| Edit | `cc/shadow/*.cch` | Source of truth |
| Rebuild lowerer | `./scripts/iterate_shadow_lower.sh` | Seed re-lowers from sources |
| Ship seed | `./scripts/iterate_shadow_lower.sh --ship [--smoke]` | Snapshot → new `vN` → flip `last-good` → host-cc |
| Commit | `git add …/last-good …/vN` | Human-gated |
| Cold check | `./scripts/smoke_bootstrap_fresh.sh` then `./scripts/smoke_i386.sh` | Before push |

A fix is not in the committed seed until promote has created the new `vN`
and `last-good` points at it.

## Default self-build

`make -C cc` builds `shadow_lower` from `last-good` (`SHADOW_LOWER_SOURCE=bootstrap`).

```bash
# Same as ./scripts/iterate_shadow_lower.sh
make -C cc SHADOW_LOWER_SOURCE=ccs ../out/cc/bin/shadow_lower

# Explicit aliases
make -C cc shadow_lower-from-bootstrap
make -C cc shadow_lower-from-ccs
```

## Snapshot / promote

```bash
# Emit into latest/ (native self-emit; requires shadow_lower binary)
./scripts/snapshot_shadow_lower.sh
./scripts/snapshot_shadow_lower.sh --smoke

# Promote latest/ → vN and point last-good at it (then commit)
./scripts/promote_shadow_bootstrap.sh          # next vN
./scripts/promote_shadow_bootstrap.sh 3        # explicit v3
```

**Before pushing a new `last-good`, run cold checks** (see
[build-when.md → Verify cold](../../../docs/build-when.md#verify-cold--second-platform)).
`--smoke` on ship only proves this machine can host-cc the snapshot.

```bash
./scripts/smoke_bootstrap_fresh.sh   # wiped out/ on this host
./scripts/smoke_i386.sh              # Linux ILP32 Docker (RO mount)
# optional: CCC_HOST_CC=tcc ./scripts/smoke_i386.sh
```

Cold `make -C cc -jN` must succeed without a warm `out/` (seed stamp is
`out/runtime/.seed.stamp`, separate from `.headers_lowered.stamp`). If a
promote breaks that, fix the seed/Makefile before landing `last-good`.

`snapshot_shadow_lower.sh` rewrites both absolute quoted includes and
repo-relative angle includes (`<cc/shadow/foo.h>` → `"foo.h"`) so the
committed `vN/include/` tree is self-contained. Do not promote a `latest/`
that still contains `<cc/shadow/...>`.

Serdes can parse/emit `shadow_lower.ccs` (umbrella `.cch` passthrough + growable
stage2 buffer). Header `static_map` fragments splice at EOF when markers are
absent (after grammar + umbrella `#include`s). Snapshot is native self-emit only
(`scripts/snapshot_shadow_lower.sh --smoke`).