#!/bin/sh
set -eu

# Smoke redis / pigz / levenshtein under the native front (default ccc).
# Requires native shadow_lower. Wired from test.sh --full when front=serdes;
# runnable standalone.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

fail() { echo "[serdes-real] FAIL: $*" >&2; exit 1; }

CCC="${CCC:-./out/cc/bin/ccc}"
[ -x "$CCC" ] || CCC="./cc/bin/ccc"
[ -x "$CCC" ] || fail "ccc not built"
[ -x ./out/cc/bin/shadow_lower ] || [ -x ./cc/bin/shadow_lower ] || \
  fail "native shadow_lower missing (make -C cc)"

export CC_FRONTEND=native

echo "[serdes-real] redis_idiomatic (build + functional smoke)"
"$CCC" --frontend=native build --no-cache \
  real_projects/redis/redis_idiomatic.ccs -o out/redis_idiomatic_serdes \
  || fail "redis_idiomatic build"
out="$(python3 real_projects/redis/redis_smoke.py \
        --server out/redis_idiomatic_serdes --port 0 2>&1)" || {
  echo "$out" | tail -50 >&2
  fail "redis_smoke.py"
}
case "$out" in
  *"SMOKE OK"*) ;;
  *) echo "$out" | tail -50 >&2; fail "redis smoke did not print SMOKE OK" ;;
esac
echo "[serdes-real] redis OK"

echo "[serdes-real] pigz_idiomatic"
"$CCC" --frontend=native build --no-cache -O \
  real_projects/pigz/pigz_idiomatic.ccs -o out/pigz_idiomatic_serdes \
  --ld-flags "-lz" --release \
  || fail "pigz_idiomatic build"
printf 'hello serdes pigz\n' > /tmp/pigz_serdes_in.txt
rm -f /tmp/pigz_serdes_in.txt.gz
./out/pigz_idiomatic_serdes /tmp/pigz_serdes_in.txt \
  || fail "pigz compress"
[ -f /tmp/pigz_serdes_in.txt.gz ] || fail "pigz did not write .gz"
if command -v gzip >/dev/null 2>&1; then
  gzip -t /tmp/pigz_serdes_in.txt.gz || fail "pigz output not valid gzip"
fi
sz="$(wc -c < /tmp/pigz_serdes_in.txt.gz | tr -d ' ')"
[ "$sz" -gt 10 ] || fail "pigz output too small ($sz)"
echo "[serdes-real] pigz_idiomatic OK (${sz} bytes)"

echo "[serdes-real] pigz_cc"
ZOPFLI_CFLAGS=""
ZOPFLI_LDFLAGS=""
if pkg-config libzopfli --exists 2>/dev/null; then
  ZOPFLI_CFLAGS="$(pkg-config --cflags libzopfli)"
  ZOPFLI_LDFLAGS="$(pkg-config --libs libzopfli)"
elif pkg-config zopfli --exists 2>/dev/null; then
  ZOPFLI_CFLAGS="$(pkg-config --cflags zopfli)"
  ZOPFLI_LDFLAGS="$(pkg-config --libs zopfli)"
elif [ -f /opt/homebrew/include/zopfli.h ]; then
  ZOPFLI_CFLAGS="-I/opt/homebrew/include"
  ZOPFLI_LDFLAGS="-L/opt/homebrew/lib -lzopfli"
fi
"$CCC" --frontend=native build --no-cache -O \
  real_projects/pigz/pigz_cc/pigz_cc.ccs -o out/pigz_cc_serdes \
  --ld-flags "-lz ${ZOPFLI_LDFLAGS}" \
  --cc-flags "${ZOPFLI_CFLAGS}" --release \
  || fail "pigz_cc build"
echo "[serdes-real] pigz_cc OK"

echo "[serdes-real] levenshtein (cclev.abi3.so)"
rm -f bin/cclev.abi3.so
"$CCC" --frontend=native build --no-cache -O \
  real_projects/levenshtein/levenshtein_cc.ccs \
  || fail "levenshtein build"
PYTHONPATH=bin python3 -c "import cclev; assert cclev.distance('kitten','sitting') == 3" \
  || fail "cclev import/distance"
echo "[serdes-real] levenshtein OK"

echo "[serdes-real] all OK"
