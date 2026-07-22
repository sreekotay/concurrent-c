#!/usr/bin/env bash
set -euo pipefail

# Regression test for scripts/apply_tcc_patches.sh (#116): the script must
# handle all three tcc-tree states without manual intervention:
#
#   1. current  — patches already applied: no-op ("already applied").
#   2. stale    — the tree carries an OLDER version of the patch set (the
#                 post-pull shape that #116 fixed: reverse-check fails,
#                 forward apply conflicts): auto-reset to pristine + apply.
#   3. pristine — clean tree: plain apply.
#
# The stale state is manufactured from *git history* of the patch file
# (no network): the previous version of 0001-cc-ext-hooks.patch is applied
# to a pristine tree.  If the repo has no older patch version in history
# (shallow clone), a synthetic stale state is used instead: current patch
# applied, then a patched file is perturbed and an untracked conflict file
# added, reproducing the same dirty-and-not-current shape.
#
# The tcc tree is restored to the correct (current-patch) state on exit
# regardless of outcome.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APPLY="$ROOT_DIR/scripts/apply_tcc_patches.sh"
PATCH_DIR="$ROOT_DIR/third_party/tcc-patches"
TCC_DIR="$ROOT_DIR/third_party/tcc"
PATCH_FILE="third_party/tcc-patches/0001-cc-ext-hooks.patch"

fail() { echo "[tcc-patch-apply] FAIL: $*" >&2; exit 1; }

[ -d "$TCC_DIR/.git" ] || [ -f "$TCC_DIR/.git" ] || fail "tcc submodule not initialized"
[ -x "$APPLY" ] || [ -f "$APPLY" ] || fail "apply script missing"

restore() {
  # Whatever happened, put the tree back to pristine + current patches.
  git -C "$TCC_DIR" checkout -- . >/dev/null 2>&1 || true
  git -C "$TCC_DIR" clean -fdq >/dev/null 2>&1 || true
  bash "$APPLY" >/dev/null 2>&1 || {
    echo "[tcc-patch-apply] WARNING: restore apply failed — tcc tree may need scripts/apply_tcc_patches.sh" >&2
  }
}
trap restore EXIT

# Precondition: bring the tree to the known-good state first (the suite may
# run from any state).
git -C "$TCC_DIR" checkout -- .
git -C "$TCC_DIR" clean -fdq
bash "$APPLY" >/dev/null 2>&1 || fail "initial apply on pristine tree failed"

# --- State 1: current -> no-op --------------------------------------------
# (git-apply whitespace warnings on stderr are pre-existing patch-file noise;
# every leg asserts the script's stdout AND the resulting tree state.)
out="$(bash "$APPLY" 2>/dev/null)" || fail "apply on current tree exited nonzero"
case "$out" in
  *"already applied"*) : ;;
  *) fail "apply on current tree was not a no-op (output: $out)" ;;
esac

# --- State 2: stale --------------------------------------------------------
git -C "$TCC_DIR" checkout -- .
git -C "$TCC_DIR" clean -fdq

old_rev="$(git -C "$ROOT_DIR" log --format=%H -n 2 -- "$PATCH_FILE" | sed -n 2p || true)"
if [ -n "$old_rev" ]; then
  # Real stale state: the previous patch version from history.
  git -C "$ROOT_DIR" show "$old_rev:$PATCH_FILE" > "$TCC_DIR/.cc_stale_test.patch" \
    || fail "could not extract old patch version from $old_rev"
  git -C "$TCC_DIR" apply --whitespace=nowarn .cc_stale_test.patch \
    || fail "old patch did not apply to pristine tree"
  rm -f "$TCC_DIR/.cc_stale_test.patch"
  echo "[tcc-patch-apply] stale state: real old patch from ${old_rev:0:12}"
else
  # Shallow-history fallback: synthesize the same dirty-and-not-current shape.
  bash "$APPLY" >/dev/null
  echo "/* stale perturbation */" >> "$TCC_DIR/tccpp.c"
  echo "stale" > "$TCC_DIR/cc_ast_record.h.orig"
  echo "[tcc-patch-apply] stale state: synthetic (no older patch in history)"
fi

# Sanity: this state must NOT be current (reverse-check fails for at least
# one patch) — otherwise the stale leg tests nothing.
stale_is_current=1
for p in "$PATCH_DIR"/000*.patch; do
  git -C "$TCC_DIR" apply --reverse --check "$p" >/dev/null 2>&1 || { stale_is_current=0; break; }
done
[ "$stale_is_current" = "0" ] || fail "manufactured stale state is indistinguishable from current"

out="$(bash "$APPLY" 2>/dev/null)" || fail "apply on STALE tree exited nonzero (the #116 regression)"
case "$out" in
  *"resetting to pristine"*) : ;;
  *) fail "stale tree did not trigger the auto-reset path (output: $out)" ;;
esac
for p in "$PATCH_DIR"/000*.patch; do
  git -C "$TCC_DIR" apply --reverse --check "$p" >/dev/null 2>&1 \
    || fail "after stale-state apply, $(basename "$p") is not cleanly applied"
done

# --- State 3: pristine -----------------------------------------------------
git -C "$TCC_DIR" checkout -- .
git -C "$TCC_DIR" clean -fdq
out="$(bash "$APPLY" 2>/dev/null)" || fail "apply on pristine tree exited nonzero"
case "$out" in
  *"All TCC patches applied."*) : ;;
  *) fail "pristine tree apply did not report success (output: $out)" ;;
esac
for p in "$PATCH_DIR"/000*.patch; do
  git -C "$TCC_DIR" apply --reverse --check "$p" >/dev/null 2>&1 \
    || fail "after pristine apply, $(basename "$p") is not cleanly applied"
done

echo "[tcc-patch-apply] OK (current no-op, stale auto-reset, pristine apply)"
