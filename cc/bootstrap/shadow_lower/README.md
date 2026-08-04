# shadow_lower bootstrap snapshots

Deliberate freezes of the lowered C for `examples/serdes/c/shadow_lower.ccs`
plus its lowered local-header tree. Stage-0 builds the lowerer with host `cc`
without re-lowering that `.ccs` through the legacy frontend.

| Path | Role | Git |
|------|------|-----|
| `latest/` | Scratch emit from the current tree | Ignored |
| `vN/` | Promoted snapshot | Committed when you choose |
| `last-good` | Pointer to the active `vN` | Committed |

**Source of truth** remains `examples/serdes/c/*.ccs` / `*.cch`. Snapshot
contents are regenerate-only — do not hand-edit.

## Commands

```bash
# Emit into latest/ (rewrites absolute includes for portability)
./scripts/snapshot_shadow_lower.sh

# Optional host-cc smoke of latest/ (needs existing runtime/tcc objs)
./scripts/snapshot_shadow_lower.sh --smoke

# Promote latest/ → vN and point last-good at it (then commit)
./scripts/promote_shadow_bootstrap.sh          # next vN
./scripts/promote_shadow_bootstrap.sh 3        # explicit v3
```

Build the promoted seed (does not change the default self-build):

```bash
make -C cc shadow_lower-from-bootstrap
```

Default `make -C cc` still lowers `shadow_lower.ccs` via `--frontend=legacy`.
