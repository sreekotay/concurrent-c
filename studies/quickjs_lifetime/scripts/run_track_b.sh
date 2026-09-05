#!/usr/bin/env bash
# Build + run Track B CC timer host (libuv + QuickJS). Optional txiki/Rust later.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STUDY="$ROOT/studies/quickjs_lifetime"
TB="$STUDY/track_b"
RESULTS="$TB/results"
DATE="$(date +%Y%m%d)"
BUILD="$RESULTS/raw"
mkdir -p "$RESULTS" "$BUILD"
: >"$RESULTS/.gitkeep"

if [[ -z "${CC_QUICKJS_SRC:-}" ]]; then
  if [[ -d /tmp/quickjs-ng ]]; then
    export CC_QUICKJS_SRC=/tmp/quickjs-ng
  elif [[ -d "$ROOT/quickjs" ]]; then
    export CC_QUICKJS_SRC="$ROOT/quickjs"
  else
    echo "error: set CC_QUICKJS_SRC" >&2
    exit 2
  fi
fi

LIBUV_SRC="$TB/deps/libuv"
LIBUV_BUILD="$TB/deps/libuv-build"
LIBUV_PIN=aabb7651de73ec2f1a74361ca3430eed1a62e402

if [[ ! -f "$LIBUV_BUILD/libuv.a" ]]; then
  echo "== building pinned libuv ($LIBUV_PIN)"
  if [[ ! -d "$LIBUV_SRC/.git" ]]; then
    mkdir -p "$TB/deps"
    git clone https://github.com/libuv/libuv.git "$LIBUV_SRC"
    git -C "$LIBUV_SRC" checkout "$LIBUV_PIN"
  fi
  cmake -S "$LIBUV_SRC" -B "$LIBUV_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBUV_BUILD_SHARED=OFF \
    -DLIBUV_BUILD_TESTS=OFF
  cmake --build "$LIBUV_BUILD" -j
fi

CCC="${CCC:-$ROOT/cc/bin/ccc}"
[[ -x "$CCC" ]] || CCC="$(command -v ccc || true)"
[[ -n "$CCC" ]] || { echo "error: ccc not found" >&2; exit 2; }

UV_INC="$LIBUV_SRC/include"
UV_LIB="$LIBUV_BUILD/libuv.a"
LD_EXTRA="$UV_LIB"
case "$(uname -s)" in
  Darwin) LD_EXTRA="$LD_EXTRA -framework CoreFoundation" ;;
  Linux) LD_EXTRA="$LD_EXTRA -lpthread -ldl" ;;
esac

echo "== Track B CC  CC_QUICKJS_SRC=$CC_QUICKJS_SRC"
"$CCC" build "$TB/cc/driver.ccs" \
  -o "$BUILD/driver_cc" \
  --cc-flags "-I$TB/cc -I$UV_INC" \
  --ld-flags "$LD_EXTRA"

SCRIPT="$TB/workload/timers.js"
CC_OUT="$BUILD/${DATE}_cc.jsonl"
"$BUILD/driver_cc" "$SCRIPT" | tee "$CC_OUT"

oracle=FAIL
grep -q 'ORACLE PASS' "$CC_OUT" && oracle=PASS

{
  echo "Track B CC timer host"
  echo "date: $DATE"
  echo "quickjs: $(cd "$CC_QUICKJS_SRC" && git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "libuv: $LIBUV_PIN"
  echo "oracle: $oracle"
  echo "---"
  grep -E '^(META|CHK|PERF|ORACLE)' "$CC_OUT" || true
} | tee "$RESULTS/${DATE}_cc.txt"

# Optional upstream reference if tjs is on PATH or TXIKI_BIN set.
TXIKI_BIN="${TXIKI_BIN:-}"
if [[ -z "$TXIKI_BIN" && -x /tmp/txiki.js/build/tjs ]]; then
  TXIKI_BIN=/tmp/txiki.js/build/tjs
fi
if [[ -n "$TXIKI_BIN" && -x "$TXIKI_BIN" ]]; then
  echo "== txiki reference: $TXIKI_BIN"
  "$TXIKI_BIN" run "$SCRIPT" | tee "$BUILD/${DATE}_txiki.jsonl" || true
fi

# Optional Rust twin
if [[ -f "$TB/rust/Cargo.toml" ]]; then
  echo "== Rust twin"
  ( cd "$TB/rust" && \
      LIBUV_DIR="$LIBUV_BUILD" LIBUV_INCLUDE="$UV_INC" cargo build --release )
  RS="$TB/rust/target/release/quickjs_lifetime_track_b"
  if [[ -x "$RS" ]]; then
    "$RS" "$SCRIPT" | tee "$BUILD/${DATE}_rust.jsonl"
    grep -q 'ORACLE PASS' "$BUILD/${DATE}_rust.jsonl" && \
      echo "oracle_rust=PASS" || echo "oracle_rust=FAIL"
  fi
fi

echo "oracle_cc=$oracle"
[[ "$oracle" == PASS ]]
