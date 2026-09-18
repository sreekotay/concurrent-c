# TCC Patches for Concurrent C

TinyCC support does not require a CC language fork of TCC; the current
compatibility patch is ~1.8 KB against a pinned pristine upstream mob.

This directory contains that patch: the `CONFIG_CC_EXT` hooks TinyCC needs
for CC comptime / `libtcc`.

## Documentation

| Document | Purpose |
|----------|---------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Patch workflow, build configuration, common mistakes |
| [HOOKS.md](HOOKS.md) | Technical reference for TCC extension points and APIs |

## Quick Start

```bash
# Apply patches (after clone or submodule update)
make tcc-patch-apply

# Build TCC with CC extensions
make tcc

# After modifying TCC sources, regenerate the patch
make tcc-patch-regen
```

## Patch File

**0001-cc-ext-hooks.patch** — the whole dirty-tree delta (`make tcc-patch-regen`).
Product syntax lowering is the lowerer, `cclower_cc`; lowered C is ordinary C.

- `CONFIG_CC_EXT` build flag wiring + `CC_TCC_EXT_AVAILABLE` + `CC_TCC_EXT_ABI`
- `pp_line` negative-delta fix (preserve user `#line` resumes)
- dwarf `unsigned i` locals (quiet `-Wsign-compare`)
- ARM EABI function frame is 16 bytes (`push {r10,fp,ip,lr}`) so `fp` stays
  8-aligned. A 12-byte `{fp,ip,lr}` frame left 8-byte locals / `_Atomic uint64_t`
  at 4-mod-8 (SIGBUS on `LDREXD` under qemu-user and strict ARM).

Retired (do not reintroduce): stub-AST recording, parse-to-ast,
`TCCExtParser`, UFCS host-parse tolerance, `TOK_CC_ARROW` (`=>`),
`CC_REC_*`, column/`cc_tok_off` tracking.

`CONFIG_CC_EXT` extensions are guarded by `#ifdef CONFIG_CC_EXT`.
The ARM frame change is unconditional under `TCC_ARM_EABI`.

## Files Modified

| File | Changes |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | `CC_TCC_EXT_AVAILABLE`; `CC_TCC_EXT_ABI`; dwarf loop index type |
| `tccpp.c` | `#line` negative-delta swallow fix |
| `arm-gen.c` | EABI 16-byte frame + matching epilog / param offsets |

## Upstream Compatibility

- Submodule source: `https://github.com/sreekotay/tinycc.git`
- Superproject pin: pristine `origin/upstream-mob` (no CC commits in the gitlink)
- Patch regen baseline: `origin/upstream-mob`
- Extensions are isolated behind `CONFIG_CC_EXT`
- Goal: keep changes minimal and easy to rebase
- CC hook edits ride `0001-cc-ext-hooks.patch` in this repo (`make tcc-patch-regen`);
  the applied working tree is intentionally dirty (`.gitmodules` `ignore = dirty`)
- Optional: experimental / upstream-bound work may live on fork `mob`; do not pin it
