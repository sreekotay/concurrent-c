# Lowerer bootstrap seeds

Committed seeds of the lowerer, one line of pins named
`MAJOR.MINOR.PATCH-SEED`. `last-good` names the pin a fresh checkout builds
the compiler from; `ccc --version` prints it. Stage zero of `make -C cc`
host-compiles the seed with no lowerer in hand.

Two shapes of pin:

| Pin | Holds | Stage zero builds |
|-----|-------|-------------------|
| `0.4.0-400` and later | `cclex.c`, `ccparse.c`, `cclower.c`, `ccindex.c`, the C of module `lower` (`lower_cch.c`), and the lowered faces that C includes | `out/cc/bin/*_cc`, the clean lowerer's tools |
| `0.3.4-*` | `shadow_lower.c` plus `include/*.h` | `shadow_lower`, the retired front, only for a unit whose `version=` pin selects the 0.3 line (`make -C cc shadow_lower-pin PIN_VER=… PIN_OUT=…`, run by `ccc` itself) |

| Path | Role | Git |
|------|------|-----|
| `latest/` | Scratch snapshot from the current tree | Ignored |
| `MAJOR.MINOR.PATCH-SEED/` | Promoted seed | Committed when you choose |
| `last-good` | The seed stage zero builds from | Committed |

**Source of truth is `cc/lower/*.cch` / `*.ccs`.** A seed is regenerate-only:
never edit a pin, and never edit `out/.cc-build/clean/cc/lower/*` to land a
fix. The 0.3 pins have no sources in the tree; they are frozen C.

## Iterate, gate, ship

```bash
make -C cc lower-cc                 # rebuild the tools from source with the tools in hand
./scripts/lowerer_selfhost.sh       # gen0 builds gen1 builds gen2: the fixed point
./scripts/ship_seed.sh              # snapshot → latest/
./scripts/ship_seed.sh --promote    # …→ MAJOR.MINOR.PATCH-SEED, last-good
git add cc/bootstrap/lowerer/last-good cc/bootstrap/lowerer/MAJOR.MINOR.PATCH-SEED
```

Ship a seed when a lowerer change should reach a fresh checkout. The
selfhost gate is the freeze check: a seed whose tools do not reproduce
themselves from source is not a seed. Cold check before pushing a seed:
`./scripts/smoke_bootstrap_fresh.sh`.

Keep `last-good` plus one or two prior seeds for rollback.
