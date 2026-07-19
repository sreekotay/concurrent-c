#!/usr/bin/env bash
# RESP (Redis Serialization Protocol) benchmark: generated @grammar(schema)
# parser + writer vs hand-rolled references on a redis-benchmark-shaped
# corpus of SET commands (synthesized in-process — no corpus file).
#   ./bench.sh [NCMD] [K]   NCMD = commands in corpus (default 100000),
#                           K = corpus replays per trial (default 30)
set -e
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
INC="$repo/cc/include"; RT="$repo/cc/runtime/arena_state.c"
CC="${CC:-gcc} -O2"
CCC="$repo/cc/bin/ccc"
NCMD=${1:-100000}; K=${2:-30}

if [ ! -x "$CCC" ]; then
  echo "compiler not built ($CCC missing) — run: make -C $repo/cc"; exit 1
fi

need_build() {
  local bin=$1; shift
  [ ! -x "$bin" ] && return 0
  local s; for s in "$@"; do [ "$s" -nt "$bin" ] && return 0; done
  return 1
}

if need_build "$here/bench_resp" "$here/bench_resp.ccs" "$CCC" "$RT"; then
  (cd "$repo" && "$CCC" build "$here/bench_resp.ccs" --out-dir "$here/.gen" >/dev/null)
  $CC -I "$repo/out/include" -I "$INC" "$here/.gen/bench_resp.c" "$RT" -o "$here/bench_resp"
fi

"$here/bench_resp" "$NCMD" "$K"
