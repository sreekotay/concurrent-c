#!/usr/bin/env bash
# Build and run the json.h benchmark.
#   ./bench.sh            ours, default corpora (twitter.json + numbers.json)
#   ./bench.sh -y         also run yyjson (requires yyjson.c/.h in this dir — see README)
#   ./bench.sh [K] [corpus...]
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
INC="$repo/cc/include"; RT="$repo/cc/runtime/arena_state.c"
CC="${CC:-gcc} -O2 -I $INC"
YY=0; [ "$1" = "-y" ] && { YY=1; shift; }
K=${1:-200}; shift || true
CORPORA=("${@:-twitter.json numbers.json}")

if [ ! -x "$here/bench" ] || [ "$here/bench.c" -nt "$here/bench" ] || [ "$here/json.h" -nt "$here/bench" ]; then
  $CC "$here/bench.c" "$RT" -o "$here/bench"
fi
if [ "$YY" = 1 ]; then
  [ -f "$here/yyjson.c" ] || { echo "yyjson.c not found — see README to enable -y"; exit 1; }
  [ -x "$here/yy" ] || $CC "$here/yy.c" "$here/yyjson.c" -o "$here/yy"
fi

for c in ${CORPORA[@]}; do
  echo "== $c  (K=$K) =="
  printf 'ours   '; "$here/bench" "$here/$c" "$K" 3
  if [ "$YY" = 1 ]; then "$here/yy" "$here/$c" "$K"; fi
done
