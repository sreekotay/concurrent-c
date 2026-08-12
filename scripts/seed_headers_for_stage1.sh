#!/usr/bin/env bash
# Crude host-C seed of out/include (+ optional runtime rewrite) so stage-1
# lower_headers can compile when neither out/include nor the stage-1 binary
# exists yet (true fresh clone / smoke_bootstrap_fresh).
#
# Not a substitute for real lowering: stage-1 must re-run over this seed.
# Only strips `@as` / legacy `/*@as*/`, `@typeview` / `@restricted` /
# `@typehooks` blocks, and rewrites `.cch` includes to `.h`.
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

def strip_typeview_blocks(text: str) -> str:
    """Erase @typeview / @restricted / @typehooks define blocks (brace-matched)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] in "\"'":
            q = text[i]
            out.append(q)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if text.startswith("//", i):
            while i < n and text[i] != "\n":
                out.append(text[i])
                i += 1
            continue
        if text.startswith("/*", i):
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append(text[i])
                i += 1
            if i + 1 < n:
                out.append(text[i])
                out.append(text[i + 1])
                i += 2
            continue
        kw = None
        if text.startswith("@typeview", i) and (
            i + 9 == n or not (text[i + 9].isalnum() or text[i + 9] == "_")
        ):
            kw = 9
        elif text.startswith("@restricted", i) and (
            i + 11 == n or not (text[i + 11].isalnum() or text[i + 11] == "_")
        ):
            kw = 11
        elif text.startswith("@typehooks", i) and (
            i + 10 == n or not (text[i + 10].isalnum() or text[i + 10] == "_")
        ):
            kw = 10
        if kw is not None:
            # Drop a leading `typedef` that only exists for the define form.
            j = len(out)
            while j > 0 and out[j - 1] in " \t\n\r":
                j -= 1
            if j >= 7 and "".join(out[j - 7 : j]) == "typedef":
                j -= 7
                while j > 0 and out[j - 1] in " \t":
                    j -= 1
                del out[j:]
            p = i + kw
            # Sugar @typeview(Mode) Base — erase keyword+parens only.
            while p < n and text[p] in " \t\n\r":
                p += 1
            if p < n and text[p] == "(":
                depth = 0
                while p < n:
                    if text[p] == "(":
                        depth += 1
                    elif text[p] == ")":
                        depth -= 1
                        p += 1
                        break
                    p += 1
                i = p
                continue
            # Define form: … on Base[*] { … } ;
            while p < n and text[p] != "{":
                p += 1
            if p >= n:
                i += kw
                continue
            depth = 0
            while p < n:
                if text[p] == "{":
                    depth += 1
                elif text[p] == "}":
                    depth -= 1
                    if depth == 0:
                        p += 1
                        break
                p += 1
            while p < n and text[p] in " \t\n\r":
                p += 1
            if p < n and text[p] == ";":
                p += 1
            i = p
            continue
        out.append(text[i])
        i += 1
    return "".join(out)

def seed_text(text: str) -> str:
    text = text.replace("/*@as*/", "")
    text = re.sub(r"@as\b", "", text)
    text = strip_typeview_blocks(text)
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
