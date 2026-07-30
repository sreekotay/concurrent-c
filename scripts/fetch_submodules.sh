#!/bin/sh
# Fetch the submodules a compiler build needs, at the smallest size that works.
#
# Two of the five submodules in .gitmodules are build inputs:
#
#   third_party/tcc   patched TinyCC — the parser/compiler foundation. Fully
#                     checked out: the build applies patches and compiles it.
#   third_party/xjb   float-to-string algorithm. One file is used
#                     (src/ftoa.cpp) out of a 37M tree that is mostly
#                     benchmark corpora, so it is fetched sparse.
#
# liblfds is optional at every level: cc/runtime/channel.c probes for it with
# __has_include and the native ring queue is the primary lock-free path either
# way. It is skipped by default because its only host is liblfds.org, which has
# no mirror — a fetch failure there used to abort the whole install. Pass
# --with-liblfds to opt in.
#
# bearssl and curl back the opt-in TLS/HTTP modules; `make deps` fetches those.
set -e

usage() {
  cat <<'EOF'
Usage: scripts/fetch_submodules.sh [--with-liblfds] [--full]

Options:
  --with-liblfds  Also fetch third_party/liblfds (optional channel backend).
  --full          Fetch complete submodule trees; skip the sparse/partial
                  optimizations. Use when you need to work in a submodule.
  -h, --help      Show this help.
EOF
}

WITH_LIBLFDS=0
FULL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --with-liblfds) WITH_LIBLFDS=1 ;;
    --full) FULL=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; echo "Error: unknown argument: $1" >&2; exit 1 ;;
  esac
  shift
done

if [ ! -f .gitmodules ] || [ ! -d cc ]; then
  echo "Error: run this from the concurrent-c repo root." >&2
  exit 1
fi

# Partial clone needs a server that advertises the filter; fall back quietly.
submodule_update() {
  path="$1"
  shift
  if [ "$FULL" = "1" ]; then
    git submodule update --init "$path"
  elif ! git submodule update --init --filter=blob:none "$path" 2>/dev/null; then
    git submodule update --init "$path"
  fi
}

echo "Fetching third_party/tcc..."
git submodule sync -- third_party/tcc >/dev/null 2>&1 || true
submodule_update third_party/tcc

# xjb: check out src/ only. Sparse-checkout has to be configured before the
# working tree is written, so the clone and the pinned-commit checkout are
# separate steps here.
echo "Fetching third_party/xjb (sparse: src/ only)..."
git submodule sync -- third_party/xjb >/dev/null 2>&1 || true
if [ "$FULL" = "1" ] || [ -d third_party/xjb/.git ] || [ -f third_party/xjb/.git ]; then
  submodule_update third_party/xjb
else
  git submodule init third_party/xjb
  xjb_url="$(git config --get submodule.third_party/xjb.url)"
  if [ -n "$xjb_url" ] && \
     git clone --filter=blob:none --sparse --no-checkout "$xjb_url" third_party/xjb 2>/dev/null; then
    git -C third_party/xjb sparse-checkout set src
    git submodule update third_party/xjb
  else
    rm -rf third_party/xjb
    submodule_update third_party/xjb
  fi
fi

if [ "$WITH_LIBLFDS" = "1" ]; then
  echo "Fetching third_party/liblfds..."
  git submodule sync -- third_party/liblfds >/dev/null 2>&1 || true
  if ! submodule_update third_party/liblfds; then
    echo "Warning: could not fetch third_party/liblfds; continuing without it." >&2
    echo "         Channels use the native ring queue instead." >&2
  fi
fi

echo "Submodules ready."
