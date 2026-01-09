#!/bin/sh
set -euo pipefail

# Regenerate the TCC patch from the current working tree diff inside the submodule.
#
# Intended workflow for upgrading TCC:
#   1) Update `third_party/tcc` to the desired upstream commit (clean tree).
#   2) Run `make tcc-patch-apply` to apply our hooks (working tree becomes dirty).
#   3) Adjust hooks if needed.
#   4) Run `make tcc-patch-regen` to capture the new diff into the patch file.
#   5) Run `make tcc-update-check` to rebuild + smoke test.
#
# Note: This script regenerates patches from *uncommitted* diffs (HEAD -> working tree).
# If you committed changes inside `third_party/tcc`, this will produce an empty patch.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT_DIR/third_party/tcc-patches"
TCC_DIR="$ROOT_DIR/third_party/tcc"
OUT_PATCH="$PATCH_DIR/0001-cc-ext-hooks.patch"

cd "$TCC_DIR"

if git diff --quiet --no-ext-diff && git diff --quiet --no-ext-diff --cached; then
  echo "[regen] third_party/tcc has no local modifications; nothing to regenerate." >&2
  echo "[regen] Tip: keep CC hook changes as an uncommitted diff on top of the upstream TCC commit." >&2
  exit 1
fi

tmp="$OUT_PATCH.tmp"
git diff --binary --no-color > "$tmp"

if [ ! -s "$tmp" ]; then
  echo "[regen] Generated patch is empty; refusing to overwrite $OUT_PATCH" >&2
  rm -f "$tmp"
  exit 1
fi

mv "$tmp" "$OUT_PATCH"
echo "[regen] Wrote $(basename "$OUT_PATCH") ($(wc -l < "$OUT_PATCH") lines)"


