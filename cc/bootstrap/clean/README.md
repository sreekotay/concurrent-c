# clean lowerer bootstrap seeds

Committed seeds for the **clean lowerer**, the default front. A seed is the
lowered C of its four tools (`cclex`, `ccparse`, `cclower`, `ccindex`), the C
of module `lower` they share (`lower_cch.c`), and the lowered faces that C
includes. Stage zero of `make -C cc` host-compiles the seed under
`last-good` into `out/cc/bin/*_cc` with no lowerer in hand.

| Path | Role | Git |
|------|------|-----|
| `latest/` | Scratch snapshot from the current tree | Ignored |
| `MAJOR.MINOR.PATCH-N/` | Promoted seed | Committed when you choose |
| `last-good` | The seed stage zero builds from | Committed |

**Source of truth is `cc/lower/*.cch` / `*.ccs`.** A seed is regenerate-only:
never edit a pin, and never edit `out/.cc-build/clean/cc/lower/*` to land a
fix.

## Iterate, gate, ship

```bash
make -C cc lower-cc                 # rebuild the tools from source with the tools in hand
./scripts/lowerer_selfhost.sh       # gen0 builds gen1 builds gen2: the fixed point
./scripts/ship_clean_seed.sh        # snapshot → latest/
./scripts/ship_clean_seed.sh --promote   # …→ MAJOR.MINOR.PATCH-N, last-good
git add cc/bootstrap/clean/last-good cc/bootstrap/clean/MAJOR.MINOR.PATCH-N
```

Ship a seed when a lowerer change should reach a fresh checkout. The
selfhost gate is the freeze check: a seed whose tools do not reproduce
themselves from source is not a seed. Cold check before pushing a seed:
`./scripts/smoke_bootstrap_fresh.sh`.

Keep `last-good` plus one or two prior seeds for rollback.
