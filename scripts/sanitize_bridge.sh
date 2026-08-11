#!/usr/bin/env bash
# Sanitizer-load the concurrent-c-python addon inside Linux Docker and run
# bridge smokes. macOS SIP cannot DYLD_INSERT ASan/TSan into stock Node —
# this is the supported runtime path. See docs/sanitizers.md.
#
#   ./scripts/sanitize_bridge.sh           # mem + neg (ASan)
#   ./scripts/sanitize_bridge.sh fuzz      # mem + seeded walk (FUZZ_SEED / FUZZ_OPS)
#   ./scripts/sanitize_bridge.sh chaos     # + CHAOS_SCALE=quick
#   ./scripts/sanitize_bridge.sh mem       # mem only
#   ./scripts/sanitize_bridge.sh tsan      # mem under TSan (Node-noise suppressions)
#   ./scripts/sanitize_bridge.sh tsan-fuzz # mem + seeded fuzz under TSan
#   ./scripts/sanitize_bridge.sh bisect    # ordered mem-rung prefixes (ASan SEGV hunt)
#
# Progress: every step prints [+elapsed UTC]; long node runs emit a
# heartbeat every HEARTBEAT_SECS (default 15) with the last log line so a
# hang is obvious. Per-suite wall timeout: SANITIZE_BRIDGE_TIMEOUT
# (default 180s) via timeout(1) → exit 124/137.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

MODE="${1:-memneg}"
HEARTBEAT_SECS="${HEARTBEAT_SECS:-15}"
SUITE_TIMEOUT="${SANITIZE_BRIDGE_TIMEOUT:-180}"
IMAGE="${SANITIZE_BRIDGE_IMAGE:-node:20-bookworm}"

if ! command -v docker >/dev/null 2>&1; then
  echo "sanitize_bridge: need docker" >&2
  exit 1
fi

host_ts() { date -u '+%Y-%m-%dT%H:%M:%SZ'; }
echo "[$(host_ts)] sanitize_bridge: mode=$MODE image=$IMAGE timeout=${SUITE_TIMEOUT}s heartbeat=${HEARTBEAT_SECS}s"

# Vendor C is regenerate-only (prepare-publish / local). CI checkouts lack it,
# and the container mounts the repo RO — emit on the host before docker.
ensure_vendor() {
  local need=0
  if [ ! -f npm/cc-python/vendor/cc_python.c ]; then need=1; fi
  if [ ! -d npm/cc-python/vendor/include ]; then need=1; fi
  if [ ! -d npm/cc-python/vendor/runtime ]; then need=1; fi
  if [ "$need" -eq 0 ]; then
    echo "[$(host_ts)] vendor tree present — skip emit"
    return 0
  fi
  echo "[$(host_ts)] preparing npm/cc-python/vendor (emit + includes)…"
  if [ ! -f third_party/tcc/configure ]; then
    git submodule update --init third_party/tcc
    if [ ! -f third_party/tcc/configure ]; then
      git submodule update --checkout --force third_party/tcc
    fi
  fi
  if [ ! -x ./cc/bin/ccc ] && [ ! -x ./cc/bin/.ccc-bin ]; then
    make -C cc -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  fi
  mkdir -p npm/cc-python/vendor
  ./cc/bin/ccc build --emit-c-only npm/cc-python/src/cc_python.ccs \
    -o npm/cc-python/vendor/cc_python.c
  rm -rf npm/cc-python/vendor/include npm/cc-python/vendor/runtime
  cp -rL out/include npm/cc-python/vendor/include
  cp -rL out/runtime npm/cc-python/vendor/runtime
  cp -rL cc/include/ccc/vendor npm/cc-python/vendor/include/ccc/vendor
  echo "[$(host_ts)] vendor ready"
}
ensure_vendor

echo "[$(host_ts)] starting docker (first image pull can take a minute)…"

INNER=$(mktemp)
trap 'rm -f "$INNER"' EXIT
cat >"$INNER" <<'INNER'
#!/bin/bash
set -euo pipefail
MODE="$1"
HEARTBEAT_SECS="$2"
SUITE_TIMEOUT="$3"
T0=$(date +%s)

ts() {
  local now elapsed
  now=$(date +%s)
  elapsed=$((now - T0))
  printf '[+%4ds %s] ' "$elapsed" "$(date -u '+%H:%M:%S')"
}
step() { ts; echo ">>> $*"; }
ok()   { ts; echo "OK  $*"; }
fail() { ts; echo "FAIL $*" >&2; }

# Run a suite with heartbeat + hard timeout. Heartbeats show last log line.
# Always LD_PRELOAD the sanitizer runtime for the suite command only.
run_suite() {
  local name="$1"; shift
  local log="/tmp/sanitize_bridge_${name}.log"
  local t_suite
  t_suite=$(date +%s)
  step "start suite=$name timeout=${SUITE_TIMEOUT}s cmd: $*"
  : >"$log"

  (
    while true; do
      sleep "$HEARTBEAT_SECS" || exit 0
      [ -f "$log" ] || exit 0
      local age last nlines
      age=$(( $(date +%s) - t_suite ))
      nlines=$(wc -l <"$log" | tr -d ' ')
      last=$(tail -n 1 "$log" 2>/dev/null | tr -d '\r' | head -c 140)
      ts
      echo "... alive suite=$name elapsed=${age}s lines=${nlines} last=${last:-"(no output yet)"}"
    done
  ) &
  local hb_pid=$!

  # Stream suite stdout/err live (prefixed) while also appending to $log
  # for heartbeats. Heartbeat lines go to stderr so they don't enter $log.
  set +e
  if command -v timeout >/dev/null 2>&1; then
    timeout --signal=KILL "$SUITE_TIMEOUT" \
      env LD_PRELOAD="$SAN_LD_PRELOAD" "$@" \
      > >(tee -a "$log" | sed -u 's/^/    | /') \
      2> >(tee -a "$log" | sed -u 's/^/    ! /' >&2)
    local st=$?
  else
    env LD_PRELOAD="$SAN_LD_PRELOAD" "$@" \
      > >(tee -a "$log" | sed -u 's/^/    | /') \
      2> >(tee -a "$log" | sed -u 's/^/    ! /' >&2)
    local st=$?
  fi
  set -e

  kill "$hb_pid" 2>/dev/null || true
  wait "$hb_pid" 2>/dev/null || true
  # Drain process substitutions
  sleep 0.2 2>/dev/null || true

  local age_done
  age_done=$(( $(date +%s) - t_suite ))

  if [ "$st" -eq 124 ] || [ "$st" -eq 137 ]; then
    fail "suite=$name TIMEOUT after ${SUITE_TIMEOUT}s (exit $st) — hung or too slow"
    return 1
  fi
  if [ "$st" -eq 139 ]; then
    fail "suite=$name SEGV (exit 139) after ${age_done}s — see last | lines above"
    return 139
  fi
  if [ "$st" -ne 0 ]; then
    fail "suite=$name exit=$st after ${age_done}s"
    return "$st"
  fi
  ok "suite=$name exit=0 (${age_done}s)"
  return 0
}

step "apt-get update"
apt-get update -qq >/tmp/apt-update.log 2>&1
ok "apt update done"

step "apt-get install clang python3 python3-dev libpython3-dev (quiet; can take ~30s)"
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  clang python3 python3-dev libpython3-dev >/tmp/apt-install.log 2>&1
ok "packages installed"

# Bookworm ships both i386 and x86_64 runtimes; `find | head` picks i386
# first on amd64 and LD_PRELOAD silently fails (wrong ELF class).
host_arch=$(uname -m)
case "$host_arch" in
  x86_64|amd64) san_arch=x86_64 ;;
  aarch64|arm64) san_arch=aarch64 ;;
  i386|i686) san_arch=i386 ;;
  *) san_arch="$host_arch" ;;
esac

SAN_KIND=asan
case "$MODE" in
  tsan|tsan-fuzz) SAN_KIND=tsan ;;
esac

step "locate ${SAN_KIND} runtime (arch=$san_arch)"
SAN_SO=$(find /usr/lib/llvm-*/lib/clang/*/lib/linux \
  -name "libclang_rt.${SAN_KIND}-${san_arch}.so" 2>/dev/null | head -1)
test -n "$SAN_SO" && test -f "$SAN_SO"
ok "SAN_SO=$SAN_SO"
SAN_LIBDIR=$(dirname "$SAN_SO")
export LD_LIBRARY_PATH="${SAN_LIBDIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ "$SAN_KIND" = tsan ]; then
  FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g -O1 -fPIC'
  LINK_SAN='-fsanitize=thread'
  ADDON_OUT=/tmp/cc_python_tsan.node
  # Node / libuv / V8 / CPython races are out of scope; keep our addon+runtime.
  SUPP=/tmp/cc_python_tsan.supp
  cat >"$SUPP" <<'SUPP'
# Stock Node / V8 / libuv / CPython races are out of scope for the addon gate.
# Keep reports whose stacks stay in our .node / runtime.
race:^node::
race:^v8::
race:^uv_
race:napi_
called_from_lib:libpython3*
called_from_lib:libnode*
SUPP
  export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:report_thread_leaks=0:suppressions=$SUPP}"
else
  FLAGS='-fsanitize=address -shared-libasan -fno-omit-frame-pointer -g -O1 -fPIC'
  LINK_SAN='-fsanitize=address -shared-libasan'
  ADDON_OUT=/tmp/cc_python_asan.node
  # Fiber stack switches break ASan fake-stack / SUAR bookkeeping.
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0:abort_on_error=0:detect_stack_use_after_return=0}"
fi

step "compile vendor/cc_python.c ($SAN_KIND)"
# shellcheck disable=SC2086
clang $FLAGS -Inpm/cc-python/vendor/include \
  -c npm/cc-python/vendor/cc_python.c -o /tmp/tu.o
ok "tu.o"

step "compile vendor/runtime/concurrent_c.c ($SAN_KIND)"
# shellcheck disable=SC2086
clang $FLAGS -DCC_ENABLE_ASYNC -Inpm/cc-python/vendor/include \
  -c npm/cc-python/vendor/runtime/concurrent_c.c -o /tmp/rt.o
ok "rt.o"

step "link $(basename "$ADDON_OUT")"
# shellcheck disable=SC2086
clang $LINK_SAN -shared \
  -o "$ADDON_OUT" /tmp/tu.o /tmp/rt.o -lpthread -lm -ldl
ok "addon bytes=$(wc -c <"$ADDON_OUT")"

export CC_PYTHON_ADDON="$ADDON_OUT"
export OPENBLAS_NUM_THREADS=1
# Do not export LD_PRELOAD globally — TSan aborts foreign tools (ls/timeout).
# Suites wrap node with env LD_PRELOAD=…
export SAN_LD_PRELOAD="$SAN_SO"
CC_LIBPYTHON=$(ls /usr/lib/*/libpython3.*.so | head -1)
export CC_LIBPYTHON
ok "CC_LIBPYTHON=$CC_LIBPYTHON SAN_KIND=$SAN_KIND LD_PRELOAD_FOR_NODE=$SAN_LD_PRELOAD"


rc=0
run_one() {
  if ! run_suite "$@"; then rc=1; fi
}

# TSan main must be instrumented — stock Node + LD_PRELOAD=libtsan SEGV at
# __cxa_atexit. Prove the addon links/loads under a TSan host instead.
run_tsan_dlopen_gate() {
  step "TSan dlopen gate (instrumented host + addon .node)"
  cat >/tmp/cc_python_tsan_host.c <<'HOST'
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
  void *h = dlopen("/tmp/cc_python_tsan.node", RTLD_NOW);
  if (!h) {
    fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  printf("tsan_dlopen_ok\n");
  dlclose(h);
  return 0;
}
HOST
  # shellcheck disable=SC2086
  clang -fsanitize=thread -fno-omit-frame-pointer -g -O1 \
    /tmp/cc_python_tsan_host.c -o /tmp/cc_python_tsan_host -ldl
  set +e
  /tmp/cc_python_tsan_host
  local st=$?
  set -e
  if [ "$st" -ne 0 ]; then
    fail "tsan dlopen gate exit=$st"
    return "$st"
  fi
  ok "tsan dlopen gate"
  return 0
}

case "$MODE" in
  mem)
    run_one mem node --expose-gc tests/cc_python_bridge_mem.js
    ;;
  neg)
    run_one neg node tests/cc_python_bridge_neg.js
    ;;
  fuzz)
    run_one mem node --expose-gc tests/cc_python_bridge_mem.js
    run_one fuzz env CHAOS_SCALE=quick OPENBLAS_NUM_THREADS=1 \
      FUZZ_SEED="${FUZZ_SEED:-1}" FUZZ_OPS="${FUZZ_OPS:-200}" \
      node --expose-gc stress/bridge/js_python_fuzz.js
    ;;
  tsan)
    # Stock Node SEGV under LD_PRELOAD=libtsan (interceptor vs non-TSan
    # main). Default gate: TSan-instrumented host dlopens the .node.
    # Full mem/fuzz: set NODE_TSAN_BIN to a ThreadSanitizer-built node.
    run_tsan_dlopen_gate || rc=1
    if [ -n "${NODE_TSAN_BIN:-}" ] && [ -x "${NODE_TSAN_BIN}" ]; then
      step "NODE_TSAN_BIN=$NODE_TSAN_BIN — running mem under TSan Node"
      run_one mem env LD_PRELOAD= "$NODE_TSAN_BIN" --expose-gc tests/cc_python_bridge_mem.js
    else
      ts; echo "SKIP node mem under TSan (stock Node cannot LD_PRELOAD libtsan; set NODE_TSAN_BIN)"
    fi
    ;;
  tsan-fuzz)
    run_tsan_dlopen_gate || rc=1
    if [ -n "${NODE_TSAN_BIN:-}" ] && [ -x "${NODE_TSAN_BIN}" ]; then
      step "NODE_TSAN_BIN=$NODE_TSAN_BIN — running mem+fuzz under TSan Node"
      run_one mem env LD_PRELOAD= "$NODE_TSAN_BIN" --expose-gc tests/cc_python_bridge_mem.js
      run_one fuzz env LD_PRELOAD= CHAOS_SCALE=quick OPENBLAS_NUM_THREADS=1 \
        FUZZ_SEED="${FUZZ_SEED:-1}" FUZZ_OPS="${FUZZ_OPS:-200}" \
        "$NODE_TSAN_BIN" --expose-gc stress/bridge/js_python_fuzz.js
    else
      ts; echo "SKIP node fuzz under TSan (stock Node cannot LD_PRELOAD libtsan; set NODE_TSAN_BIN)"
    fi
    ;;
  chaos)
    # neg omitted here: bookworm image is CPython 3.11 (no second in-process
    # interpreter). Run `sanitize_bridge.sh neg` on 3.12+ separately.
    run_one mem node --expose-gc tests/cc_python_bridge_mem.js
    run_one fuzz env CHAOS_SCALE=quick OPENBLAS_NUM_THREADS=1 \
      FUZZ_SEED="${FUZZ_SEED:-1}" FUZZ_OPS="${FUZZ_OPS:-200}" \
      node --expose-gc stress/bridge/js_python_fuzz.js
    run_one chaos env CHAOS_SCALE=quick OPENBLAS_NUM_THREADS=1 \
      node --expose-gc stress/bridge/js_python_chaos.js
    ;;
  bisect)
    if [ "$SAN_KIND" != asan ]; then
      fail "bisect mode requires ASan (got SAN_KIND=$SAN_KIND)"
      exit 2
    fi
    # Ordered prefixes of scripts/asan_mem_bisect.js (same ASan preload).
    # Prints BISECT_PREFIX_OK / BISECT_PREFIX_FAIL so the host can see the
    # first failing cumulative rung without re-pulling apt each try.
    step "ordered prefix bisect (rungs 1..N)"
    first_fail=""
    for n in 7 8 9 10 11 12; do
      step "prefix 1..$n"
      set +e
      if command -v timeout >/dev/null 2>&1; then
        timeout --signal=KILL "$SUITE_TIMEOUT" \
          env LD_PRELOAD="$SAN_LD_PRELOAD" BISECT_FROM=1 BISECT_TO="$n" \
          node --expose-gc scripts/asan_mem_bisect.js \
          > >(tee /tmp/bisect_${n}.log | sed -u 's/^/    | /') \
          2> >(tee -a /tmp/bisect_${n}.log | sed -u 's/^/    ! /' >&2)
        st=$?
      else
        env LD_PRELOAD="$SAN_LD_PRELOAD" BISECT_FROM=1 BISECT_TO="$n" \
          node --expose-gc scripts/asan_mem_bisect.js \
          > >(tee /tmp/bisect_${n}.log | sed -u 's/^/    | /') \
          2> >(tee -a /tmp/bisect_${n}.log | sed -u 's/^/    ! /' >&2)
        st=$?
      fi
      set -e
      sleep 0.2 2>/dev/null || true
      if [ "$st" -eq 0 ]; then
        ts; echo "BISECT_PREFIX_OK 1..$n"
      else
        ts; echo "BISECT_PREFIX_FAIL 1..$n exit=$st"
        first_fail="$n"
        # Isolate: rung N alone, then (N-1)+N if N>1
        step "isolate rung $n alone"
        set +e
        env LD_PRELOAD="$SAN_LD_PRELOAD" BISECT_FROM="$n" BISECT_TO="$n" \
          node --expose-gc scripts/asan_mem_bisect.js \
          > >(sed -u 's/^/    | /') 2> >(sed -u 's/^/    ! /' >&2)
        alone=$?
        set -e
        ts; echo "BISECT_ALONE rung=$n exit=$alone"
        if [ "$n" -gt 1 ]; then
          prev=$((n - 1))
          step "isolate rungs $prev..$n"
          set +e
          env LD_PRELOAD="$SAN_LD_PRELOAD" BISECT_FROM="$prev" BISECT_TO="$n" \
            node --expose-gc scripts/asan_mem_bisect.js \
            > >(sed -u 's/^/    | /') 2> >(sed -u 's/^/    ! /' >&2)
          pair=$?
          set -e
          ts; echo "BISECT_PAIR ${prev}..$n exit=$pair"
        fi
        break
      fi
    done
    if [ -n "$first_fail" ]; then
      ts; echo "BISECT_VERDICT first_failing_prefix=1..$first_fail"
      rc=1
    else
      ts; echo "BISECT_VERDICT all_prefixes_ok"
    fi
    ;;
  memneg|*)
    run_one mem node --expose-gc tests/cc_python_bridge_mem.js
    run_one neg node tests/cc_python_bridge_neg.js
    ;;
esac

ts; echo "=== finished overall_rc=$rc ==="
exit "$rc"
INNER

chmod +x "$INNER"

DOCKER_FLAGS=(--rm
  -v "$ROOT:/src:ro"
  -v "$INNER:/sanitize_bridge_inner.sh:ro"
  -w /src
  -e "FUZZ_SEED=${FUZZ_SEED:-1}"
  -e "FUZZ_OPS=${FUZZ_OPS:-200}"
  -e "FUZZ_TIMEOUT=${FUZZ_TIMEOUT:-60}"
  -e "FUZZ_HEARTBEAT_SECS=${FUZZ_HEARTBEAT_SECS:-5}"
  -e "FUZZ_PROGRESS_OPS=${FUZZ_PROGRESS_OPS:-25}"
  -e "FUZZ_OP_TIMEOUT_MS=${FUZZ_OP_TIMEOUT_MS:-10000}"
  -e "NODE_TSAN_BIN=${NODE_TSAN_BIN:-}")
# TSan needs ADDR_NO_RANDOMIZE (personality); default Docker seccomp blocks it.
case "$MODE" in
  tsan|tsan-fuzz)
    DOCKER_FLAGS+=(--security-opt seccomp=unconfined)
    ;;
esac
# No -t: keeps progress lines line-buffered in CI/logs.

# Retry docker pull once on transient registry timeouts (CI flakiness).
docker_run() {
  local tries=0
  while true; do
    tries=$((tries + 1))
    if docker run "$@"; then
      return 0
    fi
    local st=$?
    # 125 = docker daemon/client error (incl. pull timeout)
    if [ "$st" -ne 125 ] || [ "$tries" -ge 3 ]; then
      return "$st"
    fi
    echo "[$(host_ts)] docker run failed (exit 125) — retry $tries/3 after backoff…"
    sleep $((tries * 5))
  done
}

docker_run "${DOCKER_FLAGS[@]}" "$IMAGE" \
  bash /sanitize_bridge_inner.sh "$MODE" "$HEARTBEAT_SECS" "$SUITE_TIMEOUT"
st=$?
echo "[$(host_ts)] sanitize_bridge: docker exit=$st"
exit "$st"
