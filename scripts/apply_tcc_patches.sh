#!/usr/bin/env bash
set -euo pipefail

# Apply the local TCC hook patches. Run from repo root.
#
# The superproject pins a pristine upstream-mob commit; CC hooks live only in
# third_party/tcc-patches/ (regen via scripts/regen_tcc_patches.sh). After
# apply, the submodule working tree is intentionally dirty (.gitmodules
# ignore=dirty). Reset is REQUIRED when a pull updates a patch file: the tree
# still carries the OLD patch, the new patch's reverse-check fails, and a
# forward apply conflicts (stale hunks / leftover files from prior versions).

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT_DIR/third_party/tcc-patches"
TCC_DIR="$ROOT_DIR/third_party/tcc"

cd "$TCC_DIR"

all_current=1
for p in "$PATCH_DIR"/000*.patch; do
  if ! git apply --reverse --check "$p" >/dev/null 2>&1; then
    all_current=0
    break
  fi
done

if [ "$all_current" = "1" ]; then
  echo "All TCC patches already applied."
  exit 0
fi

# Not (all) current.  If the tree is dirty it holds a stale patch state —
# reset to pristine before applying, otherwise hunks conflict.
if ! git diff --quiet || [ -n "$(git ls-files --others --exclude-standard)" ]; then
  echo "tcc tree carries a stale patch state; resetting to pristine before apply"
  git checkout -- .
  git clean -fdq
fi

for p in "$PATCH_DIR"/000*.patch; do
  echo "Applying $(basename "$p")"
  git apply "$p"
done

echo "All TCC patches applied."
