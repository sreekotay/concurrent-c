# TCC Patch Architecture

## Overview

Concurrent-C extends TCC (Tiny C Compiler) with hooks for parsing CC syntax and emitting stub-AST nodes. These extensions are maintained as **patches** applied on top of a pristine TCC upstream.

## Key Principle

**TCC modifications live as uncommitted changes, NOT as commits.**

```
third_party/tcc/           ← Git submodule at pristine upstream commit
    │                        (branch: mob, tracking origin/mob)
    │
    │  Patches applied as UNCOMMITTED working tree changes
    │
    ▼
third_party/tcc-patches/   ← Patch files capturing those diffs
    └── 0001-cc-ext-hooks.patch
```

## Files Modified by Patches

| File | Purpose |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` to CFLAGS when `CONFIG_cc_ext=yes` |
| `tcc.h` | CC-specific fields in `TCCState`, AST node types, new tokens |
| `tcc.c` | CC output type flag, token registrar |
| `libtcc.c` | `cc_tcc_parse_to_ast()` API, include path setup |
| `tccgen.c` | Statement/expression parsing extensions (closures, @arena, etc.) |
| `tccpp.c` | `=>` arrow token lexing |

## Workflow

### Applying Patches (after clone or TCC update)

```bash
make tcc-patch-apply
# or: ./scripts/apply_tcc_patches.sh
```

This applies `0001-cc-ext-hooks.patch` to the TCC working tree.

### Making Changes to TCC

1. Edit files directly in `third_party/tcc/`
2. Test your changes
3. Regenerate the patch:
   ```bash
   make tcc-patch-regen
   # or: ./scripts/regen_tcc_patches.sh
   ```
4. Commit the updated patch file (NOT the TCC changes)

### Upgrading TCC Upstream

1. Update submodule to new upstream commit:
   ```bash
   cd third_party/tcc
   git fetch origin
   git checkout <new-commit>
   ```
2. Apply patches: `make tcc-patch-apply`
3. Fix any conflicts
4. Regenerate: `make tcc-patch-regen`
5. Verify: `make tcc-update-check`

## Build Configuration

### TCC Build

TCC's `configure` creates `config.mak` with `CONFIG_cc_ext=yes`. The Makefile translates this to `-DCONFIG_CC_EXT` in CFLAGS.

```bash
cd third_party/tcc
./configure --config-cc_ext
make
```

### CC Compiler Build

The CC compiler (`cc/`) links against `libtcc.a` and needs these defines:
- `-DCONFIG_CC_EXT` — see TCC extension fields in `tcc.h`
- `-DCC_TCC_EXT_AVAILABLE` — CC's own flag for conditional code

## Common Mistakes to Avoid

1. **DON'T commit changes inside `third_party/tcc/`**
   - Changes should remain as uncommitted diffs
   - Only commit the patch file

2. **DON'T forget to regenerate patches after editing TCC**
   - Run `make tcc-patch-regen` before committing

3. **DON'T manually edit the patch file**
   - Always regenerate from the working tree diff

4. **DO verify submodule state**
   ```bash
   cd third_party/tcc
   git status  # Should show modified files (the patches)
   git log -1  # Should be at a pristine upstream commit
   ```

## Checking Patch State

```bash
# Verify TCC is at upstream with uncommitted CC patches:
cd third_party/tcc
git log --oneline -1           # Should be upstream commit
git diff --stat                # Should show CC patch changes
```

If you see local commits ahead of upstream, the state is wrong. Reset and re-apply:
```bash
git reset --hard origin/mob
git apply ../tcc-patches/0001-cc-ext-hooks.patch
```
