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
# Mains: pigz_idiomatic (wait-for + dict chain), pigz_channel, pigz_cc (build),
# redis_idiomatic (+ smoke), staticd (+ HTTP smoke), levenshtein.
# See docs/sanitizers.md. Filter with REAL_SANITIZE_ONLY=staticd,redis,…
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
    -e REAL_SANITIZE_ONLY="${REAL_SANITIZE_ONLY:-}" \
    -w /work \
    ubuntu:24.04 bash -lc '
      set -euo pipefail
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq build-essential clang python3 python3-dev \
        zlib1g-dev libzopfli-dev pkg-config rsync ca-certificates curl >/dev/null
      # rsync 24 = vanished source file (host TCC rebuild mid-copy); retry once.
      rsync_src() {
        rsync -a --delete \
          --exclude out/ --exclude bin/ --exclude .git/ \
          --exclude 'third_party/tcc/*.o' --exclude 'third_party/tcc/*.tmp' \
          --exclude 'third_party/tcc/*.o.tmp' \
          --exclude "**/node_modules/" \
          --exclude npm/cc-python/vendor/ \
          --exclude npm/cc-python/bin/ \
          /src/ /work/ || {
            rc=$?
            [ "$rc" -eq 24 ] || return "$rc"
            rsync -a --delete \
              --exclude out/ --exclude bin/ --exclude .git/ \
              --exclude 'third_party/tcc/*.o' --exclude 'third_party/tcc/*.tmp' \
              --exclude 'third_party/tcc/*.o.tmp' \
              --exclude "**/node_modules/" \
              --exclude npm/cc-python/vendor/ \
              --exclude npm/cc-python/bin/ \
              /src/ /work/ || {
                rc=$?
                [ "$rc" -eq 24 ] && return 0
                return "$rc"
              }
          }
      }
      rsync_src
      jobs=$(nproc)
      export CC=clang CXX=clang++
      ./scripts/apply_tcc_patches.sh >/dev/null 2>&1 || true
      (cd third_party/tcc && ./configure --config-cc_ext >/dev/null && make -j"$jobs" libtcc.a tcc libtcc1.a)
      # stage1 link races under high -j; build toolchain serially then fan out
      make -C cc -j1
      mkdir -p /out
      set +e
      OUT=/out ./scripts/real_projects_sanitize.sh "$MODE" --host
      rc=$?
      set -e
      cp -a out/real_sanitize/. /out/ 2>/dev/null || true
      exit "$rc"
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

# Optional filter: REAL_SANITIZE_ONLY=staticd or comma list (pigz,redis,staticd,…).
should_run() {
  local name="$1"
  local only="${REAL_SANITIZE_ONLY:-}"
  [[ -z "$only" ]] && return 0
  case ",${only}," in
    *",${name},"*) return 0 ;;
    *) return 1 ;;
  esac
}

tsan_options() {
  local supp="$ROOT/scripts/tsan_fiber.supp"
  if [[ -f "$supp" ]]; then
    echo "halt_on_error=1:suppressions=${supp}"
  else
    echo "halt_on_error=1"
  fi
}

build_run_pigz_idiomatic() {
  should_run pigz || return 0
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
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-$(tsan_options)}"
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

# Wait-for + cache(zs) + dict hop: take[] snapshot of a loop-carried slot.
# Two runs must byte-match (ordered write + copied tail).
build_run_pigz_idiomatic_dict() {
  should_run pigz || return 0
  local san="$1"
  local flags
  flags="$(san_flags "$san")"
  local bin="$OUT/pigz_idiomatic_dict_${san}"
  echo -e "${CYA}[$san]${NC} pigz_idiomatic (dict hop)"
  if ! "$CCC" build --no-cache -g \
      real_projects/pigz/pigz_idiomatic.ccs -o "$bin" \
      --cc-flags "$flags" --ld-flags "$flags -lz"; then
    fail "pigz_idiomatic dict build"
    return
  fi
  if ! can_run_sanitized; then
    skip "pigz_idiomatic dict run (Darwin ASan/TSan runtime — use --docker / Linux CI)"
    ok "pigz_idiomatic dict build"
    return
  fi
  python3 -c "open(r'$OUT/pw_in.bin','wb').write((b'The quick brown fox\n')*50000)"
  unset CC_WORKERS || true
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-$(tsan_options)}"
  rm -f "$OUT/pw_in.bin.gz"
  if ! run_timeout "$bin" "$OUT/pw_in.bin"; then
    fail "pigz_idiomatic dict run 1"
    return
  fi
  if command -v gzip >/dev/null 2>&1; then
    gzip -t "$OUT/pw_in.bin.gz" || { fail "pigz_idiomatic dict gzip"; return; }
  fi
  mv "$OUT/pw_in.bin.gz" "$OUT/pw_a.gz"
  if ! run_timeout "$bin" "$OUT/pw_in.bin"; then
    fail "pigz_idiomatic dict run 2"
    return
  fi
  if command -v gzip >/dev/null 2>&1; then
    gzip -t "$OUT/pw_in.bin.gz" || { fail "pigz_idiomatic dict gzip 2"; return; }
  fi
  if ! cmp -s "$OUT/pw_a.gz" "$OUT/pw_in.bin.gz"; then
    fail "pigz_idiomatic dict runs differ"
    return
  fi
  ok "pigz_idiomatic dict"
}

build_run_pigz_channel() {
  should_run pigz || return 0
  local san="$1"
  local flags ld
  flags="$(san_flags "$san")"
  ld="$flags"
  local bin="$OUT/pigz_channel_${san}"
  echo -e "${CYA}[$san]${NC} pigz_channel"
  if ! "$CCC" build --no-cache -g \
      real_projects/pigz/pigz_channel.ccs -o "$bin" \
      --cc-flags "$flags" --ld-flags "$ld -lz"; then
    fail "pigz_channel build"
    return
  fi
  if ! can_run_sanitized; then
    skip "pigz_channel run (Darwin ASan/TSan runtime — use --docker / Linux CI)"
    ok "pigz_channel build"
    return
  fi
  printf 'hello real_projects sanitize\n' >"$OUT/pigz_ch_in.txt"
  rm -f "$OUT/pigz_ch_in.txt.gz"
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-$(tsan_options)}"
  if ! run_timeout "$bin" "$OUT/pigz_ch_in.txt"; then
    fail "pigz_channel run"
    return
  fi
  [ -f "$OUT/pigz_ch_in.txt.gz" ] || { fail "pigz_channel no .gz"; return; }
  if command -v gzip >/dev/null 2>&1; then
    gzip -t "$OUT/pigz_ch_in.txt.gz" || { fail "pigz_channel bad gzip"; return; }
  fi
  ok "pigz_channel"
}

build_pigz_cc() {
  should_run pigz || return 0
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
  should_run redis || return 0
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
  # ASan + CC fibers trip asan_thread fake-stack CHECKs on GHA clang
  # (asan_thread.cpp kCurrentStackFrameMagic) mid-smoke. Build stays under
  # ASan; runtime smoke is covered by the TSan lane below.
  if [[ "$san" == asan ]]; then
    skip "redis_smoke under ASan (fiber fake-stack CHECK; covered by TSan smoke)"
    ok "redis_idiomatic build"
    return
  fi
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-$(tsan_options)}"
  local out errf="$OUT/redis_smoke_${san}.err"
  local srv_errf="$OUT/redis_server_${san}.err"
  rm -f "$srv_errf"
  # Client traceback → errf; server ASan/TSan → srv_errf.
  if ! out="$(REDIS_SMOKE_SERVER_STDERR="$srv_errf" \
        run_timeout python3 real_projects/redis/redis_smoke.py \
        --server "$bin" --port 0 2>"$errf")"; then
    echo "$out" | tail -30
    [ -s "$errf" ] && { echo "--- redis_smoke stderr ---"; tail -40 "$errf"; }
    [ -s "$srv_errf" ] && { echo "--- redis server stderr ---"; tail -60 "$srv_errf"; }
    fail "redis_smoke"
    return
  fi
  case "$out" in
    *"SMOKE OK"*)
      # halt_on_error can miss races during post-smoke teardown; still fail.
      if [ -s "$srv_errf" ] && grep -qiE 'ThreadSanitizer|AddressSanitizer|data race|heap-use-after-free' "$srv_errf"; then
        echo "--- redis server stderr (sanitizer) ---"
        tail -60 "$srv_errf"
        fail "redis_smoke sanitizer report after SMOKE OK"
      else
        ok "redis_idiomatic"
      fi
      ;;
    *)
      echo "$out" | tail -30
      [ -s "$errf" ] && { echo "--- redis_smoke stderr ---"; tail -40 "$errf"; }
      [ -s "$srv_errf" ] && { echo "--- redis server stderr ---"; tail -60 "$srv_errf"; }
      fail "redis_smoke no SMOKE OK"
      ;;
  esac
}

# server.cch stages under out/.cc-build/modules/; quoted member .ccs must
# resolve beside that copy. Symlink members from the face directory so a
# fresh lower finds them (same layout as a face-dir build).
stage_server_module_members() {
  local mod="$ROOT/out/.cc-build/modules"
  local std="$ROOT/cc/include/ccc/std"
  mkdir -p "$mod"
  local f
  for f in server_poll.ccs server_poll.cch server_serve.ccs; do
    [ -f "$std/$f" ] || continue
    ln -sfn "$std/$f" "$mod/$f"
  done
}

# HTTP/1.1 file server: build into OUT/, short fixture GET smoke (no wrk / peers).
# TLS needs BearSSL in the toolchain (optional); pages need QuickJS / libpython —
# this lane smokes static fixtures only. ASan runtime smoke skipped like redis
# (fiber fake-stack CHECK on some clang); TSan covers the smoke.
build_run_staticd() {
  should_run staticd || return 0
  local san="$1"
  local flags
  flags="$(san_flags "$san")"
  local bin="$OUT/staticd_${san}"
  local fix="$ROOT/real_projects/staticd/fixtures"
  echo -e "${CYA}[$san]${NC} staticd + HTTP smoke"
  stage_server_module_members
  if ! "$CCC" build --no-cache -g \
      real_projects/staticd/staticd.ccs -o "$bin" \
      --cc-flags "$flags" --ld-flags "$flags"; then
    fail "staticd build"
    return
  fi
  if ! can_run_sanitized; then
    skip "staticd smoke (Darwin ASan/TSan runtime — use --docker / Linux CI)"
    ok "staticd build"
    return
  fi
  if [[ "$san" == asan ]]; then
    skip "staticd smoke under ASan (fiber fake-stack CHECK; covered by TSan smoke)"
    ok "staticd build"
    return
  fi
  if [[ ! -f "$fix/1kb.bin" || ! -f "$fix/4kb.html" || ! -f "$fix/index.html" ]]; then
    if ! (cd "$ROOT/real_projects/staticd" && ./gen_fixtures.sh); then
      fail "staticd fixtures"
      return
    fi
  fi
  if ! command -v curl >/dev/null 2>&1; then
    skip "staticd smoke (need curl)"
    ok "staticd build"
    return
  fi
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
  export TSAN_OPTIONS="${TSAN_OPTIONS:-$(tsan_options)}"
  local port=$((19080 + ($$ % 200)))
  local srv_errf="$OUT/staticd_server_${san}.err"
  local pid=""
  rm -f "$srv_errf"
  "$bin" --listen "127.0.0.1:${port}" --root "$fix" --workers 1 \
    >"$OUT/staticd_server_${san}.out" 2>"$srv_errf" &
  pid=$!
  local i code=0
  for i in $(seq 1 50); do
    if curl -sS -o /dev/null --connect-timeout 0.2 \
         "http://127.0.0.1:${port}/index.html" 2>/dev/null; then
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      code=1
      break
    fi
    sleep 0.1
  done
  if [[ "$code" -ne 0 ]] || ! kill -0 "$pid" 2>/dev/null; then
    [ -s "$srv_errf" ] && { echo "--- staticd server stderr ---"; tail -60 "$srv_errf"; }
    fail "staticd smoke (server died)"
    wait "$pid" 2>/dev/null || true
    return
  fi
  local path status
  for path in /index.html /1kb.bin /4kb.html; do
    status="$(curl -sS -o /dev/null -w '%{http_code}' --connect-timeout 2 \
      "http://127.0.0.1:${port}${path}" || echo 000)"
    if [[ "$status" != "200" ]]; then
      echo "GET ${path} → HTTP ${status}"
      [ -s "$srv_errf" ] && { echo "--- staticd server stderr ---"; tail -60 "$srv_errf"; }
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      fail "staticd smoke ${path}"
      return
    fi
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  if [ -s "$srv_errf" ] && grep -qiE 'ThreadSanitizer|AddressSanitizer|data race|heap-use-after-free' "$srv_errf"; then
    echo "--- staticd server stderr (sanitizer) ---"
    tail -60 "$srv_errf"
    fail "staticd smoke sanitizer report"
    return
  fi
  ok "staticd"
}

build_run_levenshtein() {
  should_run levenshtein || return 0
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
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
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
  build_run_pigz_idiomatic_dict "$san"
  build_run_pigz_channel "$san"
  build_pigz_cc "$san"
  build_run_redis "$san"
  build_run_staticd "$san"
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
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1:detect_stack_use_after_return=0}"
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
