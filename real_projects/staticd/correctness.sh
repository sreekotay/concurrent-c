#!/usr/bin/env bash
# Correctness gate: every enabled server must match fixtures/manifest.txt
# (status 200, Content-Length, body SHA-256) and reject traversal.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

FIX="$SCRIPT_DIR/fixtures"
MANIFEST="$FIX/manifest.txt"
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

PIDS=()
TMPDIR_RUN="$(mktemp -d "$SCRIPT_DIR/run/correctness.XXXXXX")"
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
    rm -rf "$TMPDIR_RUN"
}
trap cleanup EXIT

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

wait_port() {
    local port="$1" name="$2" i
    for i in $(seq 1 50); do
        if curl -sS -o /dev/null --connect-timeout 0.2 "http://127.0.0.1:${port}/" 2>/dev/null \
            || curl -sS -o /dev/null --connect-timeout 0.2 "http://127.0.0.1:${port}/index.html" 2>/dev/null; then
            return 0
        fi
        # Connection refused vs 404 both mean the listener is up for some servers.
        if nc -z 127.0.0.1 "$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "timeout waiting for $name on :$port" >&2
    return 1
}

start_staticd() {
    [[ -x "$STATICD_BIN" ]] || { echo "missing $STATICD_BIN" >&2; exit 1; }
    "$STATICD_BIN" --listen "127.0.0.1:${STATICD_PORT}" --root "$FIX" \
        --header 'Access-Control-Allow-Origin: *' \
        >"$TMPDIR_RUN/staticd.log" 2>&1 &
    PIDS+=($!)
    wait_port "$STATICD_PORT" staticd
}

start_darkhttpd() {
    if [[ ! -x "$DARKHTTPD_BIN" ]]; then
        echo "skip darkhttpd (not built)"
        INCLUDE_DARKHTTPD=0
        return 0
    fi
    "$DARKHTTPD_BIN" "$FIX" --addr 127.0.0.1 --port "$DARKHTTPD_PORT" \
        >"$TMPDIR_RUN/darkhttpd.log" 2>&1 &
    PIDS+=($!)
    wait_port "$DARKHTTPD_PORT" darkhttpd
}

start_nginx() {
    if ! command -v nginx >/dev/null 2>&1; then
        echo "skip nginx (not installed)"
        INCLUDE_NGINX=0
        return 0
    fi
    local prefix="$SCRIPT_DIR/run/nginx"
    mkdir -p "$prefix/logs" "$prefix"
    sed "s|FIXTURES_ROOT|$FIX|g" "$SCRIPT_DIR/nginx.conf.in" > "$prefix/nginx.conf"
    nginx -c "$prefix/nginx.conf" -p "$prefix" >"$TMPDIR_RUN/nginx.log" 2>&1 &
    PIDS+=($!)
    wait_port "$NGINX_PORT" nginx
}

start_caddy() {
    if ! command -v caddy >/dev/null 2>&1; then
        echo "skip caddy (not installed)"
        INCLUDE_CADDY=0
        return 0
    fi
    sed "s|FIXTURES_ROOT|$FIX|g" "$SCRIPT_DIR/Caddyfile.in" > "$TMPDIR_RUN/Caddyfile"
    caddy run --config "$TMPDIR_RUN/Caddyfile" >"$TMPDIR_RUN/caddy.log" 2>&1 &
    PIDS+=($!)
    wait_port "$CADDY_PORT" caddy
}

check_server() {
    local name="$1" port="$2"
    local line path expect_hash expect_size hdrs body status clen got_hash
    echo "== $name :$port =="
    while read -r line; do
        [[ "$line" =~ ^# ]] && continue
        [[ -z "$line" ]] && continue
        path=$(echo "$line" | awk '{print $1}')
        expect_hash=$(echo "$line" | awk '{print $2}')
        expect_size=$(echo "$line" | awk '{print $3}')
        hdrs="$TMPDIR_RUN/${name}${path////_}.hdr"
        body="$TMPDIR_RUN/${name}${path////_}.body"
        curl -sS -D "$hdrs" -o "$body" "http://127.0.0.1:${port}${path}"
        status=$(awk 'NR==1 {print $2}' "$hdrs")
        clen=$(awk 'BEGIN{IGNORECASE=1} /^Content-Length:/ {print $2}' "$hdrs" | tr -d '\r')
        got_hash=$(sha256_file "$body")
        if [[ "$status" != "200" ]]; then
            echo "FAIL $name $path status=$status (want 200)" >&2
            exit 1
        fi
        if [[ "$clen" != "$expect_size" ]]; then
            echo "FAIL $name $path Content-Length=$clen (want $expect_size)" >&2
            exit 1
        fi
        if [[ "$got_hash" != "$expect_hash" ]]; then
            echo "FAIL $name $path sha256=$got_hash (want $expect_hash)" >&2
            exit 1
        fi
        echo "  ok $path"
    done < "$MANIFEST"

    # Traversal must not escape the docroot.
    local tstatus
    tstatus=$(curl --path-as-is -sS -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${port}/../etc/passwd" || true)
    case "$tstatus" in
        400|403|404) echo "  ok traversal -> $tstatus" ;;
        *)
            echo "FAIL $name traversal status=$tstatus (want 400/403/404)" >&2
            exit 1
            ;;
    esac

    if [[ "$name" == staticd ]]; then
        local ostatus allow
        ostatus=$(curl -sS -o /dev/null -w '%{http_code}' -X OPTIONS \
            "http://127.0.0.1:${port}/4kb.html")
        allow=$(curl -sS -D- -o /dev/null -X OPTIONS \
            "http://127.0.0.1:${port}/4kb.html" | awk 'BEGIN{IGNORECASE=1} /^Allow:/ {print}' | tr -d '\r')
        if [[ "$ostatus" != "204" ]]; then
            echo "FAIL $name OPTIONS status=$ostatus (want 204)" >&2
            exit 1
        fi
        if [[ "$allow" != *GET* || "$allow" != *HEAD* || "$allow" != *OPTIONS* ]]; then
            echo "FAIL $name OPTIONS Allow='$allow'" >&2
            exit 1
        fi
        echo "  ok OPTIONS -> 204 $allow"

        local qcode qclen acao
        qcode=$(curl -sS -o /dev/null -w '%{http_code}' \
            "http://127.0.0.1:${port}/4kb.html?cache=1")
        qclen=$(curl -sS -D- -o /dev/null \
            "http://127.0.0.1:${port}/4kb.html?cache=1" \
            | awk 'BEGIN{IGNORECASE=1} /^Content-Length:/ {print $2}' | tr -d '\r')
        if [[ "$qcode" != "200" || "$qclen" != "4096" ]]; then
            echo "FAIL $name query status=$qcode clen=$qclen" >&2
            exit 1
        fi
        echo "  ok query strip -> 200 clen=$qclen"
        acao=$(curl -sS -D- -o /dev/null "http://127.0.0.1:${port}/4kb.html" \
            | awk 'BEGIN{IGNORECASE=1} /^Access-Control-Allow-Origin:/ {print}' | tr -d '\r')
        if [[ "$acao" != *'*'* ]]; then
            echo "FAIL $name missing CORS header ($acao)" >&2
            exit 1
        fi
        echo "  ok --header -> $acao"
    fi
}

[[ -f "$MANIFEST" ]] || ./gen_fixtures.sh

if [[ "$INCLUDE_STATICD" == "1" ]]; then start_staticd; fi
if [[ "$INCLUDE_NGINX" == "1" ]]; then start_nginx; fi
if [[ "$INCLUDE_DARKHTTPD" == "1" ]]; then start_darkhttpd; fi
if [[ "$INCLUDE_CADDY" == "1" ]]; then start_caddy; fi

if [[ "$INCLUDE_STATICD" == "1" ]]; then check_server staticd "$STATICD_PORT"; fi
if [[ "$INCLUDE_NGINX" == "1" ]]; then check_server nginx "$NGINX_PORT"; fi
if [[ "$INCLUDE_DARKHTTPD" == "1" ]]; then check_server darkhttpd "$DARKHTTPD_PORT"; fi
if [[ "$INCLUDE_CADDY" == "1" ]]; then check_server caddy "$CADDY_PORT"; fi

if [[ "$INCLUDE_STATICD" == "1" ]]; then
    list_port=$((STATICD_PORT + 10))
    www="$TMPDIR_RUN/www"
    mkdir -p "$www/sub"
    printf 'home\n' > "$www/home.html"
    printf 'hi\n' > "$www/sub/a.txt"
    "$STATICD_BIN" --listen "127.0.0.1:${list_port}" --root "$www" \
        --list --index home.html \
        --header 'X-Staticd: 1' --header 'X-Extra: yes' \
        >"$TMPDIR_RUN/staticd-list.log" 2>&1 &
    PIDS+=($!)
    wait_port "$list_port" staticd-list
    lcode=$(curl -sS -o "$TMPDIR_RUN/list.html" -w '%{http_code}' \
        "http://127.0.0.1:${list_port}/sub/")
    lbody=$(cat "$TMPDIR_RUN/list.html")
    if [[ "$lcode" != "200" || "$lbody" != *a.txt* || "$lbody" != *href=\"/sub/a.txt\"* ]]; then
        echo "FAIL staticd listing status=$lcode body=$lbody" >&2
        exit 1
    fi
    echo "  ok --list /sub/ -> 200"
    icode=$(curl -sS -o "$TMPDIR_RUN/home.body" -w '%{http_code}' \
        "http://127.0.0.1:${list_port}/")
    ibody=$(cat "$TMPDIR_RUN/home.body")
    if [[ "$icode" != "200" || "$ibody" != home ]]; then
        echo "FAIL staticd --index status=$icode body=$ibody" >&2
        exit 1
    fi
    echo "  ok --index home.html -> /"
    # default server has fixtures/index.html; a dir without index is 403
    mkdir -p "$FIX/empty_dir_probe"
    dcode=$(curl -sS -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${STATICD_PORT}/empty_dir_probe/")
    rmdir "$FIX/empty_dir_probe" 2>/dev/null || true
    if [[ "$dcode" != "403" ]]; then
        echo "FAIL staticd dir without --list status=$dcode (want 403)" >&2
        exit 1
    fi
    echo "  ok dir without --list -> 403"
    xhdr=$(curl -sS -D- -o /dev/null "http://127.0.0.1:${list_port}/" \
        | awk 'BEGIN{IGNORECASE=1} /^X-Staticd:|^X-Extra:/ {print}' | tr -d '\r')
    if [[ "$xhdr" != *X-Staticd* || "$xhdr" != *X-Extra* ]]; then
        echo "FAIL staticd repeat --header ($xhdr)" >&2
        exit 1
    fi
    echo "  ok repeat --header"
fi

echo "correctness: PASS"
