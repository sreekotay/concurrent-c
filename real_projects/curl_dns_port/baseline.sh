#!/usr/bin/env bash
# Stock (or CC-swapped) DNS baseline for curl_dns_port.
# See BASELINE.md for the contract.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$SCRIPT_DIR/out/prefix}"
CURL_BIN="${CURL_BIN:-$PREFIX/bin/curl}"
HARNESS="${HARNESS:-$SCRIPT_DIR/out/baseline_dns}"
LIBCURL_A="${LIBCURL_A:-$PREFIX/lib/libcurl.a}"
BENCH_DIR="$SCRIPT_DIR/benchmarks"
N="${BASELINE_N:-32}"
ROUNDS="${BASELINE_ROUNDS:-10}"
SAVE=0
STAMP="$(date +%Y_%m_%d)"

flavor() {
    if [[ -f "$LIBCURL_A" ]] && nm "$LIBCURL_A" 2>/dev/null | grep cc_curl_thrdq | grep -vq ' U '; then
        echo cc
    else
        echo stock
    fi
}

usage() {
    echo "usage: $0 [--save] [N [ROUNDS]]"
    echo "  --save    write benchmarks/baseline_<flavor>_YYYY_MM_DD.txt"
    echo "  N         concurrent/abort count (default: $N / \$BASELINE_N)"
    echo "  ROUNDS    repeat concurrent+abort for median (default: $ROUNDS / \$BASELINE_ROUNDS)"
    echo "  flavor    read from libcurl.a (cc_curl_thrdq → cc, else stock)"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --save) SAVE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *)
            if [[ -z "${N_SET:-}" ]]; then
                N="$1"; N_SET=1
            else
                ROUNDS="$1"
            fi
            shift
            ;;
    esac
done

if [[ ! -x "$CURL_BIN" ]]; then
    echo "missing $CURL_BIN — run: make upstream"
    exit 1
fi
if [[ ! -x "$HARNESS" ]]; then
    echo "missing $HARNESS — run: make out/baseline_dns"
    exit 1
fi

FLAVOR="$(flavor)"
mkdir -p "$BENCH_DIR"
REPORT=""
if [[ "$SAVE" -eq 1 ]]; then
    REPORT="$BENCH_DIR/baseline_${FLAVOR}_${STAMP}.txt"
    : > "$REPORT"
fi

run() {
    if [[ -n "$REPORT" ]]; then
        tee -a "$REPORT"
    else
        cat
    fi
}

{
    echo "=== curl DNS baseline ($FLAVOR) ==="
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host: $(uname -s) $(uname -m) $(uname -r)"
    echo "pin: $(tr -d '[:space:]' < "$SCRIPT_DIR/CURL_VERSION")"
    echo "flavor: $FLAVOR"
    echo "curl_bin: $CURL_BIN"
    echo "N: $N"
    echo "rounds: $ROUNDS"
    echo
    echo "--- CLI --version ---"
    "$CURL_BIN" --version
    echo
    if ! "$CURL_BIN" --version | grep -q AsynchDNS; then
        echo "FAIL: AsynchDNS missing from Features"
        exit 1
    fi
    echo "identity: AsynchDNS present"
    echo
    echo "--- CLI happy path ---"
    "$CURL_BIN" -fsS -o /dev/null -w \
        "url=%{url_effective} http=%{http_code} dns=%{time_namelookup}s connect=%{time_connect}s total=%{time_total}s\n" \
        https://example.com/
    echo
    echo "--- CLI nxdomain (expect exit 6) ---"
    set +e
    "$CURL_BIN" -fsS -o /dev/null --max-time 10 https://no-such-host.invalid/
    nx=$?
    set -e
    echo "cli_nxdomain_exit=$nx"
    if [[ "$nx" -ne 6 ]]; then
        echo "FAIL: expected curl exit 6 (Couldn't resolve host), got $nx"
        exit 1
    fi
    echo "cli_nxdomain: OK"
    echo
    echo "--- libcurl harness ---"
} | run

# Harness prints its own sections; append if saving.
if [[ -n "$REPORT" ]]; then
    "$HARNESS" all "$N" "$ROUNDS" | tee -a "$REPORT"
    echo
    echo "wrote $REPORT"
else
    "$HARNESS" all "$N" "$ROUNDS"
fi
