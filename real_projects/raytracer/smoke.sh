#!/usr/bin/env bash
# C seq vs CC seq vs CC @parallel for on the weekend smoke image.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CCC="${CCC:-$ROOT/cc/bin/ccc}"
OUT="${RT_SMOKE_OUT:-$ROOT/out/raytracer_smoke}"
mkdir -p "$OUT"

cc -O2 -o "$OUT/rt_c" "$ROOT/real_projects/raytracer/rt.c" -lm
"$CCC" build --release "$ROOT/real_projects/raytracer/rt.ccs" -o "$OUT/rt_cc"

pick() { sed -n 's/.*checksum=\([^ ]*\).*/\1/p' | tail -1; }

c=$(RT_SMOKE=1 "$OUT/rt_c")
s=$(RT_SMOKE=1 RT_SEQ=1 "$OUT/rt_cc")
p=$(RT_SMOKE=1 RT_SEQ=0 "$OUT/rt_cc")
echo "$c"
echo "$s"
echo "$p"

cs=$(printf '%s\n' "$c" | pick)
ss=$(printf '%s\n' "$s" | pick)
ps=$(printf '%s\n' "$p" | pick)
if [[ -z "$cs" || "$cs" != "$ss" || "$cs" != "$ps" ]]; then
  echo "checksum mismatch c=$cs seq=$ss par=$ps" >&2
  exit 1
fi
echo ok
