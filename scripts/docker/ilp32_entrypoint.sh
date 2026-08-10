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
# Arch boundary: /out is the sandbox's product tree (see docs/ilp32-docker.md).
# Host third_party/tcc often has Darwin/arm64 config.mak + libtcc1.a; syncing
# those onto a named ILP32 volume silently breaks comptime (missing __aeabi_*).
# Legacy in-tree tools/cc_test (Mach-O) must not overwrite an ELF harness.
# --delete drops dest files removed from the host (stale tests/ on a named
# volume). Excluded paths are kept on dest (not --delete-excluded).
rsync -a --delete \
  --exclude '/out/' \
  --exclude '/bin/' \
  --exclude 'cc/bin/.ccc-bin' \
  --exclude 'cc/bin/ccc.bak' \
  --exclude '/tools/cc_test' \
  --exclude '/tools/cc_test.dSYM' \
  --exclude 'real_projects/pigz/out/' \
  --exclude '/third_party/tcc/config.mak' \
  --exclude '/third_party/tcc/config.h' \
  --exclude '/third_party/tcc/config.texi' \
  --exclude '/third_party/tcc/*.o' \
  --exclude '/third_party/tcc/*.a' \
  --exclude '/third_party/tcc/tcc' \
  --exclude '/third_party/tcc/*-tcc' \
  --exclude '/third_party/tcc/lib/*.o' \
  --exclude '/third_party/tcc/lib/*.a' \
  --exclude '/arm32_smoke_progress/' \
  --exclude '/arm32_smoke_stems.txt' \
  --exclude '/arm32_smoke_batch.log' \
  --exclude '.DS_Store' \
  "$SRC"/ "$WORK"/

cd "$WORK"
export CCC_ILP32_ARCH="${CCC_ILP32_ARCH:-i386}"

if [ "$#" -gt 0 ]; then
  exec "$@"
fi
exec ./scripts/smoke_ilp32.sh
