#!/usr/bin/env bash
# Build and run the json.h benchmark.
#   ./bench.sh            ours, default corpora (twitter.json + numbers.json)
#   ./bench.sh -y         also run yyjson (vendored yyjson.c/.h)
#   ./bench.sh [K] [corpus...]
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
INC="$repo/cc/include"; RT="$repo/cc/runtime/arena_state.c"
CC="${CC:-gcc} -O2 -I $INC"
YY=0; [ "$1" = "-y" ] && { YY=1; shift; }
K=${1:-200}; shift || true
if [ $# -eq 0 ]; then CORPORA=(twitter.json numbers.json); else CORPORA=("$@"); fi

need_build() {  # $1=binary  rest=sources — rebuild if missing or any source newer
  local bin=$1; shift
  [ ! -x "$bin" ] && return 0
  local s; for s in "$@"; do [ "$s" -nt "$bin" ] && return 0; done
  return 1
}

if need_build "$here/bench" "$here/bench.c" "$here/json.h" "$RT"; then
  $CC "$here/bench.c" "$RT" -o "$here/bench"
fi
if [ "$YY" = 1 ]; then
  [ -f "$here/yyjson.c" ] || { echo "yyjson.c not found"; exit 1; }
  if need_build "$here/yy" "$here/yy.c" "$here/yyjson.c"; then
    $CC "$here/yy.c" "$here/yyjson.c" -o "$here/yy"
  fi
fi

for c in "${CORPORA[@]}"; do
  echo "== $c  (K=$K) =="
  printf 'ours   '; "$here/bench" "$here/$c" "$K" 3
  if [ "$YY" = 1 ]; then "$here/yy" "$here/$c" "$K"; fi
done
