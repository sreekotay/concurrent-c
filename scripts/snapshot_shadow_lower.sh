#!/usr/bin/env bash
# Emit shadow_lower.ccs (+ lowered local headers) into
# cc/bootstrap/shadow_lower/latest/. Does not promote or commit.
#
# Native self-emit only: requires an existing shadow_lower binary
# (make -C cc / SHADOW_LOWER_SOURCE=ccs). The old --legacy ccc escape is gone.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BOOT="$ROOT/cc/bootstrap/shadow_lower"
LATEST="$BOOT/latest"
SRC="$ROOT/cc/shadow/shadow_lower.ccs"
HDR_SRC="$ROOT/out/include/cc/shadow"
SHADOW="${SHADOW:-}"
SMOKE=0

for arg in "$@"; do
  case "$arg" in
    --smoke) SMOKE=1 ;;
    --legacy)
      echo "error: --legacy snapshot emit is retired; use native shadow_lower" >&2
      echo "  make -C cc SHADOW_LOWER_SOURCE=ccs ../out/cc/bin/shadow_lower" >&2
      exit 2
      ;;
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

if [[ -z "$SHADOW" ]]; then
  echo "error: no shadow_lower binary (make -C cc)" >&2
  exit 1
fi
ts() { echo "[snapshot] $(date '+%H:%M:%S') $*"; }
t0=$(date +%s)
ts "emit start via $SHADOW ($SRC --no-cache)"
"$SHADOW" "$SRC" -o "$LATEST/shadow_lower.c" --no-cache
ts "emit done ($(( $(date +%s) - t0 ))s, $(wc -c < "$LATEST/shadow_lower.c" | tr -d ' ') bytes)"
EMITTER="native:$SHADOW"

if [[ ! -s "$LATEST/shadow_lower.c" ]]; then
  echo "error: emit produced empty $LATEST/shadow_lower.c" >&2
  exit 1
fi

# Companion lowered headers from out/include. Needed when the emit still
# #includes local .h faces; harmless if emit inlined everything.
if [[ -d "$HDR_SRC" ]] && ls "$HDR_SRC"/*.h >/dev/null 2>&1; then
  echo "[snapshot] copy lowered headers → $LATEST/include/"
  cp -f "$HDR_SRC"/*.h "$LATEST/include/"
else
  echo "[snapshot] WARN: no lowered shadow headers under $HDR_SRC" >&2
fi

echo "[snapshot] rewrite absolute includes / #line paths"
python3 - "$ROOT" "$LATEST" <<'PY'
import pathlib, re, sys

root = pathlib.Path(sys.argv[1]).resolve()
latest = pathlib.Path(sys.argv[2]).resolve()
root_s = str(root)

inc_abs = re.compile(
    r'#include\s+"[^"]*/out/include/(?:cc/shadow|examples/serdes/c)/([^"/]+)"'
)
# Self-emit may spell local lowered headers repo-relative in angle form;
# a snapshot must resolve them from its own include/ dir, not a warm out/ tree.
inc_rel = re.compile(
    r'#include\s+<(?:cc/shadow|examples/serdes/c)/([^>/]+)>'
)
line_abs = re.compile(
    r'(#line\s+\d+\s+)"' + re.escape(root_s) + r'/([^"]+)"'
)

inc_bare = re.compile(r'#include\s+<([^>"/]+\.h)>')
have_hdrs = {p.name for p in (latest / "include").glob("*.h")} if (latest / "include").is_dir() else set()

def rewrite(text: str) -> str:
    # Real glue bug is a directive line, not a comment mentioning the token.
    n_de = len(re.findall(r'(?m)^de#line\b', text))
    if n_de:
        print(f"error: emit contains {n_de}× de#line "
              f"(injected-token lead bug in shadow_attach_lead)", file=sys.stderr)
        sys.exit(1)
    text = inc_abs.sub(r'#include "\1"', text)
    text = inc_rel.sub(r'#include "\1"', text)
    text = line_abs.sub(r'\1"\2"', text)
    # Native emit may spell local lowered headers as bare <foo.h>.
    def bare(m: re.Match) -> str:
        name = m.group(1)
        if name in have_hdrs:
            return f'#include "{name}"'
        return m.group(0)
    text = inc_bare.sub(bare, text)
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
  echo "source: cc/shadow/shadow_lower.ccs"
  echo "emitter: $EMITTER"
  echo "created_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_head: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "emit_bytes: $(wc -c < "$LATEST/shadow_lower.c" | tr -d ' ')"
  echo "header_count: $hdr_count"
} > "$LATEST/SNAPSHOT.txt"

echo "[snapshot] wrote $LATEST ($(du -sh "$LATEST" | awk '{print $1}'))"

if [[ "$SMOKE" -eq 1 ]]; then
  t1=$(date +%s)
  ts "host-cc smoke start"
  OBJ="$ROOT/out/cc/obj"
  OUT_BIN="$LATEST/shadow_lower"
  for need in "$OBJ/shadow_tcc_compile.o" "$OBJ/libshadow_comptime.a" \
              "$OBJ/runtime/concurrent_c.o" "$OBJ/cparse/libcparse.a"; do
    if [[ ! -e "$need" ]]; then
      echo "error: missing $need (make -C cc first)" >&2
      exit 1
    fi
  done
  cc -O2 -o "$OUT_BIN" "$LATEST/shadow_lower.c" \
    -I"$LATEST/include" \
    -I"$ROOT/cc/include" \
    -I"$ROOT/out/include" \
    -I"$ROOT/cc/shadow" \
    -I"$ROOT/cc/src" \
    -I"$ROOT/third_party/tcc" \
    -DSHADOW_HAVE_LIBTCC=1 \
    "$OBJ/shadow_tcc_compile.o" \
    "$OBJ/runtime/concurrent_c.o" \
    "$OBJ/libshadow_comptime.a" \
    "$OBJ/cparse/libcparse.a" \
    -L"$ROOT/third_party/tcc" -ltcc -lpthread -lm
  ts "host-cc smoke binary done ($(( $(date +%s) - t1 ))s)"
  t2=$(date +%s)
  ts "hello.ccs emit start"
  "$OUT_BIN" "$ROOT/examples/hello.ccs" -o "$LATEST/hello_smoke.c" --no-cache
  test -s "$LATEST/hello_smoke.c"
  ts "hello.ccs emit done ($(( $(date +%s) - t2 ))s) — smoke OK → $OUT_BIN"
fi

echo "[snapshot] done (not promoted; run scripts/promote_shadow_bootstrap.sh when ready)"
