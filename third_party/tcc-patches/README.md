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

**0001-cc-ext-hooks.patch** - All CC extensions to TCC in a single patch:

- AST stub recording system for CC visitor passes
- External parser hooks (`ext_parser`) for CC syntax
- UFCS (Uniform Function Call Syntax) support
- Statement extensions: `@defer`, `spawn`, and parser-level rejection of retired `@arena` / `@nursery`
- Expression extensions: `await`, closures (`=>` syntax)
- New tokens: `TOK_CC_ARROW` for `=>`

All extensions are guarded by `#ifdef CONFIG_CC_EXT`.

## Files Modified

| File | Changes |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | AST node types, TCCState extensions, new tokens |
| `cc_ast_record.h` | AST recording helpers and node metadata capture |
| `libtcc.c` | `cc_tcc_parse_to_ast()` API, include path setup |
| `tccgen.c` | Statement/expression parsing extensions, better error messages |
| `tccpp.c` | `=>` arrow token lexing |

## Upstream Compatibility

- Submodule source: `https://github.com/sreekotay/tinycc.git`
- Pinned CC branch: `mob`
- Upstream baseline for patch regen: `origin/upstream-mob`
- Extensions are isolated behind `CONFIG_CC_EXT`
- Goal: keep changes minimal and easy to rebase
- Push target for CC TCC changes: `origin/mob`
- Note: `third_party/tcc` is often checked out in detached-HEAD state because the parent repo pins a specific submodule commit. When that happens, push with `git push origin HEAD:mob` or first create a local branch that tracks `origin/mob`.
