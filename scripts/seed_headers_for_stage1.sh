#!/usr/bin/env bash
# Crude host-C seed of out/include (+ optional runtime rewrite) so stage-1
# lower_headers can compile when neither out/include nor the stage-1 binary
# exists yet (true fresh clone / smoke_bootstrap_fresh).
#
# Not a substitute for real lowering: stage-1 must re-run over this seed.
# Only strips `@as` / legacy `/*@as*/` and rewrites `.cch` includes to `.h`.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
IN_INC="${1:-$ROOT_DIR/cc/include}"
OUT_INC="${2:-$ROOT_DIR/out/include}"
IN_RT="${3:-}"
OUT_RT="${4:-}"

die() { printf 'seed_headers_for_stage1: %s\n' "$*" >&2; exit 1; }
test -d "$IN_INC" || die "missing input include dir: $IN_INC"

python3 - "$IN_INC" "$OUT_INC" "$IN_RT" "$OUT_RT" <<'PY'
import pathlib, re, sys

in_inc = pathlib.Path(sys.argv[1])
out_inc = pathlib.Path(sys.argv[2])
in_rt = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None
out_rt = pathlib.Path(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4] else None

def seed_text(text: str) -> str:
    text = text.replace("/*@as*/", "")
    text = re.sub(r"@as\b", "", text)
    text = text.replace(".cch>", ".h>").replace('.cch"', '.h"')
    return text

n_h = 0
for src in in_inc.rglob("*.cch"):
    rel = src.relative_to(in_inc)
    dst = out_inc / rel.with_suffix(".h")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(seed_text(src.read_text(encoding="utf-8", errors="replace")),
                   encoding="utf-8")
    n_h += 1

n_rt = 0
if in_rt and out_rt and in_rt.is_dir():
    for src in list(in_rt.glob("*.c")) + list(in_rt.glob("*.h")):
        dst = out_rt / src.name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(seed_text(src.read_text(encoding="utf-8", errors="replace")),
                       encoding="utf-8")
        n_rt += 1

print(f"seed_headers_for_stage1: {n_h} headers -> {out_inc}"
      + (f"; {n_rt} runtime -> {out_rt}" if n_rt else ""))
PY
