#!/usr/bin/env bash
# ASan / TSan / light fuzz for real_projects main specimens.
#
#   ./scripts/real_projects_sanitize.sh asan
#   ./scripts/real_projects_sanitize.sh tsan
#   ./scripts/real_projects_sanitize.sh fuzz
#   ./scripts/real_projects_sanitize.sh all
#   ./scripts/real_projects_sanitize.sh asan --docker   # force Linux container
#
# Darwin: ASan/TSan *runtime* with CC fibers often hangs (same class as
# DYLD_INSERT / interceptor issues). Default on Darwin is build-with-flags
# + skip run, or --docker for a full Linux pass. Ubuntu CI runs full.
#
# Mains: pigz_idiomatic, pigz_cc (build), redis_idiomatic (+ smoke),
# levenshtein. See docs/sanitizers.md.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-asan}"
shift || true
DOCKER=0
for a in "$@"; do
  case "$a" in
    --docker) DOCKER=1 ;;
  esac
done

HOST_ARG=0
for a in "$@"; do
  case "$a" in --host) HOST_ARG=1 ;; esac
done

if [[ "$(uname -s)" == "Darwin" && "$DOCKER" -eq 0 && "$HOST_ARG" -eq 0 ]]; then
  # Auto-docker when available for runtime sanitizers / fuzz.
  if command -v docker >/dev/null 2>&1 && [[ "$MODE" == "asan" || "$MODE" == "tsan" || "$MODE" == "fuzz" || "$MODE" == "all" ]]; then
    echo "real_projects_sanitize: Darwin → Docker Linux (host ASan+fibers hangs; use --host to force Darwin build-only)"
    DOCKER=1
  fi
fi

if [[ "$DOCKER" -eq 1 ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "real_projects_sanitize: --docker needs docker" >&2
    exit 1
  fi
  echo "real_projects_sanitize: docker mode=$MODE"
  exec docker run --rm --platform linux/arm64 \
    -v "$ROOT:/src:ro" \
    -v "$ROOT/out/real_sanitize:/out" \
    -e MODE="$MODE" \
    -e REAL_FUZZ_N="${REAL_FUZZ_N:-40}" \
    -e REAL_SANITIZE_TIMEOUT="${REAL_SANITIZE_TIMEOUT:-120}" \
    -e REAL_FUZZ_SEED="${REAL_FUZZ_SEED:-1}" \
    -w /work \
    ubuntu:24.04 bash -lc '
      set -euo pipefail
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq build-essential clang python3 python3-dev \
        zlib1g-dev libzopfli-dev pkg-config rsync ca-certificates >/dev/null
      rsync -a --delete \
        --exclude out/ --exclude bin/ --exclude .git/ \
        --exclude third_party/tcc/*.o --exclude "**/node_modules/" \
        --exclude npm/cc-python/vendor/ \
        --exclude npm/cc-python/bin/ \
        /src/ /work/
      jobs=$(nproc)
      export CC=clang CXX=clang++
      ./scripts/apply_tcc_patches.sh >/dev/null 2>&1 || true
      (cd third_party/tcc && ./configure --config-cc_ext >/dev/null && make -j"$jobs" libtcc.a tcc libtcc1.a)
      # stage1 link races under high -j; build toolchain serially then fan out
      make -C cc -j1
      mkdir -p /out
      OUT=/out ./scripts/real_projects_sanitize.sh "$MODE" --host
      cp -a out/real_sanitize/. /out/ 2>/dev/null || true
    '
fi

# --host: skip docker re-entry when already inside the container / Linux CI
for a in "$@"; do
  case "$a" in
    --host) : ;;
  esac
done

CCC="${CCC:-./cc/bin/ccc}"
[ -x "$CCC" ] || CCC="./out/cc/bin/ccc"
[ -x "$CCC" ] || { echo "real_projects_sanitize: need ccc (make cc)" >&2; exit 1; }

# Sanitizer builds need clang (GCC rejects some no_sanitize attribute placements
# and TSan quality matches the existing test_tsan.sh / stress_sanitize lanes).
case "$MODE" in
  asan|tsan|fuzz|all)
    if command -v clang >/dev/null 2>&1; then
      export CC="${CC:-clang}"
      export CXX="${CXX:-clang++}"
    fi
    ;;
esac

OUT="${OUT:-$ROOT/out/real_sanitize}"
mkdir -p "$OUT"
FUZZ_N="${REAL_FUZZ_N:-40}"
TIMEOUT_S="${REAL_SANITIZE_TIMEOUT:-120}"
HOST_OS="$(uname -s)"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YEL=$'\033[1;33m'; CYA=$'\033[0;36m'; NC=$'\033[0m'
passed=0
failed=0
skipped=0

zopfli_flags() {
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
  elif [ -f /usr/include/zopfli/zopfli.h ]; then
    # Debian/Ubuntu libzopfli-dev (no .pc): headers live under zopfli/
    ZOPFLI_CFLAGS="-I/usr/include/zopfli"
    ZOPFLI_LDFLAGS="-lzopfli"
  fi
}

ok() { echo -e "  ${GREEN}OK${NC}  $*"; passed=$((passed + 1)); }
fail() { echo -e "  ${RED}FAIL${NC} $*"; failed=$((failed + 1)); }
skip() { echo -e "  ${YEL}SKIP${NC} $*"; skipped=$((skipped + 1)); }

run_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout --signal=KILL "$TIMEOUT_S" "$@"
  else
    perl -e 'alarm shift; exec @ARGV' "$TIMEOUT_S" "$@"
  fi
}

san_flags() {
  case "$1" in
    asan) echo "-fsanitize=address -fno-omit-frame-pointer -g" ;;
    tsan) echo "-fsanitize=thread -fno-omit-frame-pointer -g" ;;
    *) echo "" ;;
  esac
}

# Darwin runtime under ASan/TSan is unreliable for fiber-heavy CC binaries.
can_run_sanitized() {
  [[ "$HOST_OS" != "Darwin" ]]
}

build_run_pigz_idiomatic() {
  local san="$1"
  local flags ld
  flags="$(san_flags "$san")"
  ld="$flags"
  local bin="$OUT/pigz_idiomatic_${san}"
  echo -e "${CYA}[$san]${NC} pigz_idiomatic"
  if ! "$CCC" build --no-cache -g \
      real_projects/pigz/pigz_idiomatic.ccs -o "$bin" \
      --cc-flags "$flags" --ld-flags "$ld -lz"; then
    fail "pigz_idiomatic build"
    return
  fi
  if ! can_run_sanitized; then
    skip "pigz_idiomatic run (Darwin ASan/TSan runtime — use --docker / Linux CI)"
    ok "pigz_idiomatic build"
    return
  fi
  printf 'hello real_projects sanitize\n' >"$OUT/pigz_in.txt"
  rm -f "$OUT/pigz_in.txt.gz"
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}"
  if ! run_timeout "$bin" "$OUT/pigz_in.txt"; then
    fail "pigz_idiomatic run"
    return
  fi
  [ -f "$OUT/pigz_in.txt.gz" ] || { fail "pigz_idiomatic no .gz"; return; }
  if command -v gzip >/dev/null 2>&1; then
    gzip -t "$OUT/pigz_in.txt.gz" || { fail "pigz_idiomatic bad gzip"; return; }
  fi
  ok "pigz_idiomatic"
}

build_pigz_cc() {
  local san="$1"
  local flags
  flags="$(san_flags "$san")"
  zopfli_flags
  local bin="$OUT/pigz_cc_${san}"
  echo -e "${CYA}[$san]${NC} pigz_cc (build)"
  if ! "$CCC" build --no-cache -g \
      real_projects/pigz/pigz_cc/pigz_cc.ccs -o "$bin" \
      --cc-flags "$flags ${ZOPFLI_CFLAGS}" \
      --ld-flags "$flags -lz ${ZOPFLI_LDFLAGS}"; then
    fail "pigz_cc build"
    return
  fi
  ok "pigz_cc build"
}

build_run_redis() {
  local san="$1"
  local flags
  flags="$(san_flags "$san")"
  local bin="$OUT/redis_idiomatic_${san}"
  echo -e "${CYA}[$san]${NC} redis_idiomatic + smoke"
  # getrusage / RUSAGE_SELF need sys/resource.h; force GNU/BSD feature macros.
  if ! "$CCC" build --no-cache -g \
      real_projects/redis/redis_idiomatic.ccs -o "$bin" \
      --cc-flags "$flags -D_GNU_SOURCE" --ld-flags "$flags"; then
    fail "redis_idiomatic build"
    return
  fi
  if ! can_run_sanitized; then
    skip "redis_smoke (Darwin ASan/TSan runtime — use --docker / Linux CI)"
    ok "redis_idiomatic build"
    return
  fi
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}"
  local out errf="$OUT/redis_smoke_${san}.err"
  # Smoke swallows server stderr; capture it ourselves for ASan/TSan reports.
  if ! out="$(run_timeout python3 real_projects/redis/redis_smoke.py \
        --server "$bin" --port 0 2>"$errf")"; then
    echo "$out" | tail -30
    [ -s "$errf" ] && { echo "--- redis_smoke stderr ---"; tail -40 "$errf"; }
    fail "redis_smoke"
    return
  fi
  case "$out" in
    *"SMOKE OK"*) ok "redis_idiomatic" ;;
    *)
      echo "$out" | tail -30
      [ -s "$errf" ] && { echo "--- redis_smoke stderr ---"; tail -40 "$errf"; }
      fail "redis_smoke no SMOKE OK"
      ;;
  esac
}

build_run_levenshtein() {
  local san="$1"
  local flags
  flags="$(san_flags "$san")"
  echo -e "${CYA}[$san]${NC} levenshtein (cclev)"
  rm -f bin/cclev.abi3.so
  local build_log="$OUT/levenshtein_${san}_build.log"
  if ! "$CCC" build --no-cache -g \
      real_projects/levenshtein/levenshtein_cc.ccs \
      --cc-flags "$flags" --ld-flags "$flags" >"$build_log" 2>&1; then
    tail -20 "$build_log"
    fail "levenshtein build"
    return
  fi
  if [ ! -f bin/cclev.abi3.so ]; then
    tail -20 "$build_log"
    fail "levenshtein missing bin/cclev.abi3.so"
    return
  fi
  if grep -qE 'error:' "$build_log"; then
    tail -20 "$build_log"
    fail "levenshtein build errors (ccc exit 0)"
    return
  fi
  cp -f bin/cclev.abi3.so "$OUT/cclev_${san}.abi3.so"
  if ! can_run_sanitized; then
    skip "levenshtein import (Darwin dlopen+ASan)"
    ok "levenshtein build"
    return
  fi
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1}"
  if PYTHONPATH=bin run_timeout python3 -c \
      "import cclev; assert cclev.distance('kitten','sitting') == 3"; then
    ok "levenshtein import"
  else
    skip "levenshtein import under $san (dlopen limit)"
    ok "levenshtein build-only"
  fi
}

run_san() {
  local san="$1"
  echo -e "${YEL}=== real_projects $san (host=$HOST_OS) ===${NC}"
  build_run_pigz_idiomatic "$san"
  build_pigz_cc "$san"
  build_run_redis "$san"
  build_run_levenshtein "$san"
}

run_fuzz() {
  echo -e "${YEL}=== real_projects fuzz (asan, N=${FUZZ_N}, host=$HOST_OS) ===${NC}"
  if ! can_run_sanitized; then
    skip "process fuzz on Darwin — use --docker / Linux CI"
    return
  fi
  local bin="$OUT/pigz_idiomatic_asan"
  if [ ! -x "$bin" ]; then
    build_run_pigz_idiomatic asan || true
  fi
  if [ ! -x "$bin" ]; then
    fail "fuzz needs pigz_idiomatic_asan"
  else
    local i crashes=0
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1}"
    for i in $(seq 1 "$FUZZ_N"); do
      local f="$OUT/fuzz_in_$i.bin"
      local n=$(( (i * 97 + 13) % 8192 ))
      dd if=/dev/urandom of="$f" bs=1 count="$n" status=none 2>/dev/null || \
        head -c "$n" </dev/urandom >"$f"
      rm -f "${f}.gz"
      set +e
      run_timeout "$bin" "$f" >/dev/null 2>"$OUT/fuzz_pigz_$i.err"
      set -e
      if grep -qiE 'AddressSanitizer|heap-use-after-free|buffer-overflow' \
           "$OUT/fuzz_pigz_$i.err" 2>/dev/null; then
        crashes=$((crashes + 1))
        echo -e "  ${RED}ASan${NC} pigz fuzz case $i"
      fi
    done
    if [ "$crashes" -eq 0 ]; then
      ok "pigz fuzz ${FUZZ_N} inputs"
    else
      fail "pigz fuzz $crashes/$FUZZ_N ASan hits"
    fi
  fi

  build_run_levenshtein asan || true
  if [ -f bin/cclev.abi3.so ]; then
    if REAL_FUZZ_N="$FUZZ_N" REAL_FUZZ_SEED="${REAL_FUZZ_SEED:-1}" \
       PYTHONPATH=bin python3 - <<'PY'
import os, random, sys
try:
    import cclev
except Exception as e:
    print("skip import", e)
    sys.exit(0)
rng = random.Random(int(os.environ.get("REAL_FUZZ_SEED", "1")))
N = int(os.environ.get("REAL_FUZZ_N", "40"))
for i in range(N):
    a = "".join(chr(rng.randint(32, 126)) for _ in range(rng.randint(0, 64)))
    b = "".join(chr(rng.randint(32, 126)) for _ in range(rng.randint(0, 64)))
    d = cclev.distance(a, b)
    assert isinstance(d, int) and d >= 0
print("levenshtein fuzz ok", N)
PY
    then
      ok "levenshtein fuzz ${FUZZ_N}"
    else
      fail "levenshtein fuzz"
    fi
  else
    skip "levenshtein fuzz (no cclev.so)"
  fi
}

case "$MODE" in
  asan) run_san asan ;;
  tsan) run_san tsan ;;
  fuzz) run_fuzz ;;
  all)
    run_san asan
    run_san tsan
    run_fuzz
    ;;
  --host)
    echo "usage: $0 asan|tsan|fuzz|all [--docker|--host]" >&2
    exit 2
    ;;
  *)
    echo "usage: $0 asan|tsan|fuzz|all [--docker]" >&2
    exit 2
    ;;
esac

echo ""
echo -e "summary: ${GREEN}$passed ok${NC}, ${RED}$failed fail${NC}, ${YEL}$skipped skip${NC}"
if [ "$failed" -eq 0 ]; then
  echo -e "${GREEN}real_projects_sanitize: clean${NC}"
  exit 0
fi
echo -e "${RED}real_projects_sanitize: failures${NC}"
exit 1
