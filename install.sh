#!/bin/sh
# Install ccc via Homebrew from this repo.
# Run from repo root (after cloning with submodules, or we will init required ones).
set -e

cd "$(dirname "$0")"

if [ ! -f Formula/ccc.rb ]; then
  echo "Error: run from concurrent-c repo root (no Formula/ccc.rb found)." >&2
  exit 1
fi

if [ ! -d third_party/tcc/.git ] || [ ! -f third_party/tcc/libtcc.c ] || \
   [ ! -d third_party/liblfds/.git ] || [ ! -f third_party/liblfds/liblfds7.1.1/liblfds711/inc/liblfds711.h ]; then
  echo "Initializing required submodules..."
  git submodule update --init third_party/tcc third_party/liblfds
fi

brew install --formula Formula/ccc.rb
