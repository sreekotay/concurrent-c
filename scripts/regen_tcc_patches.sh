#!/bin/sh
set -euo pipefail

# Regenerate the TCC patch capturing all CC extension changes from upstream.
#
# Intended workflow for upgrading TCC:
#   1) Update `third_party/tcc` to the desired upstream commit (clean tree).
#   2) Run `make tcc-patch-apply` to apply our hooks (working tree becomes dirty).
#   3) Adjust hooks if needed.
#   4) Run `make tcc-patch-regen` to capture the new diff into the patch file.
#   5) Run `make tcc-update-check` to rebuild + smoke test.
#
# Note: This script captures all changes from the pinned upstream baseline to the
# current working tree after applying CC hooks.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT_DIR/third_party/tcc-patches"
TCC_DIR="$ROOT_DIR/third_party/tcc"
OUT_PATCH="$PATCH_DIR/0001-cc-ext-hooks.patch"
BASE_REF="${TCC_PATCH_BASE_REF:-}"

cd "$TCC_DIR"

# Prefer the fork's upstream mirror, but keep working in older local checkouts
# where the fork remote may still be named `fork`.
if [ -z "$BASE_REF" ]; then
  for candidate in origin/upstream-mob fork/upstream-mob upstream/mob origin/mob; do
    if git rev-parse --verify "$candidate" >/dev/null 2>&1; then
      BASE_REF="$candidate"
      break
    fi
  done
fi

if [ -z "$BASE_REF" ]; then
  echo "[regen] No upstream base ref found. Set TCC_PATCH_BASE_REF or fetch origin/fork/upstream." >&2
  exit 1
fi

# Check if there are any changes from the upstream mirror.
if git diff --quiet --no-ext-diff "$BASE_REF"; then
  echo "[regen] third_party/tcc has no changes from $BASE_REF; nothing to regenerate." >&2
  exit 1
fi

tmp="$OUT_PATCH.tmp"
# Capture all changes from the upstream mirror to current working tree.
git diff "$BASE_REF" --binary --no-color > "$tmp"

if [ ! -s "$tmp" ]; then
  echo "[regen] Generated patch is empty; refusing to overwrite $OUT_PATCH" >&2
  rm -f "$tmp"
  exit 1
fi

mv "$tmp" "$OUT_PATCH"
echo "[regen] Wrote $(basename "$OUT_PATCH") ($(wc -l < "$OUT_PATCH") lines)"


