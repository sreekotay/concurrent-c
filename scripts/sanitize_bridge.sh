#!/usr/bin/env bash
# ASan-load the concurrent-c-python addon inside Linux Docker and run
# bridge smokes. macOS SIP cannot DYLD_INSERT ASan into stock Node —
# this is the supported runtime path. See docs/sanitizers.md.
#
#   ./scripts/sanitize_bridge.sh           # mem + neg
#   ./scripts/sanitize_bridge.sh fuzz      # mem + seeded walk (FUZZ_SEED / FUZZ_OPS)
#   ./scripts/sanitize_bridge.sh chaos     # + CHAOS_SCALE=quick
#   ./scripts/sanitize_bridge.sh mem       # mem only
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
    timeout --signal=KILL "$SUITE_TIMEOUT" "$@" \
      > >(tee -a "$log" | sed -u 's/^/    | /') \
      2> >(tee -a "$log" | sed -u 's/^/    ! /' >&2)
    local st=$?
  else
    "$@" \
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

step "locate ASan runtime"
ASAN_SO=$(find /usr/lib/llvm-*/lib/clang/*/lib/linux \
  -name 'libclang_rt.asan-*.so' 2>/dev/null | head -1)
test -n "$ASAN_SO" && test -f "$ASAN_SO"
ok "ASAN_SO=$ASAN_SO"

FLAGS='-fsanitize=address -shared-libasan -fno-omit-frame-pointer -g -O1 -fPIC'
step "compile vendor/cc_python.c"
clang $FLAGS -Inpm/cc-python/vendor/include \
  -c npm/cc-python/vendor/cc_python.c -o /tmp/tu.o
ok "tu.o"

step "compile vendor/runtime/concurrent_c.c"
clang $FLAGS -DCC_ENABLE_ASYNC -Inpm/cc-python/vendor/include \
  -c npm/cc-python/vendor/runtime/concurrent_c.c -o /tmp/rt.o
ok "rt.o"

step "link cc_python_asan.node"
clang -fsanitize=address -shared-libasan -shared \
  -o /tmp/cc_python_asan.node /tmp/tu.o /tmp/rt.o -lpthread -lm -ldl
ok "addon bytes=$(wc -c </tmp/cc_python_asan.node)"

export LD_PRELOAD="$ASAN_SO"
export CC_PYTHON_ADDON=/tmp/cc_python_asan.node
export ASAN_OPTIONS=detect_leaks=0:halt_on_error=0:abort_on_error=0
export OPENBLAS_NUM_THREADS=1
CC_LIBPYTHON=$(ls /usr/lib/*/libpython3.*.so | head -1)
export CC_LIBPYTHON
ok "CC_LIBPYTHON=$CC_LIBPYTHON"

rc=0
run_one() {
  if ! run_suite "$@"; then rc=1; fi
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
          env BISECT_FROM=1 BISECT_TO="$n" \
          node --expose-gc scripts/asan_mem_bisect.js \
          > >(tee /tmp/bisect_${n}.log | sed -u 's/^/    | /') \
          2> >(tee -a /tmp/bisect_${n}.log | sed -u 's/^/    ! /' >&2)
        st=$?
      else
        env BISECT_FROM=1 BISECT_TO="$n" \
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
        env BISECT_FROM="$n" BISECT_TO="$n" \
          node --expose-gc scripts/asan_mem_bisect.js \
          > >(sed -u 's/^/    | /') 2> >(sed -u 's/^/    ! /' >&2)
        alone=$?
        set -e
        ts; echo "BISECT_ALONE rung=$n exit=$alone"
        if [ "$n" -gt 1 ]; then
          prev=$((n - 1))
          step "isolate rungs $prev..$n"
          set +e
          env BISECT_FROM="$prev" BISECT_TO="$n" \
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
  -e "FUZZ_OP_TIMEOUT_MS=${FUZZ_OP_TIMEOUT_MS:-10000}")
# No -t: keeps progress lines line-buffered in CI/logs.

docker run "${DOCKER_FLAGS[@]}" "$IMAGE" \
  bash /sanitize_bridge_inner.sh "$MODE" "$HEARTBEAT_SECS" "$SUITE_TIMEOUT"
st=$?
echo "[$(host_ts)] sanitize_bridge: docker exit=$st"
exit "$st"
