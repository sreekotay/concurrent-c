#!/usr/bin/env bash
# Specimen: mixed JS-delay + static load to exercise opt-in ready-app steals.
#
#   /slow?ms=N  — busy-waits under the process-wide pages exclusive
#   /4kb.html   — static MISS (no exclusive); can progress while /slow holds it
#
# Default mix is 1 slow : (SLOW_EVERY-1) static (SLOW_EVERY=5 → 1:4).
#
# Compares CC_SERVER_STEP_STEAL modes (default in the binary is off / 0):
#   0 = serial, 2 = queue only, 1 = queue+steal.
# Prints wrk latency and staticd's "step steals" line on shutdown.
#
# Steal is not a substitute for a parking exclusive around shared engines.
# Prefer proving a win on independently runnable HOL before turning it on.
#
#   ./bench_steal_mix.sh
#   DELAY_MS=50 WORKERS=4 CONCURRENCY=100 DURATION=5 ./bench_steal_mix.sh
#   SLOW_EVERY=2 ./bench_steal_mix.sh   # 1:1 alternate
#
# Needs QuickJS (CC_QUICKJS_SRC or repo ./quickjs) and wrk.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

STATICD_BIN="${STATICD_BIN:-$SCRIPT_DIR/out/staticd}"
PORT="${PORT:-8091}"
WORKERS="${WORKERS:-4}"
CONCURRENCY="${CONCURRENCY:-100}"
DURATION="${DURATION:-5}"
DELAY_MS="${DELAY_MS:-20}"
SLOW_EVERY="${SLOW_EVERY:-5}"
TIMEOUT="${TIMEOUT:-30}"
THREADS="${THREADS:-2}"
PAGES="${PAGES:-$SCRIPT_DIR/pages}"
FIX="${FIX:-$SCRIPT_DIR/fixtures}"
STATIC_PATH="${STATIC_PATH:-/4kb.html}"
SLOW_PATH="${SLOW_PATH:-/slow?ms=${DELAY_MS}}"

if [[ "$SLOW_EVERY" -lt 1 ]]; then
    echo "SLOW_EVERY must be >= 1" >&2
    exit 1
fi

if [[ -z "${CC_QUICKJS_SRC:-}" && -d "$REPO_ROOT/quickjs" ]]; then
    export CC_QUICKJS_SRC="$REPO_ROOT/quickjs"
fi

if [[ ! -x "$STATICD_BIN" ]]; then
    echo "missing $STATICD_BIN — make -C real_projects/staticd staticd" >&2
    exit 1
fi
if ! command -v wrk >/dev/null 2>&1; then
    echo "need wrk" >&2
    exit 1
fi
if [[ ! -f "$PAGES/slow.js" ]]; then
    echo "missing $PAGES/slow.js" >&2
    exit 1
fi

TMP="$(mktemp -d "$SCRIPT_DIR/run/stealmix.XXXXXX")"
cleanup() {
    if [[ -n "${PID:-}" ]]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    reap
    rm -rf "$TMP"
}
reap() {
    local p
    for p in $(lsof -tiTCP:"$PORT" -sTCP:LISTEN 2>/dev/null || true); do
        kill -9 "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT
reap

cat >"$TMP/mix.lua" <<EOF
local n = 0
local every = $SLOW_EVERY
local slow = "$SLOW_PATH"
local fast = "$STATIC_PATH"
request = function()
  n = n + 1
  if every <= 1 or (n % every) == 1 then
    return wrk.format(nil, slow)
  end
  return wrk.format(nil, fast)
end
EOF

parse_wrk() {
    local file="$1"
    local rps errs p50 p99
    rps=$(awk '/Requests\/sec:/ {v=$2} END{printf "%.0f", v+0}' "$file")
    errs=$(awk '/Socket errors:/ {
        s=0; for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; print s; exit
    }' "$file")
    [[ -z "$errs" ]] && errs=0
    p50=$(awk '/Latency Distribution/,0 { if ($1=="50%") {print $2; exit} }' "$file")
    p99=$(awk '/Latency Distribution/,0 { if ($1=="99%") {print $2; exit} }' "$file")
    to_ms() {
        local v="$1"
        if [[ "$v" == *us ]]; then
            awk -v x="${v%us}" 'BEGIN{printf "%.3f", x/1000}'
        elif [[ "$v" == *ms ]]; then
            awk -v x="${v%ms}" 'BEGIN{printf "%.3f", x}'
        elif [[ "$v" == *s ]]; then
            awk -v x="${v%s}" 'BEGIN{printf "%.3f", x*1000}'
        else
            echo "${v:-0}"
        fi
    }
    p50=$(to_ms "${p50:-0}")
    p99=$(to_ms "${p99:-0}")
    echo "${p50:-0} ${p99:-0} ${rps:-0} ${errs:-0}"
}

run_one() {
    local steal="$1" tag="$2"
    local log="$TMP/${tag}.log" out="$TMP/${tag}.wrk"
    local steals=0

    reap
    CC_SERVER_STEP_STEAL="$steal" "$STATICD_BIN" \
        --listen "127.0.0.1:${PORT}" \
        --root "$FIX" \
        --pages "$PAGES" \
        --workers "$WORKERS" \
        >"$log" 2>&1 &
    PID=$!

    local i code
    for i in $(seq 1 50); do
        if nc -z 127.0.0.1 "$PORT" 2>/dev/null; then break; fi
        sleep 0.1
    done
    code=$(curl -sS -m 3 -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${PORT}/slow?ms=1" || echo 000)
    if [[ "$code" == "501" ]]; then
        echo "SKIP: QuickJS not attached (set CC_QUICKJS_SRC)" >&2
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
        PID=""
        exit 0
    fi
    if [[ "$code" != "200" ]]; then
        echo "FAIL /slow status=$code" >&2
        cat "$log" >&2 || true
        exit 1
    fi
    code=$(curl -sS -m 2 -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${PORT}${STATIC_PATH}" || echo 000)
    if [[ "$code" != "200" ]]; then
        echo "FAIL $STATIC_PATH status=$code" >&2
        exit 1
    fi

    wrk -t "$THREADS" -c "$CONCURRENCY" -d "${DURATION}s" \
        --timeout "${TIMEOUT}s" --latency \
        -s "$TMP/mix.lua" "http://127.0.0.1:${PORT}${STATIC_PATH}" \
        >"$out" 2>&1 || true

    kill -INT "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    PID=""
    steals=$(awk '/step steals:/ {print $NF; exit}' "$log" 2>/dev/null || echo 0)
    steals=${steals:-0}

    read -r p50 p99 rps errs < <(parse_wrk "$out")
    printf '%-12s mode=%-4s steals=%-8s p50=%-8s p99=%-8s rps=%-10s err=%s\n' \
        "$tag" "$steal" "$steals" "$p50" "$p99" "$rps" "$errs"
}

STATIC_N=$((SLOW_EVERY - 1))
if [[ "$SLOW_EVERY" -le 1 ]]; then
    MIX_DESC="all slow"
else
    MIX_DESC="1 slow : ${STATIC_N} static"
fi
echo "# steal mix specimen: workers=$WORKERS c=$CONCURRENCY d=${DURATION}s delay=${DELAY_MS}ms"
echo "# mix: $MIX_DESC (SLOW_EVERY=$SLOW_EVERY)  $SLOW_PATH  /  $STATIC_PATH"
echo "# modes: 0=serial (binary default)  2=queue-only  1=queue+steal"
echo "# QuickJS: ${CC_QUICKJS_SRC:-./quickjs probe}"
run_one 0 "serial"
run_one 2 "queue"
run_one 1 "steal"
