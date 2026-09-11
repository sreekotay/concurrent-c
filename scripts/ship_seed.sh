#!/usr/bin/env bash
# Freeze the lowerer's tools as a bootstrap seed.
#
#   ./scripts/ship_seed.sh              # snapshot → cc/bootstrap/lowerer/latest/
#   ./scripts/ship_seed.sh --promote    # …then promote to <base>-N, flip last-good
#
# A seed is the lowered C of cclex, ccparse, cclower and ccindex, the C of
# module `lower` they share, and the lowered faces that C includes. Stage
# zero of `make -C cc` host-compiles it with no lowerer in hand;
# `make -C cc lower-cc` then rebuilds the tools from their sources with the
# seeded ones, and scripts/lowerer_selfhost.sh says whether the two agree.
# Source of truth is cc/lower/*.cch and *.ccs; a pin is regenerate-only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/cc/bootstrap/lowerer"
LATEST="$BOOT/latest"
CCC="$ROOT/cc/bin/ccc"
# The lowerer's own root: the module product and faces it lowered, which
# the driver compiles a unit against.
HDR_SRC="$ROOT/out/.cc-build/clean/cc/lower"
TOOLS="cclex ccparse cclower ccindex"
PROMOTE=0
N=""
for arg in "$@"; do
  case "$arg" in
    --promote) PROMOTE=1 ;;
    [0-9]*) N="$arg" ;;
    -h|--help) sed -n 2,12p "$0"; exit 0 ;;
    *) echo "error: unknown arg: $arg" >&2; exit 2 ;;
  esac
done
[[ -x "$CCC" ]] || { echo "error: no $CCC (make -C cc)" >&2; exit 1; }
[[ -x "$ROOT/out/cc/bin/cclower_cc" ]] || { echo "error: no out/cc/bin/cclower_cc (make -C cc lower-cc)" >&2; exit 1; }
ts() { echo "[seed] $(date '+%H:%M:%S') $*"; }
rm -rf "$LATEST"
mkdir -p "$LATEST"
t0=$(date +%s)
for t in $TOOLS; do
  ts "emit $t"
  "$CCC" --no-cache --emit-c-only "$ROOT/cc/lower/$t.ccs" -o "$LATEST/$t.c"
  [[ -s "$LATEST/$t.c" ]] || { echo "error: empty emit for $t" >&2; exit 1; }
done
[[ -s "$HDR_SRC/lower_cch.c" ]] || { echo "error: no module product $HDR_SRC/lower_cch.c" >&2; exit 1; }
cp -f "$HDR_SRC/lower_cch.c" "$LATEST/lower_cch.c"
cp -f "$HDR_SRC"/*.h "$LATEST/"
ts "rewrite include and #line paths"
python3 - "$ROOT" "$LATEST" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1]).resolve()
latest = pathlib.Path(sys.argv[2]).resolve()
root_s = str(root)
inc_abs = re.compile(r'#include\s+"[^"]*/out/include/cc/lower/([^"/]+)"')
inc_angle = re.compile(r'#include\s+<cc/lower/([^>/]+)>')
line_abs = re.compile(r'(#line\s+\d+\s+)"' + re.escape(root_s) + r'/([^"]+)"')
n = 0
for p in sorted(latest.glob("*.c")) + sorted(latest.glob("*.h")):
    text = p.read_text()
    new = inc_abs.sub(r'#include "\1"', text)
    new = inc_angle.sub(r'#include "\1"', new)
    new = line_abs.sub(r'\1"\2"', new)
    if new != text:
        p.write_text(new)
        n += 1
print(f"[seed] rewrote {n} files")
left = []
for p in sorted(latest.glob("*.c")) + sorted(latest.glob("*.h")):
    for m in re.finditer(r'#include\s+"([^"]*/[^"]*)"', p.read_text()):
        left.append(f"{p.name}: {m.group(0)}")
if left:
    print("error: absolute or nested quoted includes remain:", file=sys.stderr)
    for l in left[:10]: print("  " + l, file=sys.stderr)
    sys.exit(1)
PY
{
  echo "git: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '?')"
  echo "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "tools: $TOOLS"
  echo "cclower_cc: $(cksum "$ROOT/out/cc/bin/cclower_cc" | cut -d' ' -f1)"
} > "$LATEST/SNAPSHOT.txt"
ts "snapshot done ($(( $(date +%s) - t0 ))s, $(du -sh "$LATEST" | cut -f1))"
if [[ $PROMOTE -eq 0 ]]; then
  echo "  promote: $0 --promote"
  exit 0
fi
VERSION_BASE="$(sed -n 's/^CCC_VERSION_BASE ?= //p' "$ROOT/cc/Makefile" | head -1 | tr -d '[:space:]')"
[[ -n "$VERSION_BASE" ]] || VERSION_BASE="0.3.4"
if [[ -z "$N" ]]; then
  max=0
  for d in "$BOOT"/[0-9]*.[0-9]*.[0-9]*-[0-9]*; do
    [[ -d "$d" ]] || continue
    n="${d##*-}"
    if [[ "$n" =~ ^[0-9]+$ ]] && (( n > max )); then max=$n; fi
  done
  N=$((max + 1))
fi
DEST="$BOOT/${VERSION_BASE}-$N"
[[ -e "$DEST" ]] && { echo "error: $DEST exists" >&2; exit 1; }
mkdir -p "$DEST"
cp -f "$LATEST"/*.c "$LATEST"/*.h "$LATEST/SNAPSHOT.txt" "$DEST/"
printf '%s\n' "${VERSION_BASE}-$N" > "$BOOT/last-good"
ts "promoted ${VERSION_BASE}-$N; last-good updated"
echo "  git add cc/bootstrap/lowerer/last-good cc/bootstrap/lowerer/${VERSION_BASE}-$N"
