# TCC Patches for Concurrent C

This directory contains patches to TCC (Tiny C Compiler) that enable CC language extensions.

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

**0001-cc-ext-hooks.patch** — minimal `CONFIG_CC_EXT` surface for libtcc
(comptime / `cpp_expand` / `--exe`). Product syntax lowering is native
`shadow_lower`; lowered C is ordinary C.

- `CONFIG_CC_EXT` build flag wiring + `CC_TCC_EXT_AVAILABLE`
- `pp_line` negative-delta fix (preserve user `#line` resumes)
- dwarf `unsigned i` locals (quiet `-Wsign-compare`)

Retired (do not reintroduce): stub-AST recording, parse-to-ast,
`TCCExtParser`, UFCS host-parse tolerance, `TOK_CC_ARROW` (`=>`),
`CC_REC_*`, column/`cc_tok_off` tracking.

All extensions are guarded by `#ifdef CONFIG_CC_EXT`.

## Files Modified

| File | Changes |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | `CC_TCC_EXT_AVAILABLE`; dwarf loop index type |
| `tccpp.c` | `#line` negative-delta swallow fix |

## Upstream Compatibility

- Submodule source: `https://github.com/sreekotay/tinycc.git`
- Pinned CC branch: `mob`
- Upstream baseline for patch regen: `origin/upstream-mob`
- Extensions are isolated behind `CONFIG_CC_EXT`
- Goal: keep changes minimal and easy to rebase
- Push target for CC TCC changes: `origin/mob`
- Note: `third_party/tcc` is often checked out in detached-HEAD state because the parent repo pins a specific submodule commit. When that happens, push with `git push origin HEAD:mob` or first create a local branch that tracks `origin/mob`.
