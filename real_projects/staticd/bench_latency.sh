#!/usr/bin/env bash
# Latency-first matrix: page-cache fixtures, shuffle server order, median
# over REPEATS (first round discarded as warmup when REPEATS>1).
#
#   ./bench_latency.sh              # directional: 3s, 3 rounds, 3 files
#   FULL=1 ./bench_latency.sh       # receipt: 30s, 5 rounds, 5 files
#   SMOKE=1 ./bench_latency.sh      # 2s, 4kb.html @ c=10
#
# Explicit REPEATS / DURATION / FILES / CONCURRENCY always win.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FIX="$SCRIPT_DIR/fixtures"
STATICD_BIN="${STATICD_BIN:-$SCRIPT_DIR/out/staticd}"
DARKHTTPD_BIN="${DARKHTTPD_BIN:-$SCRIPT_DIR/out/darkhttpd}"
INCLUDE_STATICD="${INCLUDE_STATICD:-1}"
INCLUDE_NGINX="${INCLUDE_NGINX:-1}"
INCLUDE_DARKHTTPD="${INCLUDE_DARKHTTPD:-1}"
INCLUDE_CADDY="${INCLUDE_CADDY:-0}"
STATICD_PORT="${STATICD_PORT:-8080}"
NGINX_PORT="${NGINX_PORT:-8081}"
DARKHTTPD_PORT="${DARKHTTPD_PORT:-8082}"
CADDY_PORT="${CADDY_PORT:-8083}"

SMOKE="${SMOKE:-0}"
FULL="${FULL:-0}"
if [[ "$SMOKE" == "1" ]]; then
    REPEATS="${REPEATS:-2}"
    DURATION="${DURATION:-2}"
    WARMUP="${WARMUP:-0}"
    CONCURRENCY="${CONCURRENCY:-10}"
    FILES="${FILES:-4kb.html}"
elif [[ "$FULL" == "1" ]]; then
    REPEATS="${REPEATS:-5}"
    DURATION="${DURATION:-30}"
    WARMUP="${WARMUP:-10}"
    CONCURRENCY="${CONCURRENCY:-1 10 100}"
    FILES="${FILES:-1kb.bin 4kb.html 64kb.js 1mb.bin 10mb.bin}"
else
    REPEATS="${REPEATS:-3}"
    DURATION="${DURATION:-3}"
    WARMUP="${WARMUP:-1}"
    CONCURRENCY="${CONCURRENCY:-1 10 100}"
    FILES="${FILES:-4kb.html 1mb.bin 10mb.bin}"
fi

BENCH_OUT="${BENCH_OUT:-}"
THREADS="${THREADS:-2}"

PIDS=()
TMPDIR_RUN="$(mktemp -d "$SCRIPT_DIR/run/bench.XXXXXX")"
reap_listen() {
    local port p
    for port in "$STATICD_PORT" "$NGINX_PORT" "$DARKHTTPD_PORT" "$CADDY_PORT"; do
        for p in $(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null || true); do
            kill -9 "$p" 2>/dev/null || true
        done
    done
}
cleanup() {
    local p
    for p in "${PIDS[@]:-}"; do
        kill "$p" 2>/dev/null || true
    done
    sleep 0.15
    for p in "${PIDS[@]:-}"; do
        kill -9 "$p" 2>/dev/null || true
        wait "$p" 2>/dev/null || true
    done
    if [[ -f "$SCRIPT_DIR/run/nginx/nginx.pid" ]]; then
        nginx -c "$SCRIPT_DIR/run/nginx/nginx.conf" -p "$SCRIPT_DIR/run/nginx" -s quit 2>/dev/null || true
    fi
    reap_listen
    rm -rf "$TMPDIR_RUN"
}
trap cleanup EXIT
reap_listen

detect_tool() {
    if command -v wrk >/dev/null 2>&1; then
        echo wrk
    elif command -v hey >/dev/null 2>&1; then
        echo hey
    else
        echo none
    fi
}

TOOL="$(detect_tool)"
if [[ "$TOOL" == "none" ]]; then
    echo "need wrk or hey; run ./setup.sh" >&2
    exit 1
fi

wait_port() {
    local port="$1" name="$2" i
    for i in $(seq 1 50); do
        if nc -z 127.0.0.1 "$port" 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    echo "timeout waiting for ${name:-server} on :$port" >&2
    return 1
}

expect_200() {
    local port="$1" name="$2" code
    code=$(curl -sS -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${port}/index.html" || echo 000)
    if [[ "$code" != "200" ]]; then
        echo "$name on :$port not serving (HTTP $code)" >&2
        return 1
    fi
}

start_servers() {
    if [[ "$INCLUDE_STATICD" == "1" ]]; then
        [[ -x "$STATICD_BIN" ]] || { echo "missing staticd" >&2; exit 1; }
        "$STATICD_BIN" --listen "127.0.0.1:${STATICD_PORT}" --root "$FIX" \
            >"$TMPDIR_RUN/staticd.log" 2>&1 &
        PIDS+=($!)
        wait_port "$STATICD_PORT" staticd
        expect_200 "$STATICD_PORT" staticd
    fi
    if [[ "$INCLUDE_NGINX" == "1" ]]; then
        if ! command -v nginx >/dev/null 2>&1; then
            INCLUDE_NGINX=0
        else
            local prefix="$SCRIPT_DIR/run/nginx"
            mkdir -p "$prefix/logs"
            sed "s|FIXTURES_ROOT|$FIX|g" "$SCRIPT_DIR/nginx.conf.in" > "$prefix/nginx.conf"
            nginx -c "$prefix/nginx.conf" -p "$prefix" >"$TMPDIR_RUN/nginx.log" 2>&1 &
            PIDS+=($!)
            wait_port "$NGINX_PORT" nginx
            expect_200 "$NGINX_PORT" nginx
        fi
    fi
    if [[ "$INCLUDE_DARKHTTPD" == "1" ]]; then
        if [[ ! -x "$DARKHTTPD_BIN" ]]; then
            INCLUDE_DARKHTTPD=0
        else
            "$DARKHTTPD_BIN" "$FIX" --addr 127.0.0.1 --port "$DARKHTTPD_PORT" \
                >"$TMPDIR_RUN/darkhttpd.log" 2>&1 &
            PIDS+=($!)
            wait_port "$DARKHTTPD_PORT" darkhttpd
            expect_200 "$DARKHTTPD_PORT" darkhttpd
        fi
    fi
    if [[ "$INCLUDE_CADDY" == "1" ]]; then
        if ! command -v caddy >/dev/null 2>&1; then
            INCLUDE_CADDY=0
        else
            sed "s|FIXTURES_ROOT|$FIX|g" "$SCRIPT_DIR/Caddyfile.in" > "$TMPDIR_RUN/Caddyfile"
            caddy run --config "$TMPDIR_RUN/Caddyfile" >"$TMPDIR_RUN/caddy.log" 2>&1 &
            PIDS+=($!)
            wait_port "$CADDY_PORT" caddy
            expect_200 "$CADDY_PORT" caddy
        fi
    fi
}

page_cache() {
    # Touch every fixture byte once so subsequent runs are warm-cache.
    cat "$FIX"/* >/dev/null 2>&1 || true
}

# Parse wrk -latency output into p50 p75 p90 p99 rps errs
# (wrk's Latency Distribution has 50/75/90/99 — no 95).
parse_wrk() {
    local file="$1"
    local rps errs p50 p75 p90 p99
    rps=$(awk '/Requests\/sec:/ {print $2}' "$file" | tail -1)
    errs=$(awk '/Socket errors:/ {
        s=0; for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; print s; exit
    }' "$file")
    [[ -z "$errs" ]] && errs=0
    p50=$(awk '/Latency Distribution/,0 { if ($1=="50%") {print $2; exit} }' "$file")
    p75=$(awk '/Latency Distribution/,0 { if ($1=="75%") {print $2; exit} }' "$file")
    p90=$(awk '/Latency Distribution/,0 { if ($1=="90%") {print $2; exit} }' "$file")
    p99=$(awk '/Latency Distribution/,0 { if ($1=="99%") {print $2; exit} }' "$file")
    # Normalize units to ms: wrk prints "123.45us" or "1.23ms" or "1.23s"
    to_ms() {
        local v="$1"
        if [[ "$v" == *us ]]; then
            awk -v x="${v%us}" 'BEGIN{printf "%.3f", x/1000}'
        elif [[ "$v" == *ms ]]; then
            awk -v x="${v%ms}" 'BEGIN{printf "%.3f", x}'
        elif [[ "$v" == *s ]]; then
            awk -v x="${v%s}" 'BEGIN{printf "%.3f", x*1000}'
        else
            echo "$v"
        fi
    }
    p50=$(to_ms "${p50:-0}")
    p75=$(to_ms "${p75:-0}")
    p90=$(to_ms "${p90:-0}")
    p99=$(to_ms "${p99:-0}")
    echo "${p50:-0} ${p75:-0} ${p90:-0} ${p99:-0} ${rps:-0} ${errs:-0}"
}

parse_hey() {
    local file="$1"
    local rps p50 p75 p90 p99 errs
    rps=$(awk '/Requests\/sec:/ {print $2}' "$file" | tail -1)
    p50=$(awk '/50% in / {print $(NF-1)}' "$file" | head -1)
    p75=$(awk '/75% in / {print $(NF-1)}' "$file" | head -1)
    p90=$(awk '/90% in / {print $(NF-1)}' "$file" | head -1)
    p99=$(awk '/99% in / {print $(NF-1)}' "$file" | head -1)
    errs=$(awk '/\[ERROR\]/ {c++} END{print c+0}' "$file")
    # hey prints seconds
    to_ms() { awk -v x="${1:-0}" 'BEGIN{printf "%.3f", x*1000}'; }
    echo "$(to_ms "$p50") $(to_ms "$p75") $(to_ms "$p90") $(to_ms "$p99") ${rps:-0} ${errs:-0}"
}

run_load() {
    local url="$1" c="$2" out="$3"
    if [[ "$TOOL" == "wrk" ]]; then
        # wrk requires connections >= threads
        local t="$THREADS"
        if [[ "$c" -lt "$t" ]]; then t="$c"; fi
        if [[ "$t" -lt 1 ]]; then t=1; fi
        wrk -t "$t" -c "$c" -d "${DURATION}s" --latency "$url" >"$out" 2>&1 || true
    else
        hey -z "${DURATION}s" -c "$c" "$url" >"$out" 2>&1 || true
    fi
}

median_of() {
    # stdin: one number per line → median
    sort -n | awk '{
        a[NR]=$1
    } END {
        if (NR==0) {print 0; exit}
        if (NR%2) print a[(NR+1)/2]
        else print (a[NR/2]+a[NR/2+1])/2
    }'
}

LABELS=()
PORTS=()
if [[ "$INCLUDE_STATICD" == "1" ]]; then LABELS+=(staticd); PORTS+=("$STATICD_PORT"); fi
if [[ "$INCLUDE_NGINX" == "1" ]]; then LABELS+=(nginx); PORTS+=("$NGINX_PORT"); fi
if [[ "$INCLUDE_DARKHTTPD" == "1" ]]; then LABELS+=(darkhttpd); PORTS+=("$DARKHTTPD_PORT"); fi
if [[ "$INCLUDE_CADDY" == "1" ]]; then LABELS+=(caddy); PORTS+=("$CADDY_PORT"); fi

[[ -f "$FIX/manifest.txt" ]] || ./gen_fixtures.sh
start_servers
page_cache

HOST="$(uname -srm)"
CPUS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo '?')"
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

{
    echo "# staticd latency benchmark"
    echo "# host: $HOST, cpus: $CPUS, date: $DATE"
    echo "# tool: $TOOL, duration: ${DURATION}s, warmup_rounds: 1 (discarded if REPEATS>1), repeats: $REPEATS (median of measured)"
    echo "# note: WARMUP=$WARMUP reserved; wrk has no internal warmup — round 0 discarded"
    echo "# fixtures: page-cached before each block"
    echo "#"
    printf '%-12s %-6s %-10s %10s %10s %10s %10s %12s %6s\n' \
        file c server p50_ms p75_ms p90_ms p99_ms rps errs
} | tee "$TMPDIR_RUN/receipt.txt"

for file in $FILES; do
    for c in $CONCURRENCY; do
        page_cache
        # Shuffle server indices
        order=()
        for i in "${!LABELS[@]}"; do order+=("$i"); done
        # Fisher-Yates
        for ((i=${#order[@]}-1; i>0; i--)); do
            j=$((RANDOM % (i+1)))
            tmp=${order[i]}; order[i]=${order[j]}; order[j]=$tmp
        done

        for idx in "${order[@]}"; do
            name="${LABELS[$idx]}"
            port="${PORTS[$idx]}"
            url="http://127.0.0.1:${port}/${file}"

            declare -a p50s=() p75s=() p90s=() p99s=() rpss=() errss=()
            for ((r=0; r<REPEATS; r++)); do
                outf="$TMPDIR_RUN/${name}_${file}_${c}_r${r}.txt"
                echo "  wrk $name $file c=$c round=$((r+1))/$REPEATS (${DURATION}s)..." >&2
                run_load "$url" "$c" "$outf"
                if [[ "$TOOL" == "wrk" ]]; then
                    read -r p50 p75 p90 p99 rps errs < <(parse_wrk "$outf")
                else
                    read -r p50 p75 p90 p99 rps errs < <(parse_hey "$outf")
                fi
                if [[ "$REPEATS" -gt 1 && "$r" -eq 0 ]]; then
                    continue  # discard warmup round
                fi
                p50s+=("$p50"); p75s+=("$p75"); p90s+=("$p90"); p99s+=("$p99")
                rpss+=("$rps"); errss+=("$errs")
            done
            med() { printf '%s\n' "$@" | median_of; }
            # Sum errs (not median) — any errors matter
            err_sum=0
            for e in "${errss[@]:-}"; do err_sum=$((err_sum + ${e%.*})); done
            line=$(printf '%-12s %-6s %-10s %10s %10s %10s %10s %12s %6s\n' \
                "$file" "$c" "$name" \
                "$(med "${p50s[@]}")" "$(med "${p75s[@]}")" \
                "$(med "${p90s[@]}")" "$(med "${p99s[@]}")" \
                "$(med "${rpss[@]}")" "$err_sum")
            echo "$line" | tee -a "$TMPDIR_RUN/receipt.txt"
        done
    done
done

if [[ -n "$BENCH_OUT" ]]; then
    mkdir -p "$(dirname "$BENCH_OUT")"
    cp "$TMPDIR_RUN/receipt.txt" "$BENCH_OUT"
    echo "wrote $BENCH_OUT"
fi
