#!/usr/bin/env bash
# Build and run the json.h benchmark.
#   ./bench.sh            ours, default corpora (twitter.json + numbers.json)
#   ./bench.sh -y         also run yyjson (vendored yyjson.c/.h)
#   ./bench.sh -g         also run the @grammar engine tiers (needs make -C cc)
#   ./bench.sh -c         also print a correctness checksum (verified untimed)
#   ./bench.sh [K] [corpus...]
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
INC="$repo/cc/include"; RT="$repo/cc/runtime/arena_state.c"
CC="${CC:-gcc} -O2 -I $INC"
YY=0; GEN=0; CHK=""
while :; do
  case "$1" in
    -y) YY=1; shift;;
    -g) GEN=1; shift;;
    -c) CHK="-c"; shift;;
    *) break;;
  esac
done
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
if [ "$GEN" = 1 ]; then
  CCC="$repo/cc/bin/ccc"
  if [ ! -x "$CCC" ]; then
    echo "-g: compiler not built ($CCC missing) — run: make -C $repo/cc"; exit 1
  fi
  if need_build "$here/bench_gen" "$here/bench_grammar.ccs" "$CCC" "$RT"; then
    (cd "$repo" && "$CCC" build "$here/bench_grammar.ccs" --out-dir "$here/.gen" >/dev/null)
    $CC -I "$repo/out/include" "$here/.gen/bench_grammar.c" "$RT" -o "$here/bench_gen"
  fi
fi

for c in "${CORPORA[@]}"; do
  echo "== $c  (K=$K) =="
  printf 'ours   '; "$here/bench" $CHK "$here/$c" "$K" 3
  if [ "$YY" = 1 ]; then "$here/yy" "$here/$c" "$K"; fi
  if [ "$GEN" = 1 ]; then "$here/bench_gen" "$here/$c" "$K" 3 | sed 's/^/  /'; fi
done
