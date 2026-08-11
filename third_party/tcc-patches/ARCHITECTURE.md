# TCC Patch Architecture

Concurrent-C extends TCC with a small `CONFIG_CC_EXT` surface used by libtcc
(comptime / `--exe`). Product syntax lowering is native `shadow_lower`.

> See also: [HOOKS.md](HOOKS.md), [README.md](README.md).

## Key Principle

**TCC modifications live in a fetchable forked submodule history, and the patch file snapshots that delta against a mirrored upstream base.**

```
third_party/tcc/           ← Git submodule pinned to sreekotay/tinycc
    │                        (`mob` carries the CC hook commits)
    │
    ▼
third_party/tcc-patches/   ← Patch snapshot vs upstream-mob
    └── 0001-cc-ext-hooks.patch
```

## Files Modified by Patches

| File | Purpose |
|------|---------|
| `Makefile` | `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | UFCS scratch fields, `TOK_CC_ARROW` |
| `tccgen.c` | UFCS host-parse tolerance |
| `tccpp.c` | `=>` arrow lexing |

Stub-AST recording and `TCCExtParser` were removed with the legacy front.

## Workflow

```bash
git submodule update --init --recursive
make tcc-patch-apply
# edit third_party/tcc/ …
make tcc-patch-regen
# commit patch (+ optional submodule mob push) in the parent repo
```

## Build

```bash
cd third_party/tcc && ./configure --config-cc_ext && make libtcc.a tcc libtcc1.a
```

CC defines: `-DCONFIG_CC_EXT`, `-DCC_TCC_EXT_AVAILABLE`.
