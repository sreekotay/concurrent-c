#!/usr/bin/env bash
# Build and run the json.h benchmark.
#   ./bench.sh            ours, default corpora (twitter.json + numbers.json)
#   ./bench.sh -y         also run yyjson (vendored yyjson.c/.h)
#   ./bench.sh -g         also run the @grammar engine tiers (needs make -C cc)
#   ./bench.sh -w         also run the @grammar WRITE bench (Feed -> canonical JSON)
#   ./bench.sh -s         also run the hidden-class (shapes) DOM vs tape DOM
#   ./bench.sh -d         also run the ENGINE shaped DOM (cc_dom) vs tape + access
#   ./bench.sh -a         all of the above (-y -g -w -s -d)
#   ./bench.sh -c         also print a correctness checksum (verified untimed)
#   ./bench.sh [K] [corpus...]
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
# Host C must use lowered headers/runtime — raw .cch carries Concurrent-C surface
# (@as etc.) that gcc cannot parse. out/include first; cc/include second for
# plain vendor headers (ffc.h) that lower-headers does not copy.
OUT_INC="$repo/out/include"
INC="$repo/cc/include"
RT="$repo/out/runtime/arena_state.c"
CCC="$repo/cc/bin/ccc"
# Dead-strip like ccc --release does: without it every binary carries the
# runtime TU's full ffc surface (~35 KB dead) and size comparisons lie.
case "$(uname)" in Darwin) LDGC="-Wl,-dead_strip";; *) LDGC="-Wl,--gc-sections";; esac
CC="${CC:-gcc} -O2 -ffunction-sections -fdata-sections $LDGC -I$OUT_INC -I$INC"
YY=0; GEN=0; WR=0; SH=0; DM=0; CHK=""
while :; do
  case "$1" in
    -y) YY=1; shift;;
    -g) GEN=1; shift;;
    -w) WR=1; shift;;
    -s) SH=1; shift;;
    -d) DM=1; shift;;
    -a) YY=1; GEN=1; WR=1; SH=1; DM=1; shift;;
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

if [ ! -f "$OUT_INC/ccc/cc_arena.h" ] || [ ! -f "$RT" ]; then
  echo "lowered headers/runtime missing — run: make -C $repo/cc lower-headers" >&2
  exit 1
fi

if need_build "$here/bench" "$here/bench.c" "$here/json.h" "$RT" "$OUT_INC/ccc/cc_arena.h"; then
  $CC "$here/bench.c" "$RT" -o "$here/bench"
fi
if [ "$YY" = 1 ]; then
  [ -f "$here/yyjson.c" ] || { echo "yyjson.c not found"; exit 1; }
  if need_build "$here/yy" "$here/yy.c" "$here/yyjson.c"; then
    $CC "$here/yy.c" "$here/yyjson.c" -o "$here/yy"
  fi
fi
if [ "$GEN" = 1 ]; then
  if [ ! -x "$CCC" ]; then
    echo "-g: compiler not built ($CCC missing) — run: make -C $repo/cc"; exit 1
  fi
  if need_build "$here/bench_gen" "$here/bench_grammar.ccs" "$CCC"; then
    (cd "$repo" && "$CCC" build --release "$here/bench_grammar.ccs" \
      -o "$here/bench_gen" --out-dir "$here/.gen" >/dev/null)
  fi
fi
if [ "$SH" = 1 ]; then
  if need_build "$here/bench_shape" "$here/bench_shape.c" "$here/json_shape.h" \
                "$here/json.h" "$RT" "$OUT_INC/ccc/cc_arena.h"; then
    $CC "$here/bench_shape.c" "$RT" -o "$here/bench_shape"
  fi
fi
if [ "$DM" = 1 ]; then
  if [ ! -x "$CCC" ]; then
    echo "-d: compiler not built ($CCC missing) — run: make -C $repo/cc"; exit 1
  fi
  if need_build "$here/bench_dom" "$here/bench_dom.ccs" "$CCC"; then
    (cd "$repo" && "$CCC" build --release "$here/bench_dom.ccs" \
      -o "$here/bench_dom" --out-dir "$here/.gend" >/dev/null)
  fi
fi
if [ "$WR" = 1 ]; then
  if [ ! -x "$CCC" ]; then
    echo "-w: compiler not built ($CCC missing) — run: make -C $repo/cc"; exit 1
  fi
  if need_build "$here/bench_write" "$here/bench_write.ccs" "$CCC"; then
    (cd "$repo" && "$CCC" build --release "$here/bench_write.ccs" \
      -o "$here/bench_write" --out-dir "$here/.genw" >/dev/null)
  fi
fi

for c in "${CORPORA[@]}"; do
  echo "== $c  (K=$K) =="
  printf 'ours   '; "$here/bench" $CHK "$here/$c" "$K" 3
  if [ "$YY" = 1 ]; then "$here/yy" "$here/$c" "$K"; fi
  if [ "$GEN" = 1 ]; then "$here/bench_gen" "$here/$c" "$K" 3 | sed 's/^/  /'; fi
  if [ "$WR" = 1 ]; then "$here/bench_write" "$here/$c" | sed 's/^/  /'; fi
  if [ "$SH" = 1 ]; then "$here/bench_shape" "$here/$c" "$K" 3 | sed 's/^/  /'; fi
  if [ "$DM" = 1 ]; then "$here/bench_dom" "$here/$c" "$K" | sed 's/^/  /'; fi
done
