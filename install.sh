#!/bin/sh
# Bootstrap, build, and install Concurrent-C.
# - If run from the repo root, use the current checkout.
# - Otherwise, clone into $PWD/concurrent-c (or $CC_REPO_DIR) first.
set -e

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Error: required command not found: $1" >&2
    exit 1
  fi
}

need_cmd git
need_cmd make

BUILD="${BUILD:-release}"
PREFIX="${PREFIX:-/usr/local}"
CC_REPO_URL="${CC_REPO_URL:-https://github.com/sreekotay/concurrent-c.git}"
CC_REPO_DIR="${CC_REPO_DIR:-$PWD/concurrent-c}"

if [ -f "$PWD/Formula/ccc.rb" ] && [ -f "$PWD/Makefile" ] && [ -d "$PWD/cc" ]; then
  ROOT_DIR="$PWD"
  echo "Using existing concurrent-c checkout in $ROOT_DIR"
else
  ROOT_DIR="$CC_REPO_DIR"
  if [ -d "$ROOT_DIR/.git" ]; then
    echo "Using existing concurrent-c checkout in $ROOT_DIR"
  elif [ -e "$ROOT_DIR" ]; then
    echo "Error: target exists and is not a concurrent-c checkout: $ROOT_DIR" >&2
    exit 1
  else
    echo "Cloning concurrent-c into $ROOT_DIR..."
    git clone "$CC_REPO_URL" "$ROOT_DIR"
  fi
fi

cd "$ROOT_DIR"

echo "Initializing required submodules..."
git submodule sync -- third_party/tcc third_party/liblfds >/dev/null 2>&1 || true
git submodule update --init third_party/tcc third_party/liblfds

if [ -f third_party/tcc/cc_ast_record.h ]; then
  echo "TCC patches already applied."
else
  ./scripts/apply_tcc_patches.sh
fi

echo "Configuring TinyCC..."
(
  cd third_party/tcc
  ./configure --config-cc_ext
)

if [ -n "${JOBS:-}" ]; then
  jobs="$JOBS"
else
  jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

echo "Building TinyCC..."
make -C third_party/tcc -j"$jobs"

echo "Building Concurrent-C..."
make cc BUILD="$BUILD"

echo "Installing Concurrent-C to $PREFIX..."
make install BUILD="$BUILD" PREFIX="$PREFIX"

echo "Install complete."
echo "Checkout: $ROOT_DIR"
