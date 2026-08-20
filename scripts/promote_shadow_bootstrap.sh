#!/usr/bin/env bash
# Promote cc/bootstrap/shadow_lower/latest/ → MAJOR.MINOR.PATCH-N/ and
# point last-good at it. Does not commit — print the paths to add.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/cc/bootstrap/shadow_lower"
LATEST="$BOOT/latest"
LAST_GOOD="$BOOT/last-good"

if [[ ! -f "$LATEST/shadow_lower.c" ]] || [[ ! -d "$LATEST/include" ]]; then
  echo "error: missing $LATEST (run scripts/snapshot_shadow_lower.sh first)" >&2
  exit 1
fi

# Portable bootstrap requires flattened includes — reject un-rewritten angles.
if grep -R -n -E '#include[[:space:]]*<(cc/shadow|examples/serdes/c)/' "$LATEST" >/dev/null 2>&1; then
  echo "error: $LATEST still has <cc/shadow/...> (or legacy <examples/serdes/c/...>) includes" >&2
  echo "  re-run: ./scripts/snapshot_shadow_lower.sh  (path rewrite must flatten them)" >&2
  exit 1
fi

VERSION_BASE="$(sed -n 's/^CCC_VERSION_BASE ?= //p' "$ROOT/cc/Makefile" | head -1 | tr -d '[:space:]')"
if [[ -z "$VERSION_BASE" ]]; then
  VERSION_BASE="0.3.4"
fi

if [[ $# -gt 1 ]]; then
  echo "usage: $0 [N]" >&2
  exit 2
fi

if [[ $# -eq 1 ]]; then
  if [[ ! "$1" =~ ^[0-9]+$ ]]; then
    echo "error: version must be a non-negative integer (got: $1)" >&2
    exit 2
  fi
  N="$1"
else
  max=0
  for d in "$BOOT"/[0-9]*.[0-9]*.[0-9]*-[0-9]*; do
    [[ -d "$d" ]] || continue
    n="${d##*-}"
    if [[ "$n" =~ ^[0-9]+$ ]] && (( n > max )); then
      max=$n
    fi
  done
  N=$((max + 1))
fi

DEST="$BOOT/${VERSION_BASE}-$N"
if [[ -e "$DEST" ]]; then
  echo "error: $DEST already exists (pick another N or remove it)" >&2
  exit 1
fi

echo "[promote] $LATEST → $DEST"
mkdir -p "$DEST/include"
cp -f "$LATEST/shadow_lower.c" "$DEST/shadow_lower.c"
cp -f "$LATEST"/include/*.h "$DEST/include/"
[[ -f "$LATEST/SNAPSHOT.txt" ]] && cp -f "$LATEST/SNAPSHOT.txt" "$DEST/SNAPSHOT.txt"
printf '%s-%d\n' "$VERSION_BASE" "$N" > "$LAST_GOOD"

{
  echo "promoted_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_head: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  if [[ -f "$DEST/SNAPSHOT.txt" ]]; then
    echo "--- from SNAPSHOT.txt ---"
    cat "$DEST/SNAPSHOT.txt"
  fi
} > "$DEST/PROMOTE.txt"

echo "[promote] last-good → ${VERSION_BASE}-$N"
echo "[promote] commit when ready:"
echo "  git add $BOOT/last-good $DEST"
echo "  git status -- $BOOT"
