#!/bin/sh
# Fetch the submodules a compiler build needs, at the smallest size that works.
#
# Two of the five submodules in .gitmodules are build inputs:
#
#   third_party/tcc    patched TinyCC — the parser/compiler foundation. Fully
#                      checked out: the build applies patches and compiles it.
#   third_party/zmij   float-to-string (zmij.c + zmij-c.h). The upstream tree
#                      also carries a large test/benchmark corpus, so it is
#                      fetched sparse.
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

# zmij: check out the C sources only. Sparse-checkout has to be configured
# before the working tree is written when cloning fresh.
echo "Fetching third_party/zmij (sparse: zmij.c, zmij-c.h, LICENSE, README)..."
git submodule sync -- third_party/zmij >/dev/null 2>&1 || true
zmij_sparse_set() {
  mod="$(git rev-parse --git-path modules/third_party/zmij)"
  mkdir -p "$mod/info"
  printf '%s\n' '/zmij.c' '/zmij-c.h' '/LICENSE' '/README.md' > "$mod/info/sparse-checkout"
  git -C third_party/zmij config core.sparseCheckout true
  git -C third_party/zmij sparse-checkout init --no-cone 2>/dev/null || true
  git -C third_party/zmij read-tree -mu HEAD 2>/dev/null || true
}
if [ "$FULL" = "1" ]; then
  submodule_update third_party/zmij
elif [ -d third_party/zmij/.git ] || [ -f third_party/zmij/.git ]; then
  submodule_update third_party/zmij
  zmij_sparse_set
else
  git submodule init third_party/zmij
  zmij_url="$(git config --get submodule.third_party/zmij.url)"
  if [ -n "$zmij_url" ] && \
     git clone --filter=blob:none --sparse --no-checkout "$zmij_url" third_party/zmij 2>/dev/null; then
    git -C third_party/zmij sparse-checkout init --no-cone
    git -C third_party/zmij sparse-checkout set 'zmij.c' 'zmij-c.h' 'LICENSE' 'README.md'
    git submodule update third_party/zmij
  else
    rm -rf third_party/zmij
    submodule_update third_party/zmij
    zmij_sparse_set
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
