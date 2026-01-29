#!/bin/bash
# Download original pigz source files from GitHub

set -e
cd "$(dirname "$0")"

BASE_URL="https://raw.githubusercontent.com/madler/pigz/master"

echo "Downloading pigz source files..."
curl -sLO "$BASE_URL/pigz.c"
curl -sLO "$BASE_URL/yarn.c"
curl -sLO "$BASE_URL/yarn.h"
curl -sLO "$BASE_URL/try.c"
curl -sLO "$BASE_URL/try.h"

echo "Done. Files downloaded:"
ls -la *.c *.h
