#!/bin/sh
# Fetch the submodules a compiler build needs, at the smallest size that works.
#
# The build-required submodule:
#
#   third_party/tcc   patched TinyCC — the parser/compiler foundation. Fully
#                     checked out: the build applies patches and compiles it.
#
# Float formatting (Żmij) is vendored under cc/runtime/vendor/ — no submodule.
#
# liblfds is optional at every level: cc/runtime/channel.c probes for it with
# __has_include and the native ring queue is the primary lock-free path either
# way. It is skipped by default because its only host is liblfds.org, which has
# no mirror — a fetch failure there used to abort the whole install. Pass
# --with-liblfds to opt in.
#
# bearssl and curl back the opt-in TLS/HTTP modules. `make bearssl` /
# `make deps` fetch BearSSL when the tree is missing; pass --with-bearssl
# here to fetch without building. curl is still `git submodule update`.
set -e

usage() {
  cat <<'EOF'
Usage: scripts/fetch_submodules.sh [--with-liblfds] [--with-bearssl] [--full]

Options:
  --with-liblfds  Also fetch third_party/liblfds (optional channel backend).
  --with-bearssl  Also fetch third_party/bearssl (TLS; `make bearssl` does this).
  --full          Fetch complete submodule trees; skip partial-clone filters.
                  Use when you need to work in a submodule.
  -h, --help      Show this help.
EOF
}

WITH_LIBLFDS=0
WITH_BEARSSL=0
FULL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --with-liblfds) WITH_LIBLFDS=1 ;;
    --with-bearssl) WITH_BEARSSL=1 ;;
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

if [ "$WITH_LIBLFDS" = "1" ]; then
  echo "Fetching third_party/liblfds..."
  git submodule sync -- third_party/liblfds >/dev/null 2>&1 || true
  if ! submodule_update third_party/liblfds; then
    echo "Warning: could not fetch third_party/liblfds; continuing without it." >&2
    echo "         Channels use the native ring queue instead." >&2
  fi
fi

if [ "$WITH_BEARSSL" = "1" ]; then
  echo "Fetching third_party/bearssl..."
  git submodule sync -- third_party/bearssl >/dev/null 2>&1 || true
  submodule_update third_party/bearssl
fi

echo "Submodules ready."
