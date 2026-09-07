#!/usr/bin/env bash
# Correctness gate: every enabled server must match fixtures/manifest.txt
# (status 200, Content-Length, body SHA-256) and reject traversal.
# staticd also: OPTIONS, query strip, --header, --index, --list,
# Connection token list, Range / 304 / If-Range, intermediate symlink
# jail, atomic rename under a hot name, WebSocket echo, --pages script
# replies (SKIP if QuickJS / libpython missing), optional TLS
# (--tls-cert/--tls-key with BearSSL sample PEMs).
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

hdr_field() {
    local file="$1" name="$2"
    awk -v n="$name" 'BEGIN{IGNORECASE=1} $0 ~ "^" n ":" {
        sub(/^[^:]+:[ \t]*/, ""); print; exit
    }' "$file" | tr -d '\r'
}

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

        local rcode rclen rbytes
        rcode=$(curl -sS -D "$TMPDIR_RUN/range.hdr" -o "$TMPDIR_RUN/range.body" \
            -w '%{http_code}' -H 'Range: bytes=0-15' \
            "http://127.0.0.1:${port}/4kb.html")
        rclen=$(hdr_field "$TMPDIR_RUN/range.hdr" Content-Length)
        rbytes=$(wc -c < "$TMPDIR_RUN/range.body" | tr -d ' ')
        if [[ "$rcode" != "206" || "$rclen" != "16" || "$rbytes" != "16" ]]; then
            echo "FAIL $name Range 0-15 status=$rcode clen=$rclen bytes=$rbytes" >&2
            exit 1
        fi
        echo "  ok Range bytes=0-15 -> 206 clen=16"

        local ovr
        ovr=$(curl -sS -o /dev/null -w '%{http_code}' \
            -H 'Range: bytes=999999999999999999999999-999999999999999999999999' \
            "http://127.0.0.1:${port}/4kb.html")
        if [[ "$ovr" != "416" ]]; then
            echo "FAIL $name Range overflow status=$ovr (want 416)" >&2
            exit 1
        fi
        echo "  ok Range overflow -> 416"

        local lm ims_code ifr_ok ifr_miss bad_ims conn_h
        curl -sS -D "$TMPDIR_RUN/lm.hdr" -o /dev/null \
            "http://127.0.0.1:${port}/4kb.html"
        lm=$(hdr_field "$TMPDIR_RUN/lm.hdr" Last-Modified)
        if [[ -z "$lm" ]]; then
            echo "FAIL $name missing Last-Modified" >&2
            exit 1
        fi
        ims_code=$(curl -sS -o /dev/null -w '%{http_code}' \
            -H "If-Modified-Since: $lm" \
            "http://127.0.0.1:${port}/4kb.html")
        if [[ "$ims_code" != "304" ]]; then
            echo "FAIL $name If-Modified-Since status=$ims_code (want 304)" >&2
            exit 1
        fi
        echo "  ok If-Modified-Since -> 304"
        bad_ims=$(curl -sS -o /dev/null -w '%{http_code}' \
            -H 'If-Modified-Since: not-a-date' \
            "http://127.0.0.1:${port}/4kb.html")
        if [[ "$bad_ims" != "200" ]]; then
            echo "FAIL $name malformed If-Modified-Since status=$bad_ims (want 200)" >&2
            exit 1
        fi
        echo "  ok malformed If-Modified-Since -> 200"
        ifr_ok=$(curl -sS -o /dev/null -w '%{http_code}' \
            -H "If-Range: $lm" -H 'Range: bytes=0-15' \
            "http://127.0.0.1:${port}/4kb.html")
        if [[ "$ifr_ok" != "206" ]]; then
            echo "FAIL $name If-Range match status=$ifr_ok (want 206)" >&2
            exit 1
        fi
        echo "  ok If-Range match -> 206"
        ifr_miss=$(curl -sS -o /dev/null -w '%{http_code}' \
            -H 'If-Range: Wed, 01 Jan 1990 00:00:00 GMT' \
            -H 'Range: bytes=0-15' \
            "http://127.0.0.1:${port}/4kb.html")
        if [[ "$ifr_miss" != "200" ]]; then
            echo "FAIL $name If-Range miss status=$ifr_miss (want 200)" >&2
            exit 1
        fi
        echo "  ok If-Range miss -> 200"

        conn_h=$(curl -sS -D- -o /dev/null -H 'Connection: foo, close' \
            "http://127.0.0.1:${port}/4kb.html" \
            | awk 'BEGIN{IGNORECASE=1} /^Connection:/ {print}' | tr -d '\r')
        if [[ "$conn_h" != *close* ]]; then
            echo "FAIL $name Connection: foo, close -> '$conn_h'" >&2
            exit 1
        fi
        echo "  ok Connection token list -> $conn_h"

        check_ws "$port"
    fi
}

check_ws() {
    local port="$1"
    python3 - "$port" <<'PY'
import base64, hashlib, os, socket, struct, sys

port = int(sys.argv[1])
key = base64.b64encode(os.urandom(16)).decode()
guid = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
want = base64.b64encode(hashlib.sha1(key.encode() + guid).digest()).decode()

s = socket.create_connection(("127.0.0.1", port), 2)
req = (
    "GET /echo HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n"
).encode()
s.sendall(req)
buf = b""
while b"\r\n\r\n" not in buf:
    chunk = s.recv(4096)
    if not chunk:
        raise SystemExit("FAIL WS handshake closed")
    buf += chunk
head, rest = buf.split(b"\r\n\r\n", 1)
if b"101" not in head.split(b"\r\n", 1)[0]:
    raise SystemExit(f"FAIL WS status {head.split(b'\\n', 1)[0]!r}")
if want.encode() not in head:
    raise SystemExit("FAIL WS Accept mismatch")

def recvn(sock, n, extra):
    out = extra
    extra = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise SystemExit("FAIL WS short read")
        out += chunk
    return out, extra

def send_text(sock, text):
    payload = text.encode()
    mkey = os.urandom(4)
    masked = bytes(b ^ mkey[i % 4] for i, b in enumerate(payload))
    hdr = bytearray([0x81, 0x80 | len(payload)])
    hdr.extend(mkey)
    sock.sendall(bytes(hdr) + masked)

def read_unmasked(sock, extra):
    hdr, extra = recvn(sock, 2, extra)
    if (hdr[0] & 0x80) == 0:
        raise SystemExit("FAIL WS fragment")
    opcode = hdr[0] & 0x0F
    n = hdr[1] & 0x7F
    if hdr[1] & 0x80:
        raise SystemExit("FAIL WS server masked")
    if n == 126:
        ext, extra = recvn(sock, 2, extra)
        n = struct.unpack("!H", ext)[0]
    elif n == 127:
        raise SystemExit("FAIL WS huge frame")
    body, extra = recvn(sock, n, extra)
    return opcode, body, extra

send_text(s, "hello")
op, body, rest = read_unmasked(s, rest)
if op != 1 or body != b"hello":
    raise SystemExit(f"FAIL WS echo op={op} body={body!r}")
s.close()
PY
    echo "  ok WS echo hello"
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

    mkdir -p "$www/public"
    printf 'ok\n' > "$www/public/ok.txt"
    printf 'old\n' > "$www/swap.txt"
    ln -sfn /etc "$www/public/external"
    leaf_src=""
    if [[ -e /etc/hosts ]]; then leaf_src=/etc/hosts
    elif [[ -e /etc/passwd ]]; then leaf_src=/etc/passwd
    fi
    if [[ -n "$leaf_src" ]]; then
        ln -sfn "$leaf_src" "$www/public/secret"
        esc_name=$(basename "$leaf_src")
        ok_body=$(curl -sS "http://127.0.0.1:${list_port}/public/ok.txt")
        if [[ "$ok_body" != $'ok\n' && "$ok_body" != ok ]]; then
            echo "FAIL staticd jail peer /public/ok.txt body=$ok_body" >&2
            exit 1
        fi
        esc_code=$(curl --path-as-is -sS -o "$TMPDIR_RUN/esc.body" -w '%{http_code}' \
            "http://127.0.0.1:${list_port}/public/external/${esc_name}")
        if [[ "$esc_code" == "200" ]] || cmp -s "$TMPDIR_RUN/esc.body" "$leaf_src" 2>/dev/null; then
            echo "FAIL staticd intermediate symlink escape status=$esc_code" >&2
            exit 1
        fi
        echo "  ok intermediate symlink /public/external/${esc_name} -> $esc_code"
        esc_code=$(curl --path-as-is -sS -o "$TMPDIR_RUN/leaf.body" -w '%{http_code}' \
            "http://127.0.0.1:${list_port}/public/secret")
        if [[ "$esc_code" == "200" ]] || cmp -s "$TMPDIR_RUN/leaf.body" "$leaf_src" 2>/dev/null; then
            echo "FAIL staticd leaf symlink escape status=$esc_code" >&2
            exit 1
        fi
        echo "  ok leaf symlink /public/secret -> $esc_code"
        esc_code=$(curl --path-as-is -sS -o "$TMPDIR_RUN/escdir.body" -w '%{http_code}' \
            "http://127.0.0.1:${list_port}/public/external/")
        if [[ "$esc_code" == "200" ]] && grep -q hosts "$TMPDIR_RUN/escdir.body" 2>/dev/null; then
            echo "FAIL staticd symlink dir listing escaped" >&2
            exit 1
        fi
        echo "  ok symlink dir /public/external/ -> $esc_code"
    fi

    old_body=$(curl -sS "http://127.0.0.1:${list_port}/swap.txt")
    if [[ "$old_body" != $'old\n' && "$old_body" != old ]]; then
        echo "FAIL staticd swap.txt before rename body=$old_body" >&2
        exit 1
    fi
    printf 'new\n' > "$www/swap.txt.new"
    mv -f "$www/swap.txt.new" "$www/swap.txt"
    tick=$(date +%s)
    while [[ "$(date +%s)" -le "$tick" ]]; do
        curl -sS -o /dev/null "http://127.0.0.1:${list_port}/swap.txt" || true
        sleep 0.05
    done
    new_body=$(curl -sS "http://127.0.0.1:${list_port}/swap.txt")
    if [[ "$new_body" != $'new\n' && "$new_body" != new ]]; then
        echo "FAIL staticd rename still serving stale body=$new_body" >&2
        exit 1
    fi
    echo "  ok pathname revalidate after rename"

    # --pages: script replies (QuickJS / CPython). Never serve source.
    pages_port=$((STATICD_PORT + 11))
    off_code=$(curl -sS -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${STATICD_PORT}/hello" || true)
    if [[ "$off_code" != "404" ]]; then
        echo "FAIL staticd --pages off /hello status=$off_code (want 404)" >&2
        exit 1
    fi
    echo "  ok --pages off /hello -> 404"

    pages_js="$TMPDIR_RUN/pages_js"
    mkdir -p "$pages_js"
    cp "$SCRIPT_DIR/pages/hello.js" "$pages_js/hello.js"
    cp "$SCRIPT_DIR/pages/boom.js" "$pages_js/boom.js"
    "$STATICD_BIN" --listen "127.0.0.1:${pages_port}" --root "$www" \
        --pages "$pages_js" --workers 1 \
        >"$TMPDIR_RUN/staticd-pages-js.log" 2>&1 &
    PIDS+=($!)
    wait_port "$pages_port" staticd-pages-js
    js_code=$(curl -sS -o "$TMPDIR_RUN/hello.js.body" -w '%{http_code}' \
        "http://127.0.0.1:${pages_port}/hello")
    js_body=$(cat "$TMPDIR_RUN/hello.js.body"; printf x); js_body=${js_body%x}
    if [[ "$js_code" == "501" ]]; then
        echo "  SKIP pages js (QuickJS not attached; set CC_QUICKJS_SRC)"
    elif [[ "$js_code" != "200" || "$js_body" != $'hello /hello\n' ]]; then
        echo "FAIL staticd pages js status=$js_code body=$(printf %q "$js_body")" >&2
        cat "$TMPDIR_RUN/staticd-pages-js.log" >&2 || true
        exit 1
    else
        echo "  ok pages hello.js -> 200"
        boom_code=$(curl -sS -o /dev/null -w '%{http_code}' \
            "http://127.0.0.1:${pages_port}/boom")
        if [[ "$boom_code" != "500" ]]; then
            echo "FAIL staticd pages throw status=$boom_code (want 500)" >&2
            exit 1
        fi
        echo "  ok pages throw -> 500"
        # must not serve .js as static from --root when pages miss
        src_code=$(curl -sS -o "$TMPDIR_RUN/nosrc.body" -w '%{http_code}' \
            "http://127.0.0.1:${pages_port}/hello.js")
        if [[ "$src_code" == "200" ]] && grep -q 'export function' "$TMPDIR_RUN/nosrc.body" 2>/dev/null; then
            echo "FAIL staticd served pages source as static" >&2
            exit 1
        fi
        echo "  ok pages source not served as static ($src_code)"
    fi

    pages_py="$TMPDIR_RUN/pages_py"
    mkdir -p "$pages_py"
    cp "$SCRIPT_DIR/pages/hello.py" "$pages_py/hello.py"
    pages_py_port=$((STATICD_PORT + 12))
    "$STATICD_BIN" --listen "127.0.0.1:${pages_py_port}" --root "$www" \
        --pages "$pages_py" --workers 1 \
        >"$TMPDIR_RUN/staticd-pages-py.log" 2>&1 &
    PIDS+=($!)
    wait_port "$pages_py_port" staticd-pages-py
    py_code=$(curl -sS -o "$TMPDIR_RUN/hello.py.body" -w '%{http_code}' \
        "http://127.0.0.1:${pages_py_port}/hello")
    py_body=$(cat "$TMPDIR_RUN/hello.py.body"; printf x); py_body=${py_body%x}
    if [[ "$py_code" == "501" ]]; then
        echo "  SKIP pages py (libpython not attached)"
    elif [[ "$py_code" != "200" || "$py_body" != $'hello /hello\n' ]]; then
        echo "FAIL staticd pages py status=$py_code body=$(printf %q "$py_body")" >&2
        cat "$TMPDIR_RUN/staticd-pages-py.log" >&2 || true
        exit 1
    else
        echo "  ok pages hello.py -> 200"
    fi

    pages_both="$TMPDIR_RUN/pages_both"
    mkdir -p "$pages_both"
    cp "$SCRIPT_DIR/pages/hello.js" "$pages_both/hello.js"
    cp "$SCRIPT_DIR/pages/hello.py" "$pages_both/hello.py"
    pages_both_port=$((STATICD_PORT + 13))
    "$STATICD_BIN" --listen "127.0.0.1:${pages_both_port}" --root "$www" \
        --pages "$pages_both" --workers 1 \
        >"$TMPDIR_RUN/staticd-pages-both.log" 2>&1 &
    PIDS+=($!)
    wait_port "$pages_both_port" staticd-pages-both
    both_code=$(curl -sS -o /dev/null -w '%{http_code}' \
        "http://127.0.0.1:${pages_both_port}/hello")
    if [[ "$both_code" != "500" ]]; then
        echo "FAIL staticd pages both faces status=$both_code (want 500)" >&2
        exit 1
    fi
    echo "  ok pages both .js+.py -> 500"

    # TLS: BearSSL sample RSA EE. SKIP if runtime was built without TLS.
    tls_port=$((STATICD_PORT + 14))
    tls_cert="$SCRIPT_DIR/../../third_party/bearssl/samples/cert-ee-rsa.pem"
    tls_key="$SCRIPT_DIR/../../third_party/bearssl/samples/key-ee-rsa.pem"
    if [[ ! -f "$tls_cert" || ! -f "$tls_key" ]]; then
        echo "  SKIP tls (BearSSL sample PEMs missing)"
    else
        set +e
        "$STATICD_BIN" --listen "127.0.0.1:${tls_port}" --root "$www" \
            --tls-cert "$tls_cert" --tls-key "$tls_key" --workers 1 \
            >"$TMPDIR_RUN/staticd-tls.log" 2>&1 &
        tls_pid=$!
        PIDS+=($tls_pid)
        set -e
        tls_up=0
        for i in $(seq 1 50); do
            if curl -skS -o /dev/null --connect-timeout 0.2 \
                "https://127.0.0.1:${tls_port}/" 2>/dev/null; then
                tls_up=1
                break
            fi
            if ! kill -0 "$tls_pid" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done
        if [[ "$tls_up" != "1" ]]; then
            if grep -qi 'tls load failed\|undefined symbol\|Symbol not found\|cc_tls_server_load' \
                "$TMPDIR_RUN/staticd-tls.log" 2>/dev/null; then
                echo "  SKIP tls (runtime without BearSSL / load failed)"
                cat "$TMPDIR_RUN/staticd-tls.log" >&2 || true
            else
                echo "FAIL staticd tls did not accept on :${tls_port}" >&2
                cat "$TMPDIR_RUN/staticd-tls.log" >&2 || true
                exit 1
            fi
        else
            tls_code=$(curl -skS -o "$TMPDIR_RUN/tls.body" -w '%{http_code}' \
                "https://127.0.0.1:${tls_port}/public/ok.txt")
            tls_body=$(cat "$TMPDIR_RUN/tls.body"; printf x); tls_body=${tls_body%x}
            if [[ "$tls_code" != "200" || "$tls_body" != $'ok\n' ]]; then
                echo "FAIL staticd tls /public/ok.txt status=$tls_code body=$(printf %q "$tls_body")" >&2
                cat "$TMPDIR_RUN/staticd-tls.log" >&2 || true
                exit 1
            fi
            echo "  ok tls https /public/ok.txt -> 200"
        fi
    fi
fi

echo "correctness: PASS"
