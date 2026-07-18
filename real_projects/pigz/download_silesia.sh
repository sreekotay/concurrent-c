#!/usr/bin/env bash
# Download and extract the Silesia compression corpus for pigz benchmarks.
#
# Usage:
#   ./download_silesia.sh              # -> testdata/silesia/
#   ./download_silesia.sh /path/dir  # -> /path/dir/silesia/
#
# Source from other scripts:
#   source "$(dirname "$0")/download_silesia.sh"
#   download_silesia "$DATA_DIR"
#
# Corpus: http://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip (~65 MB)

set -euo pipefail

SILESIA_URL="${SILESIA_URL:-http://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip}"

download_silesia() {
    local data_dir="${1:?data directory required}"
    local corpus_dir="$data_dir/silesia"

    if [ -d "$corpus_dir" ] && [ -n "$(ls -A "$corpus_dir" 2>/dev/null)" ]; then
        return 0
    fi

    mkdir -p "$data_dir"
    cd "$data_dir"

    echo "Downloading Silesia corpus from $SILESIA_URL ..."
    rm -rf silesia silesia.zip
    mkdir -p silesia

    if command -v curl >/dev/null 2>&1; then
        curl -L --fail -o silesia.zip "$SILESIA_URL"
    elif command -v wget >/dev/null 2>&1; then
        wget -O silesia.zip "$SILESIA_URL"
    else
        echo "Error: need curl or wget to download silesia.zip" >&2
        exit 1
    fi

    echo "Extracting silesia.zip ..."
    if command -v unzip >/dev/null 2>&1; then
        unzip -q -o silesia.zip -d silesia
    elif command -v python3 >/dev/null 2>&1; then
        python3 - <<'PY'
import zipfile, os
os.makedirs("silesia", exist_ok=True)
with zipfile.ZipFile("silesia.zip") as z:
    z.extractall("silesia")
PY
    else
        echo "Error: need unzip or python3 to extract silesia.zip" >&2
        exit 1
    fi

    # Some zips contain a nested directory; normalize to silesia/*files*
    if [ -d "silesia/silesia" ] && [ -n "$(ls -A silesia/silesia 2>/dev/null)" ]; then
        rm -rf silesia.tmp
        mv silesia/silesia silesia.tmp
        rm -rf silesia
        mv silesia.tmp silesia
    fi

    if [ ! -d "silesia" ] || [ -z "$(ls -A silesia 2>/dev/null)" ]; then
        echo "Error: extraction failed (silesia directory is empty)" >&2
        exit 1
    fi

    echo "Silesia corpus ready: $corpus_dir ($(find silesia -type f | wc -l | tr -d ' ') files)"
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    download_silesia "${1:-$SCRIPT_DIR/testdata}"
fi
