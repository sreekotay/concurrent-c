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

**0001-cc-ext-hooks.patch** — CC extensions to TCC (shrunk after legacy-front removal):

- `CONFIG_CC_EXT` build flag wiring
- UFCS host-parse tolerance (still present; product UFCS is `shadow_lower`)
- `=>` lexing (`TOK_CC_ARROW`)

Retired (no longer in the patch): stub-AST recording (`cc_ast_record*`),
`cc_tcc_parse*_to_ast`, and `TCCExtParser` hooks — those existed for the
deleted multipass `--frontend=legacy` visitor.

All extensions are guarded by `#ifdef CONFIG_CC_EXT`.

## Files Modified

| File | Changes |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | UFCS scratch fields on `TCCState`, `TOK_CC_ARROW` |
| `tccgen.c` | UFCS rewrite / tolerance under `CONFIG_CC_EXT` |
| `tccpp.c` | `=>` arrow token lexing |

## Upstream Compatibility

- Submodule source: `https://github.com/sreekotay/tinycc.git`
- Pinned CC branch: `mob`
- Upstream baseline for patch regen: `origin/upstream-mob`
- Extensions are isolated behind `CONFIG_CC_EXT`
- Goal: keep changes minimal and easy to rebase
- Push target for CC TCC changes: `origin/mob`
- Note: `third_party/tcc` is often checked out in detached-HEAD state because the parent repo pins a specific submodule commit. When that happens, push with `git push origin HEAD:mob` or first create a local branch that tracks `origin/mob`.
