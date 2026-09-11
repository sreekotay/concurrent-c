#!/usr/bin/env bash
# Fetch raylib into vendor/ — not a git submodule, not committed.
# Pin: RAYLIB_TAG (default 5.5).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RAYLIB_TAG="${RAYLIB_TAG:-5.5}"
DEST="$SCRIPT_DIR/vendor/raylib"

if [[ -f "$DEST/src/raylib.h" ]]; then
    echo "raylib sources present ($DEST)"
    exit 0
fi

mkdir -p vendor
rm -rf "$DEST"
echo "fetching raylib ${RAYLIB_TAG}..."
git clone --depth 1 --branch "$RAYLIB_TAG" \
    https://github.com/raysan5/raylib.git "$DEST"
echo "raylib ${RAYLIB_TAG} at $DEST"
