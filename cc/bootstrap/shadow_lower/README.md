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

**Source of truth** remains `cc/shadow/*.ccs` / `*.cch`. Snapshot
contents are regenerate-only — do not hand-edit.

## Default self-build

`make -C cc` builds `shadow_lower` from `last-good` (`SHADOW_LOWER_SOURCE=bootstrap`).

```bash
# Iterate on .ccs before promoting a new seed
make -C cc SHADOW_LOWER_SOURCE=ccs ../out/cc/bin/shadow_lower

# Explicit aliases
make -C cc shadow_lower-from-bootstrap
make -C cc shadow_lower-from-ccs
```

## Snapshot / promote

```bash
# Emit into latest/ (prefers existing shadow_lower; --legacy forces ccc)
./scripts/snapshot_shadow_lower.sh
./scripts/snapshot_shadow_lower.sh --smoke
./scripts/snapshot_shadow_lower.sh --legacy   # prefer for promote until native self-emit is clean
                                             # self-emit host-ccs cleanly

# Promote latest/ → vN and point last-good at it (then commit)
./scripts/promote_shadow_bootstrap.sh          # next vN
./scripts/promote_shadow_bootstrap.sh 3        # explicit v3
```

**Before flipping `last-good`, run a cold rebuild on a second platform.**
`--smoke` only proves the generating machine can host-cc the snapshot; it does
not catch macOS-only path resolution or GNU ld ODR issues.

```bash
# Same machine, wiped products (simulates fresh clone after tcc is built)
./scripts/smoke_bootstrap_fresh.sh

# Linux ILP32 (Docker) — highest-leverage catch for Darwin-only promotes
./scripts/smoke_i386.sh
# optional: CCC_HOST_CC=tcc ./scripts/smoke_i386.sh
```

`snapshot_shadow_lower.sh` rewrites both absolute quoted includes and
repo-relative angle includes (`<cc/shadow/foo.h>` → `"foo.h"`) so the
committed `vN/include/` tree is self-contained. Do not promote a `latest/`
that still contains `<cc/shadow/...>`.

Serdes can parse/emit `shadow_lower.ccs` (umbrella `.cch` passthrough + growable
stage2 buffer). Header `static_map` fragments splice at EOF when markers are
absent (after grammar + umbrella `#include`s). Prefer a native snapshot once
`scripts/snapshot_shadow_lower.sh --smoke` is green; `--legacy` remains valid.