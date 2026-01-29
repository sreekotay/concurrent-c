#!/bin/bash
# Download original pigz source files from GitHub into pigz_c/

set -e
cd "$(dirname "$0")"

mkdir -p pigz_c
cd pigz_c

BASE_URL="https://raw.githubusercontent.com/madler/pigz/master"

echo "Downloading pigz source files into pigz_c/..."
curl -sLO "$BASE_URL/pigz.c"
curl -sLO "$BASE_URL/yarn.c"
curl -sLO "$BASE_URL/yarn.h"
curl -sLO "$BASE_URL/try.c"
curl -sLO "$BASE_URL/try.h"

echo "Done. Files downloaded:"
ls -la *.c *.h
