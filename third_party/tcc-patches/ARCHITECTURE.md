# TCC Patch Architecture

Concurrent-C extends TCC (Tiny C Compiler) with hooks for parsing CC syntax and emitting stub-AST nodes.

> See also: [HOOKS.md](HOOKS.md) for technical API reference, [README.md](README.md) for quick start.

## Key Principle

**TCC modifications are committed locally and tracked by the parent repo's submodule pointer.**

```
third_party/tcc/           ← Git submodule with LOCAL commits
    │                        (our changes committed on top of upstream)
    │
    │  Parent repo tracks our commit hash
    │
    ▼
third_party/tcc-patches/   ← Patch files as backup/documentation
    └── 0001-cc-ext-hooks.patch
```

The submodule commit hash in the parent repo ensures our changes are never lost.

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

### After Fresh Clone

```bash
git submodule update --init --recursive
```

This checks out the TCC submodule at our tracked commit (with CC changes included).

### Making Changes to TCC

1. Edit files directly in `third_party/tcc/`
2. Test your changes
3. Commit inside the submodule:
   ```bash
   cd third_party/tcc
   git add -A && git commit -m "Description of change"
   ```
4. Update parent repo to track new commit:
   ```bash
   cd ../..
   git add third_party/tcc && git commit -m "Update TCC with: description"
   ```
5. Optionally regenerate patch for documentation:
   ```bash
   make tcc-patch-regen
   ```

### Upgrading TCC Upstream

1. Fetch and rebase our changes onto new upstream:
   ```bash
   cd third_party/tcc
   git fetch origin
   git rebase origin/mob
   ```
2. Fix any conflicts
3. Update parent repo: `git add third_party/tcc && git commit`
4. Regenerate patch: `make tcc-patch-regen`
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

1. **DON'T forget to commit in BOTH places**
   - Commit inside `third_party/tcc/` first
   - Then commit the submodule update in the parent repo

2. **DON'T run `git submodule update` without `--init`**
   - Use `git submodule update --init` to preserve our tracked commit

3. **DON'T push to upstream TCC remote**
   - Our commits are local-only; push only to our parent repo

## Checking Submodule State

```bash
# Verify TCC has our local commits:
cd third_party/tcc
git log --oneline -3           # Should show our CC commit(s) on top
git remote -v                  # origin should be upstream TCC
```

The parent repo's submodule pointer tracks our commit hash, ensuring changes are preserved.
