# shadow_lower bootstrap snapshots

Deliberate freezes of the lowered C for `examples/serdes/c/shadow_lower.ccs`
plus its lowered local-header tree. Stage-0 builds the lowerer with host `cc`
without re-lowering that `.ccs`.

| Path | Role | Git |
|------|------|-----|
| `latest/` | Scratch emit from the current tree | Ignored |
| `vN/` | Promoted snapshot | Committed when you choose |
| `last-good` | Pointer to the active `vN` | Committed |

**Source of truth** remains `examples/serdes/c/*.ccs` / `*.cch`. Snapshot
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
./scripts/snapshot_shadow_lower.sh --legacy   # prefer for promote until serdes
                                             # self-emit host-ccs cleanly

# Promote latest/ → vN and point last-good at it (then commit)
./scripts/promote_shadow_bootstrap.sh          # next vN
./scripts/promote_shadow_bootstrap.sh 3        # explicit v3
```

Serdes can parse/emit `shadow_lower.ccs` (umbrella `.cch` passthrough + growable
stage2 buffer). Header `static_map` fragments splice at EOF when markers are
absent (after grammar + umbrella `#include`s). Prefer a serdes snapshot once
`scripts/snapshot_shadow_lower.sh --smoke` is green; `--legacy` remains valid.