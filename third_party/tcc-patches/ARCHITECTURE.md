# TCC Patch Architecture

Concurrent-C extends TCC (Tiny C Compiler) with hooks for parsing CC syntax and emitting stub-AST nodes.

> See also: [HOOKS.md](HOOKS.md) for technical API reference, [README.md](README.md) for quick start.

## Key Principle

**TCC modifications live in a fetchable forked submodule history, and the patch file snapshots that delta against a mirrored upstream base.**

```
third_party/tcc/           ← Git submodule pinned to sreekotay/tinycc
    │                        (`mob` carries the CC hook commits)
    │
    │  Parent repo tracks a fetchable fork commit hash
    │
    ▼
third_party/tcc-patches/   ← Patch files as backup/documentation
    └── 0001-cc-ext-hooks.patch
```

The fork also publishes `upstream-mob`, which mirrors TinyCC upstream and is used as the base ref for patch regeneration.

## Files Modified by Patches

| File | Purpose |
|------|---------|
| `Makefile` | Adds `-DCONFIG_CC_EXT` to CFLAGS when `CONFIG_cc_ext=yes` |
| `tcc.h` | CC-specific fields in `TCCState`, AST node types, new tokens |
| `cc_ast_record.h` | AST recording helpers and node metadata capture |
| `libtcc.c` | `cc_tcc_parse_to_ast()` API, include path setup |
| `tccgen.c` | Statement/expression parsing extensions (closures, `@defer`, retired-syntax rejection hooks, etc.) |
| `tccpp.c` | `=>` arrow token lexing |

## Workflow

### After Fresh Clone

```bash
git submodule update --init --recursive
```

This checks out the TCC submodule at the pinned fork commit.

### Making Changes to TCC

1. Edit files directly in `third_party/tcc/`
2. Test your changes
3. Confirm the push target before committing:
   - Canonical fork remote: `origin` -> `https://github.com/sreekotay/tinycc.git`
   - Canonical CC branch: `mob`
   - The submodule is usually on detached HEAD because the parent repo pins a commit. That is expected. In that state, push with `git push origin HEAD:mob`, or first check out a local branch that tracks `origin/mob`.
4. Regenerate the patch:
   ```bash
   make tcc-patch-regen
   ```
-5. Push the submodule branch to the fork:
   ```bash
   cd third_party/tcc
   git push origin HEAD:mob
   ```
6. Commit the updated patch file and submodule pointer in the parent repo.

### Upgrading TCC Upstream

1. Fetch the fork refs and rebase the CC branch onto the mirrored upstream base:
   ```bash
   cd third_party/tcc
   git fetch origin
   git checkout mob
   git rebase origin/upstream-mob
   ```
2. Fix any conflicts
3. Regenerate patch: `make tcc-patch-regen`
4. Push the rebased branch to the fork: `git push origin mob`
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

1. **DON'T leave the parent repo pointing at an unpushable submodule commit**
   - Keep `third_party/tcc/` on a commit reachable from `https://github.com/sreekotay/tinycc.git`
   - Push CC hook commits to `origin/mob` before committing the parent repo's submodule pointer
   - Regenerate `third_party/tcc-patches/0001-cc-ext-hooks.patch` after rebases

2. **DON'T run `git submodule update` without `--init`**
   - Use `git submodule update --init` to preserve our tracked commit

3. **DON'T regenerate patches against the wrong base ref**
   - `make tcc-patch-regen` diffs against `origin/upstream-mob`
   - That fork branch should stay aligned with TinyCC upstream

## Checking Submodule State

```bash
# Verify TCC is on the pinned fork commit:
cd third_party/tcc
git log --oneline -3           # Should show the CC hook commits on top
git branch -r --contains HEAD  # Should include origin/mob or fork/mob
```
The patch snapshot should match the forked submodule history:

```bash
make tcc-patch-regen
git diff --exit-code third_party/tcc-patches/0001-cc-ext-hooks.patch
```
