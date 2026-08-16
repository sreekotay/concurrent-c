#!/bin/bash
# Fetch pinned curl into curl_c/ (read-only upstream reference).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION_FILE="$SCRIPT_DIR/CURL_VERSION"
DEST_DIR="$SCRIPT_DIR/curl_c"
TMP_DIR="$(mktemp -d)"
ARCHIVE="$TMP_DIR/curl.tar.xz"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

if [ ! -f "$VERSION_FILE" ]; then
    echo "Missing CURL_VERSION"
    exit 1
fi

CURL_VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [ -z "$CURL_VERSION" ]; then
    echo "CURL_VERSION is empty"
    exit 1
fi

# Official release tarball (xz). Mirror: https://curl.se/download.html
CURL_URL="https://curl.se/download/curl-${CURL_VERSION}.tar.xz"

echo "Fetching curl ${CURL_VERSION} from:"
echo "  $CURL_URL"

curl -fL "$CURL_URL" -o "$ARCHIVE"

ROOT_DIR="$(tar -tJf "$ARCHIVE" | awk -F/ 'NR==1 { print $1; exit }')"
if [ -z "$ROOT_DIR" ]; then
    echo "Failed to detect archive root directory"
    exit 1
fi

rm -rf "$DEST_DIR"
tar -xJf "$ARCHIVE" -C "$TMP_DIR"
mv "$TMP_DIR/$ROOT_DIR" "$DEST_DIR"

echo "curl extracted to:"
echo "  $DEST_DIR"
echo
if [ -f "$DEST_DIR/lib/asyn-thrdd.c" ]; then
    echo "Threaded resolver present: lib/asyn-thrdd.c"
else
    echo "WARNING: lib/asyn-thrdd.c not found — pin may be wrong for this port"
fi
echo
echo "Next steps:"
echo "  make upstream    # stock libcurl + curl CLI (threaded resolver)"
echo "  make smoke       # one HTTPS GET through the stock binary"
