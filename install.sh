#!/bin/sh
# Install ccc via Homebrew from this repo.
# Run from repo root (after cloning with submodules, or we will init tcc).
set -e

cd "$(dirname "$0")"

if [ ! -f Formula/ccc.rb ]; then
  echo "Error: run from concurrent-c repo root (no Formula/ccc.rb found)." >&2
  exit 1
fi

if [ ! -d third_party/tcc/.git ] || [ ! -f third_party/tcc/libtcc.c ]; then
  echo "Initializing third_party/tcc submodule..."
  git submodule update --init third_party/tcc
fi

brew install --formula Formula/ccc.rb
