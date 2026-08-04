#!/usr/bin/env bash
# Emit shadow_lower.ccs (+ lowered local headers) into
# cc/bootstrap/shadow_lower/latest/. Does not promote or commit.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/cc/bootstrap/shadow_lower"
LATEST="$BOOT/latest"
SRC="$ROOT/examples/serdes/c/shadow_lower.ccs"
HDR_SRC="$ROOT/out/include/examples/serdes/c"
CCC="${CCC:-$ROOT/cc/bin/ccc}"
SMOKE=0

for arg in "$@"; do
  case "$arg" in
    --smoke) SMOKE=1 ;;
    -h|--help)
      echo "usage: $0 [--smoke]"
      exit 0
      ;;
    *)
      echo "error: unknown arg: $arg" >&2
      exit 2
      ;;
  esac
done

if [[ ! -x "$CCC" ]]; then
  echo "error: missing ccc at $CCC (make -C cc)" >&2
  exit 1
fi
if [[ ! -f "$SRC" ]]; then
  echo "error: missing $SRC" >&2
  exit 1
fi

rm -rf "$LATEST"
mkdir -p "$LATEST/include"

echo "[snapshot] emit $SRC → $LATEST/shadow_lower.c"
CC_FRONTEND=legacy "$CCC" --frontend=legacy --emit-c-only --no-cache \
  "$SRC" -o "$LATEST/shadow_lower.c" \
  --cc-flags "-I$ROOT/examples/serdes/c -I$ROOT/third_party/tcc -DSHADOW_HAVE_LIBTCC=1"

if [[ ! -d "$HDR_SRC" ]] || [[ -z "$(ls -A "$HDR_SRC"/*.h 2>/dev/null || true)" ]]; then
  echo "error: missing lowered serdes headers under $HDR_SRC" >&2
  echo "  (ccc should have lowered them; run: make -C cc)" >&2
  exit 1
fi

echo "[snapshot] copy lowered headers → $LATEST/include/"
cp -f "$HDR_SRC"/*.h "$LATEST/include/"

echo "[snapshot] rewrite absolute serdes includes / #line paths"
python3 - "$ROOT" "$LATEST" <<'PY'
import pathlib, re, sys

root = pathlib.Path(sys.argv[1]).resolve()
latest = pathlib.Path(sys.argv[2]).resolve()
root_s = str(root)

# #include "/abs/.../out/include/examples/serdes/c/foo.h" → #include "foo.h"
inc_abs = re.compile(
    r'#include\s+"[^"]*/out/include/examples/serdes/c/([^"/]+)"'
)
# #line N "/abs/repo/rel" → #line N "rel"
line_abs = re.compile(
    r'(#line\s+\d+\s+)"' + re.escape(root_s) + r'/([^"]+)"'
)

def rewrite(text: str) -> str:
    text = inc_abs.sub(r'#include "\1"', text)
    text = line_abs.sub(r'\1"\2"', text)
    return text

changed = 0
for path in [latest / "shadow_lower.c", *sorted((latest / "include").glob("*.h"))]:
    old = path.read_text(encoding="utf-8", errors="surrogateescape")
    new = rewrite(old)
    if new != old:
        path.write_text(new, encoding="utf-8", errors="surrogateescape")
        changed += 1
print(f"[snapshot] rewrote {changed} files")
PY

{
  echo "source: examples/serdes/c/shadow_lower.ccs"
  echo "created_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_head: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "emit_bytes: $(wc -c < "$LATEST/shadow_lower.c" | tr -d ' ')"
  echo "header_count: $(ls "$LATEST/include"/*.h | wc -l | tr -d ' ')"
} > "$LATEST/SNAPSHOT.txt"

echo "[snapshot] wrote $LATEST ($(du -sh "$LATEST" | awk '{print $1}'))"

if [[ "$SMOKE" -eq 1 ]]; then
  echo "[snapshot] host-cc smoke of latest/"
  OBJ="$ROOT/out/cc/obj"
  OUT_BIN="$LATEST/shadow_lower"
  cc -O2 -o "$OUT_BIN" "$LATEST/shadow_lower.c" \
    -I"$LATEST/include" \
    -I"$ROOT/cc/include" \
    -I"$ROOT/out/include" \
    -I"$ROOT/examples/serdes/c" \
    -I"$ROOT/third_party/tcc" \
    -DSHADOW_HAVE_LIBTCC=1 \
    "$OBJ/shadow_tcc_compile.o" \
    "$OBJ/libshadow_comptime.a" \
    "$OBJ/runtime/concurrent_c.o" \
    -L"$ROOT/third_party/tcc" -ltcc -lpthread -lm
  "$OUT_BIN" "$ROOT/examples/hello.ccs" -o "$LATEST/hello_smoke.c" --no-cache
  test -s "$LATEST/hello_smoke.c"
  echo "[snapshot] smoke OK → $OUT_BIN"
fi

echo "[snapshot] done (not promoted; run scripts/promote_shadow_bootstrap.sh when ready)"
