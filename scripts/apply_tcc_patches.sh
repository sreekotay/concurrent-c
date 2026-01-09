#!/bin/sh
set -euo pipefail

# Apply the local TCC hook patches. Run from repo root.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="$ROOT_DIR/third_party/tcc-patches"
TCC_DIR="$ROOT_DIR/third_party/tcc"

cd "$TCC_DIR"

for p in "$PATCH_DIR"/000*.patch; do
  echo "Applying $(basename "$p")"
  # Idempotent apply: skip if already applied.
  if git apply --reverse --check "$p" >/dev/null 2>&1; then
    echo "  (already applied)"
  else
    git apply "$p"
  fi
done

echo "All TCC patches applied."

