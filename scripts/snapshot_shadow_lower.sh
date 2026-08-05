#!/usr/bin/env bash
# Emit shadow_lower.ccs (+ lowered local headers) into
# cc/bootstrap/shadow_lower/latest/. Does not promote or commit.
#
# Prefers an existing shadow_lower binary (serdes self-emit). Fallback:
#   SNAPSHOT_EMITTER=legacy  — force legacy ccc --emit-c-only
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/cc/bootstrap/shadow_lower"
LATEST="$BOOT/latest"
SRC="$ROOT/examples/serdes/c/shadow_lower.ccs"
HDR_SRC="$ROOT/out/include/examples/serdes/c"
CCC="${CCC:-$ROOT/cc/bin/ccc}"
SHADOW="${SHADOW:-}"
SMOKE=0
FORCE_LEGACY=0

for arg in "$@"; do
  case "$arg" in
    --smoke) SMOKE=1 ;;
    --legacy) FORCE_LEGACY=1 ;;
    -h|--help)
      echo "usage: $0 [--smoke] [--legacy]"
      exit 0
      ;;
    *)
      echo "error: unknown arg: $arg" >&2
      exit 2
      ;;
  esac
done

if [[ "${SNAPSHOT_EMITTER:-}" == "legacy" ]]; then
  FORCE_LEGACY=1
fi

if [[ ! -f "$SRC" ]]; then
  echo "error: missing $SRC" >&2
  exit 1
fi

if [[ -z "$SHADOW" ]]; then
  for cand in "$ROOT/out/cc/bin/shadow_lower" "$ROOT/cc/bin/shadow_lower"; do
    if [[ -x "$cand" ]]; then SHADOW="$cand"; break; fi
  done
fi

rm -rf "$LATEST"
mkdir -p "$LATEST/include"

EMITTER="legacy"
if [[ "$FORCE_LEGACY" -eq 0 && -n "$SHADOW" ]]; then
  echo "[snapshot] emit via shadow_lower ($SHADOW)"
  "$SHADOW" "$SRC" -o "$LATEST/shadow_lower.c" --no-cache
  EMITTER="serdes:$SHADOW"
else
  if [[ ! -x "$CCC" ]]; then
    echo "error: missing ccc at $CCC (make -C cc)" >&2
    exit 1
  fi
  echo "[snapshot] emit via legacy ccc ($CCC)"
  CC_FRONTEND=legacy "$CCC" --frontend=legacy --emit-c-only --no-cache \
    "$SRC" -o "$LATEST/shadow_lower.c" \
    --cc-flags "-I$ROOT/examples/serdes/c -I$ROOT/third_party/tcc -DSHADOW_HAVE_LIBTCC=1"
  EMITTER="legacy:$CCC"
fi

if [[ ! -s "$LATEST/shadow_lower.c" ]]; then
  echo "error: emit produced empty $LATEST/shadow_lower.c" >&2
  exit 1
fi

# Companion lowered headers (legacy header lowerer). Needed when the emit
# still #includes local .h faces; harmless if emit inlined everything.
if [[ -d "$HDR_SRC" ]] && ls "$HDR_SRC"/*.h >/dev/null 2>&1; then
  echo "[snapshot] copy lowered headers → $LATEST/include/"
  cp -f "$HDR_SRC"/*.h "$LATEST/include/"
else
  echo "[snapshot] WARN: no lowered serdes headers under $HDR_SRC" >&2
fi

echo "[snapshot] rewrite absolute serdes includes / #line paths"
python3 - "$ROOT" "$LATEST" <<'PY'
import pathlib, re, sys

root = pathlib.Path(sys.argv[1]).resolve()
latest = pathlib.Path(sys.argv[2]).resolve()
root_s = str(root)

inc_abs = re.compile(
    r'#include\s+"[^"]*/out/include/examples/serdes/c/([^"/]+)"'
)
# Serdes self-emit spells local lowered headers repo-relative in angle form;
# a snapshot must resolve them from its own include/ dir, not a warm out/ tree.
inc_rel = re.compile(
    r'#include\s+<examples/serdes/c/([^>/]+)>'
)
line_abs = re.compile(
    r'(#line\s+\d+\s+)"' + re.escape(root_s) + r'/([^"]+)"'
)

def rewrite(text: str) -> str:
    text = inc_abs.sub(r'#include "\1"', text)
    text = inc_rel.sub(r'#include "\1"', text)
    text = line_abs.sub(r'\1"\2"', text)
    return text

changed = 0
paths = [latest / "shadow_lower.c"]
inc = latest / "include"
if inc.is_dir():
    paths.extend(sorted(inc.glob("*.h")))
for path in paths:
    old = path.read_text(encoding="utf-8", errors="surrogateescape")
    new = rewrite(old)
    if new != old:
        path.write_text(new, encoding="utf-8", errors="surrogateescape")
        changed += 1
print(f"[snapshot] rewrote {changed} files")
PY

hdr_count=0
if ls "$LATEST/include"/*.h >/dev/null 2>&1; then
  hdr_count=$(ls "$LATEST/include"/*.h | wc -l | tr -d ' ')
fi
{
  echo "source: examples/serdes/c/shadow_lower.ccs"
  echo "emitter: $EMITTER"
  echo "created_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_head: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "emit_bytes: $(wc -c < "$LATEST/shadow_lower.c" | tr -d ' ')"
  echo "header_count: $hdr_count"
} > "$LATEST/SNAPSHOT.txt"

echo "[snapshot] wrote $LATEST ($(du -sh "$LATEST" | awk '{print $1}'))"

if [[ "$SMOKE" -eq 1 ]]; then
  echo "[snapshot] host-cc smoke of latest/"
  OBJ="$ROOT/out/cc/obj"
  OUT_BIN="$LATEST/shadow_lower"
  for need in "$OBJ/shadow_tcc_compile.o" "$OBJ/libshadow_comptime.a" \
              "$OBJ/runtime/concurrent_c.o"; do
    if [[ ! -e "$need" ]]; then
      echo "error: missing $need (make -C cc first)" >&2
      exit 1
    fi
  done
  cc -O2 -o "$OUT_BIN" "$LATEST/shadow_lower.c" \
    -I"$LATEST/include" \
    -I"$ROOT/cc/include" \
    -I"$ROOT/out/include" \
    -I"$ROOT/examples/serdes/c" \
    -I"$ROOT/third_party/tcc" \
    -DSHADOW_HAVE_LIBTCC=1 \
    "$OBJ/shadow_tcc_compile.o" \
    "$OBJ/runtime/concurrent_c.o" \
    "$OBJ/libshadow_comptime.a" \
    -L"$ROOT/third_party/tcc" -ltcc -lpthread -lm
  "$OUT_BIN" "$ROOT/examples/hello.ccs" -o "$LATEST/hello_smoke.c" --no-cache
  test -s "$LATEST/hello_smoke.c"
  echo "[snapshot] smoke OK → $OUT_BIN"
fi

echo "[snapshot] done (not promoted; run scripts/promote_shadow_bootstrap.sh when ready)"
