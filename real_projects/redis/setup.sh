#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION_FILE="$SCRIPT_DIR/REDIS_VERSION"
DEST_DIR="$SCRIPT_DIR/redis_c"
TMP_DIR="$(mktemp -d)"
ARCHIVE="$TMP_DIR/redis.tar.gz"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

if [ ! -f "$VERSION_FILE" ]; then
    echo "Missing REDIS_VERSION"
    exit 1
fi

REDIS_VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [ -z "$REDIS_VERSION" ]; then
    echo "REDIS_VERSION is empty"
    exit 1
fi

if [ "$REDIS_VERSION" = "stable" ]; then
    REDIS_URL="https://download.redis.io/redis-stable.tar.gz"
else
    REDIS_URL="https://download.redis.io/releases/redis-${REDIS_VERSION}.tar.gz"
fi

echo "Fetching Redis from:"
echo "  $REDIS_URL"

curl -L "$REDIS_URL" -o "$ARCHIVE"

ROOT_DIR="$(tar -tzf "$ARCHIVE" | awk -F/ 'NR==1 { print $1; exit }')"
if [ -z "$ROOT_DIR" ]; then
    echo "Failed to detect archive root directory"
    exit 1
fi

rm -rf "$DEST_DIR"
tar -xzf "$ARCHIVE" -C "$TMP_DIR"
mv "$TMP_DIR/$ROOT_DIR" "$DEST_DIR"

echo "Redis extracted to:"
echo "  $DEST_DIR"
echo
echo "Next steps:"
echo "  make upstream"
echo "  make redis_idiomatic redis_cc"
