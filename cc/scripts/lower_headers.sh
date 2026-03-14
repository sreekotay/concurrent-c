#!/bin/sh
# lower_headers.sh - compatibility wrapper for the real header lowerer

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$CC_DIR/../out/cc/bin/lower_headers"

if [ ! -x "$BINARY" ]; then
    make -C "$CC_DIR" "$BINARY"
fi

exec "$BINARY" "$@"
