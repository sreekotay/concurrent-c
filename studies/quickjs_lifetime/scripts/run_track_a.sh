#!/usr/bin/env bash
# Build CC + Rust Track A drivers, run oracle, emit receipts under track_a/results/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STUDY="$ROOT/studies/quickjs_lifetime"
TA="$STUDY/track_a"
RESULTS="$TA/results"
DATE="$(date +%Y%m%d)"
SCALE="${HOSTILE_SCALE:-default}"
IMPL="${HOSTILE_IMPL:-epoch}"
export HOSTILE_IMPL="$IMPL"
TAG="${DATE}_${SCALE}_${IMPL}"
BUILD="$RESULTS/raw"
mkdir -p "$RESULTS" "$BUILD"

if [[ -z "${CC_QUICKJS_SRC:-}" ]]; then
  if [[ -d "$ROOT/quickjs" ]]; then
    export CC_QUICKJS_SRC="$ROOT/quickjs"
  elif [[ -d /tmp/quickjs-ng ]]; then
    export CC_QUICKJS_SRC=/tmp/quickjs-ng
  else
    echo "error: set CC_QUICKJS_SRC to pinned quickjs-ng (see PINNED.md)" >&2
    exit 2
  fi
fi

echo "== Track A: CC_QUICKJS_SRC=$CC_QUICKJS_SRC HOSTILE_SCALE=$SCALE HOSTILE_IMPL=$IMPL"

CCC="${CCC:-$ROOT/cc/bin/ccc}"
if [[ ! -x "$CCC" ]]; then
  CCC="$(command -v ccc || true)"
fi
if [[ -z "$CCC" ]]; then
  echo "error: ccc not found" >&2
  exit 2
fi

# --- build CC ---
cc -O2 -c "$ROOT/stress/mem_sample.c" -o "$BUILD/mem_sample.o"
"$CCC" build "$TA/cc/driver.ccs" \
  -o "$BUILD/driver_cc" \
  --ld-flags "$BUILD/mem_sample.o" \
  --cc-flags "-I$TA/cc -I$ROOT/stress"

# --- build Rust ---
( cd "$TA/rust" && cargo build --release )
DRIVER_RS="$TA/rust/target/release/quickjs_lifetime_track_a"

# --- run ---
CC_OUT="$BUILD/${TAG}_cc.jsonl"
RS_OUT="$BUILD/${TAG}_rust.jsonl"
"$BUILD/driver_cc" | tee "$CC_OUT"
"$DRIVER_RS" | tee "$RS_OUT"

normalize_chk() {
  # Strip rss (impl-dependent); keep epoch/phase/claims/weaks/regs/ok.
  grep '^CHK ' "$1" | sed -E 's/,"rss":[0-9]+//'
}

CC_NORM="$BUILD/${TAG}_cc.norm"
RS_NORM="$BUILD/${TAG}_rust.norm"
normalize_chk "$CC_OUT" >"$CC_NORM"
normalize_chk "$RS_OUT" >"$RS_NORM"

oracle_cc=FAIL
oracle_rs=FAIL
grep -q 'ORACLE PASS' "$CC_OUT" && oracle_cc=PASS
grep -q 'ORACLE PASS' "$RS_OUT" && oracle_rs=PASS

match=FAIL
if cmp -s "$CC_NORM" "$RS_NORM"; then
  match=PASS
fi

ccc_ver="$("$CCC" --version 2>/dev/null | head -1 || echo unknown)"
pin_sha="$(cd "$CC_QUICKJS_SRC" && git rev-parse HEAD 2>/dev/null || echo unknown)"
impl_note="$IMPL"

write_summary() {
  local name="$1" out="$2" oracle="$3"
  {
    echo "study: quickjs_lifetime Track A"
    echo "date: $DATE"
    echo "scale: $SCALE"
    echo "hostile_impl: $impl_note"
    echo "impl: $name"
    echo "ccc: $ccc_ver"
    echo "quickjs_src: $CC_QUICKJS_SRC"
    echo "quickjs_sha: $pin_sha"
    echo "rquickjs: 0.12.2"
    echo "oracle: $oracle"
    echo "--- PERF ---"
    grep '^PERF ' "$out" || true
    echo "--- last CHK ---"
    grep '^CHK ' "$out" | tail -3 || true
    echo "--- RSS peak (max in CHK) ---"
    grep '^CHK ' "$out" | sed -E 's/.*"rss":([0-9]+).*/\1/' | sort -n | tail -1 || true
  } >"$RESULTS/${TAG}_${name}.txt"
}

write_summary cc "$CC_OUT" "$oracle_cc"
write_summary rust "$RS_OUT" "$oracle_rs"

{
  echo "study: quickjs_lifetime Track A compare"
  echo "date: $DATE"
  echo "scale: $SCALE"
  echo "hostile_impl: $impl_note"
  echo "oracle_cc: $oracle_cc"
  echo "oracle_rust: $oracle_rs"
  echo "checkpoint_stream_match (sans rss): $match"
  echo "pinned_expected: df836d1f4490dfc6a65dbceda8a71d14ddc7f45c"
  echo "pinned_actual: $pin_sha"
  if [[ "$pin_sha" != "df836d1f4490dfc6a65dbceda8a71d14ddc7f45c" && "$pin_sha" != "unknown" ]]; then
    echo "note: engine SHA differs from PINNED.md — record in shape_review"
  fi
} >"$RESULTS/${TAG}_compare.txt"

echo ""
echo "== receipts: $RESULTS/${TAG}_{cc,rust,compare}.txt"
echo "oracle_cc=$oracle_cc oracle_rust=$oracle_rs stream_match=$match"

if [[ "$oracle_cc" != PASS || "$oracle_rs" != PASS || "$match" != PASS ]]; then
  echo "FAIL closed" >&2
  exit 1
fi
echo "ORACLE COMPARE PASS"
