#!/usr/bin/env bash
# Copy the RO-mounted repo into a writable sandbox, then run a command.
# Default command: ./scripts/smoke_ilp32.sh
#
# Mounts (typical):
#   /src   repo (read-only)
#   /work  anonymous volume (writable build sandbox)
#
# Examples:
#   ... ilp32_entrypoint.sh
#   ... ilp32_entrypoint.sh ./real_projects/pigz/compare_ilp32.sh
set -euo pipefail

SRC="${CCC_ILP32_SRC:-/src}"
WORK="${CCC_ILP32_WORK:-/work}"

if [ ! -d "$SRC/scripts" ] || [ ! -f "$SRC/scripts/smoke_ilp32.sh" ]; then
  printf 'ilp32_entrypoint: missing repo at %s (expected RO mount)\n' "$SRC" >&2
  exit 1
fi

mkdir -p "$WORK"

printf '== sync %s -> %s (writable sandbox)\n' "$SRC" "$WORK"
# Exclude host build products so we never carry foreign-arch objects into /work.
# Keep .git so submodule update still works inside the sandbox.
rsync -a \
  --exclude '/out/' \
  --exclude '/bin/' \
  --exclude 'cc/bin/.ccc-bin' \
  --exclude 'cc/bin/ccc.bak' \
  --exclude 'real_projects/pigz/out/' \
  --exclude '.DS_Store' \
  "$SRC"/ "$WORK"/

cd "$WORK"
export CCC_ILP32_ARCH="${CCC_ILP32_ARCH:-i386}"

if [ "$#" -gt 0 ]; then
  exec "$@"
fi
exec ./scripts/smoke_ilp32.sh
