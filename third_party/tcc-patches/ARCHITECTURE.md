# TCC Patch Architecture

Concurrent-C extends TCC with a small `CONFIG_CC_EXT` surface used by libtcc
(comptime / `--exe`). Product syntax lowering is native `shadow_lower`.

> See also: [HOOKS.md](HOOKS.md), [README.md](README.md).

## Key Principle

**The submodule pin is a pristine upstream mirror. The CC delta lives only in
this repo's patch file.**

```
third_party/tcc/           ← Git submodule pinned to origin/upstream-mob
    │                        (clean TinyCC; no CC commits in the pin)
    │
    ▼  make tcc-patch-apply  (working tree becomes dirty; ignore=dirty)
third_party/tcc-patches/
    └── 0001-cc-ext-hooks.patch   ← single upstream-mirror → tree diff
```

No tinycc-fork pushes are required for day-to-day CC hook work. Optional fork
`mob` history may still carry experimental or upstream-bound commits; the
superproject does not pin them.

## Files Modified by Patches

| File | Purpose |
|------|---------|
| `Makefile` | `-DCONFIG_CC_EXT` when `CONFIG_cc_ext=yes` |
| `tcc.h` | `CC_TCC_EXT_AVAILABLE`; dwarf `unsigned i` |
| `tccpp.c` | `#line` negative-delta fix |

Stub-AST, ExtParser, UFCS tolerance, and `=>` lexing were removed once
product lowering no longer needed them in libtcc.

## Workflow

```bash
git submodule update --init --recursive   # pristine upstream-mob pin
make tcc-patch-apply                      # dirty working tree
# edit third_party/tcc/ …
make tcc-patch-regen                      # rewrite 0001-cc-ext-hooks.patch
# commit the patch (+ submodule pin if upgraded) in the parent repo
```

Upstream upgrades: bump the pin → apply → adjust → regen → `make tcc-update-check`.

## Build

```bash
cd third_party/tcc && ./configure --config-cc_ext && make libtcc.a tcc libtcc1.a
```

CC defines: `-DCONFIG_CC_EXT`, `-DCC_TCC_EXT_AVAILABLE`.
