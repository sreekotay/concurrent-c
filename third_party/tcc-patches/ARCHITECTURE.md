# TCC Patch Architecture

Concurrent-C extends TCC (Tiny C Compiler) with hooks for parsing CC syntax and emitting stub-AST nodes.

> See also: [HOOKS.md](HOOKS.md) for technical API reference, [README.md](README.md) for quick start.

## Key Principle

**TCC modifications are stored as patch files and applied to a fetchable upstream submodule checkout.**

```
third_party/tcc/           ← Git submodule pinned to upstream TinyCC
    │                        (clean checkout before applying patches)
    │
    │  Parent repo tracks the upstream commit hash
    │
    ▼
third_party/tcc-patches/   ← Patch files as backup/documentation
    └── 0001-cc-ext-hooks.patch
```

The patch files in `third_party/tcc-patches/` carry our CC-specific changes, while the parent repo pins the upstream TinyCC commit they apply to.

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

This checks out the TCC submodule at the pinned upstream commit.

### Making Changes to TCC

1. Edit files directly in `third_party/tcc/`
2. Test your changes
3. Regenerate the patch:
   ```bash
   make tcc-patch-regen
   ```
4. Commit the updated patch file (and submodule pointer if you also advanced upstream).

### Upgrading TCC Upstream

1. Fetch and check out the new upstream commit:
   ```bash
   cd third_party/tcc
   git fetch origin
   git checkout origin/mob
   ```
2. Apply the patch: `make tcc-patch-apply`
3. Fix any conflicts
4. Regenerate patch: `make tcc-patch-regen`
5. Update parent repo: `git add third_party/tcc third_party/tcc-patches/0001-cc-ext-hooks.patch && git commit`
6. Verify: `make tcc-update-check`

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

1. **DON'T commit CC-only changes inside upstream TCC**
   - Keep `third_party/tcc/` on a fetchable upstream commit
   - Store CC changes in `third_party/tcc-patches/`

2. **DON'T run `git submodule update` without `--init`**
   - Use `git submodule update --init` to preserve our tracked commit

3. **DON'T push to upstream TCC remote**
   - Our commits are local-only; push only to our parent repo

## Checking Submodule State

```bash
# Verify TCC is on the pinned upstream commit:
cd third_party/tcc
git log --oneline -3           # Should show upstream TinyCC commits
git remote -v                  # origin should be upstream TCC
```
After that, `make tcc-patch-apply` should apply the CC hooks cleanly.
