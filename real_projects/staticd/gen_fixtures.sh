#!/usr/bin/env bash
# Generate frozen fixture files + fixtures/manifest.txt (path sha256 size).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIX="$SCRIPT_DIR/fixtures"
mkdir -p "$FIX"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

write_sized() {
    local path="$1" size="$2" pattern="$3"
    # Deterministic content (not /dev/urandom) so hashes are stable across machines.
    python3 - "$path" "$size" "$pattern" <<'PY'
import sys
path, size, pattern = sys.argv[1], int(sys.argv[2]), sys.argv[3].encode()
buf = (pattern * ((size // len(pattern)) + 1))[:size]
open(path, "wb").write(buf)
PY
}

write_sized "$FIX/1kb.bin" 1024 "STATICD-1KB-PAD."
write_sized "$FIX/4kb.html" 4096 "<!doctype html><!--staticd-->"
write_sized "$FIX/64kb.js" 65536 "/*staticd-js*/var x="
write_sized "$FIX/1mb.bin" $((1024 * 1024)) "STATICD-1MB-BLOCK."
write_sized "$FIX/10mb.bin" $((10 * 1024 * 1024)) "STATICD-10MB-BLOCK."
printf '%s\n' '<!doctype html><title>staticd</title><h1>ok</h1>' > "$FIX/index.html"

{
    echo "# path sha256 size"
    for f in 1kb.bin 4kb.html 64kb.js 1mb.bin 10mb.bin index.html; do
        p="$FIX/$f"
        echo "/$f $(sha256_file "$p") $(wc -c < "$p" | tr -d ' ')"
    done
} > "$FIX/manifest.txt"

echo "fixtures ready:"
cat "$FIX/manifest.txt"
