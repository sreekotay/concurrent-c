#!/usr/bin/env bash
# Compare dated stock/cc baselines as one table.
# Does not re-run libcurl; pass --queue to include blocked join/detach.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="$SCRIPT_DIR/benchmarks"
MD=0
RUN_QUEUE=0
STOCK=""
CC=""

usage() {
    cat <<'EOF'
usage: ./report.sh [--md] [--queue] [--stock FILE] [--cc FILE]

  default   latest benchmarks/baseline_{stock,cc}_*.txt
  --md      markdown table
  --queue   run make queue-smoke and add join/detach rows
  --stock / --cc
            explicit baseline files
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --md) MD=1; shift ;;
        --queue) RUN_QUEUE=1; shift ;;
        --stock) STOCK="$2"; shift 2 ;;
        --cc) CC="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

latest() {
    local kind="$1"
    ls -t "$BENCH_DIR"/baseline_"$kind"_*.txt 2>/dev/null | head -1
}

if [[ -z "$STOCK" ]]; then
    STOCK="$(latest stock || true)"
fi
if [[ -z "$CC" ]]; then
    CC="$(latest cc || true)"
fi
if [[ -z "$STOCK" || ! -f "$STOCK" ]]; then
    echo "missing stock baseline — run: make stock && make baseline-save" >&2
    exit 1
fi
if [[ -z "$CC" || ! -f "$CC" ]]; then
    echo "missing cc baseline — run: make cc && make baseline-save" >&2
    exit 1
fi

# Last matching line; print the key= value (strips a trailing unit if asked).
kv() {
    local file="$1" linepat="$2" key="$3"
    awk -v p="$linepat" -v k="$key" '
        $0 ~ p {
            for (i = 1; i <= NF; i++) {
                if ($i ~ ("^" k "=")) {
                    v = $i
                    sub("^" k "=", "", v)
                    found = v
                }
            }
        }
        END { if (found != "") print found }
    ' "$file"
}

line() {
    grep -E "$2" "$1" | tail -1 || true
}

meta() {
    sed -n "s/^$2: //p" "$1" | tail -1
}

STOCK_PIN="$(meta "$STOCK" pin)"
CC_PIN="$(meta "$CC" pin)"
STOCK_N="$(meta "$STOCK" N)"
CC_N="$(meta "$CC" N)"
STOCK_R="$(meta "$STOCK" rounds)"
CC_R="$(meta "$CC" rounds)"
STOCK_HOST="$(meta "$STOCK" host)"
CC_HOST="$(meta "$CC" host)"
STOCK_DATE="$(meta "$STOCK" date)"
CC_DATE="$(meta "$CC" date)"

STOCK_ID="$(line "$STOCK" '^identity:')"
CC_ID="$(line "$CC" '^identity:')"
STOCK_NX="$(kv "$STOCK" 'cli_nxdomain_exit=' 'cli_nxdomain_exit')"
CC_NX="$(kv "$CC" 'cli_nxdomain_exit=' 'cli_nxdomain_exit')"
STOCK_SUM="$(line "$STOCK" 'baseline_dns summary:')"
CC_SUM="$(line "$CC" 'baseline_dns summary:')"

STOCK_SEQ_OK="$(kv "$STOCK" '^sequential:' ok)"
CC_SEQ_OK="$(kv "$CC" '^sequential:' ok)"
STOCK_SEQ_FAIL="$(kv "$STOCK" '^sequential:' fail)"
CC_SEQ_FAIL="$(kv "$CC" '^sequential:' fail)"

STOCK_C_FAIL="$(kv "$STOCK" '^concurrent_summary:' fail_rounds)"
CC_C_FAIL="$(kv "$CC" '^concurrent_summary:' fail_rounds)"
STOCK_C_MED="$(kv "$STOCK" '^concurrent_summary:' wall_median)"
CC_C_MED="$(kv "$CC" '^concurrent_summary:' wall_median)"
STOCK_C_MIN="$(kv "$STOCK" '^concurrent_summary:' wall_min)"
CC_C_MIN="$(kv "$CC" '^concurrent_summary:' wall_min)"
STOCK_C_MAX="$(kv "$STOCK" '^concurrent_summary:' wall_max)"
CC_C_MAX="$(kv "$CC" '^concurrent_summary:' wall_max)"
STOCK_DNS="$(kv "$STOCK" '^concurrent_summary:' dns_mean_median)"
CC_DNS="$(kv "$CC" '^concurrent_summary:' dns_mean_median)"

STOCK_A_FAIL="$(kv "$STOCK" '^abort_summary:' fail_rounds)"
CC_A_FAIL="$(kv "$CC" '^abort_summary:' fail_rounds)"
STOCK_A_MED="$(kv "$STOCK" '^abort_summary:' wall_median)"
CC_A_MED="$(kv "$CC" '^abort_summary:' wall_median)"
STOCK_A_MIN="$(kv "$STOCK" '^abort_summary:' wall_min)"
CC_A_MIN="$(kv "$CC" '^abort_summary:' wall_min)"
STOCK_A_MAX="$(kv "$STOCK" '^abort_summary:' wall_max)"
CC_A_MAX="$(kv "$CC" '^abort_summary:' wall_max)"

MATCH=1
NOTE_SIZE=""
if [[ "$STOCK_N" != "$CC_N" || "$STOCK_R" != "$CC_R" ]]; then
    MATCH=0
    NOTE_SIZE="N/rounds differ — do not line the medians up"
fi

# Overlapping [min,max] or small absolute gap → same band (DNS noise).
band() {
    local s="$1" c="$2" smin="$3" smax="$4" cmin="$5" cmax="$6"
    awk -v s="$s" -v c="$c" -v smin="$smin" -v smax="$smax" -v cmin="$cmin" -v cmax="$cmax" '
        function n(x) { gsub(/s$/, "", x); return x + 0 }
        BEGIN {
            if (s == "" || c == "") { print "—"; exit }
            ds = n(s); dc = n(c)
            if (n(smax) >= n(cmin) && n(cmax) >= n(smin)) { print "same band"; exit }
            d = dc - ds
            if (d < 0) d = -d
            if (d < 0.020) { print "same band"; exit }
            if (d > 0) printf "cc %+.3fs\n", dc - ds
            else print "—"
        }
    '
}

ok_or() {
    local v="$1"
    if [[ "$v" == *OK* ]]; then
        echo OK
    elif [[ -n "$v" ]]; then
        echo "$v"
    else
        echo "—"
    fi
}

JOIN="—"
DETACH="—"
KNOBS="—"
if [[ "$RUN_QUEUE" -eq 1 ]]; then
    QOUT="$(make -C "$SCRIPT_DIR" -s queue-smoke)"
    KNOBS="$(echo "$QOUT" | sed -n 's/.*knobs OK (max_inflight=\([0-9]*\)).*/max_inflight=\1/p' | tail -1)"
    JOIN="$(echo "$QOUT" | sed -n 's/.*join_blocked OK (join=\([^)]*\)).*/\1/p' | tail -1)"
    DETACH="$(echo "$QOUT" | sed -n 's/.*detach_blocked OK (return=\([^)]*\)).*/\1/p' | tail -1)"
    [[ -n "$KNOBS" ]] || KNOBS="FAIL"
    [[ -n "$JOIN" ]] || JOIN="FAIL"
    [[ -n "$DETACH" ]] || DETACH="FAIL"
fi

C_NOTE="fast path"
if [[ "$MATCH" -eq 1 ]]; then
    C_NOTE="$(band "$STOCK_C_MED" "$CC_C_MED" "$STOCK_C_MIN" "$STOCK_C_MAX" "$CC_C_MIN" "$CC_C_MAX")"
    D_NOTE="$(band "$STOCK_DNS" "$CC_DNS" "$STOCK_DNS" "$STOCK_DNS" "$CC_DNS" "$CC_DNS")"
else
    D_NOTE="$NOTE_SIZE"
    C_NOTE="$NOTE_SIZE"
fi
A_NOTE="fast path (not a blocked join)"

axis_id="$(ok_or "$STOCK_ID")"
axis_id_cc="$(ok_or "$CC_ID")"
[[ "$STOCK_ID" == *AsynchDNS* ]] && axis_id="AsynchDNS"
[[ "$CC_ID" == *AsynchDNS* ]] && axis_id_cc="AsynchDNS"

seq_s="${STOCK_SEQ_OK:-—}/${STOCK_SEQ_FAIL:-—}"
seq_c="${CC_SEQ_OK:-—}/${CC_SEQ_FAIL:-—}"
[[ "$seq_s" == "—/—" ]] && seq_s="—"
[[ "$seq_c" == "—/—" ]] && seq_c="—"

SUM_S="$(ok_or "$STOCK_SUM")"
SUM_C="$(ok_or "$CC_SUM")"

rows() {
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4"
}

TABLE="$(
    rows "identity" "$axis_id" "$axis_id_cc" ""
    rows "nxdomain" "exit ${STOCK_NX:-—}" "exit ${CC_NX:-—}" ""
    rows "harness" "$SUM_S" "$SUM_C" ""
    rows "sequential ok/fail" "$seq_s" "$seq_c" ""
    rows "concurrent fail_rounds" "${STOCK_C_FAIL:-—}" "${CC_C_FAIL:-—}" ""
    rows "concurrent wall_median" "${STOCK_C_MED:-—}" "${CC_C_MED:-—}" "$C_NOTE"
    rows "concurrent wall range" "${STOCK_C_MIN:-—}–${STOCK_C_MAX:-—}" "${CC_C_MIN:-—}–${CC_C_MAX:-—}" ""
    rows "concurrent dns_mean" "${STOCK_DNS:-—}" "${CC_DNS:-—}" "$D_NOTE"
    rows "abort fail_rounds" "${STOCK_A_FAIL:-—}" "${CC_A_FAIL:-—}" ""
    rows "abort wall_median" "${STOCK_A_MED:-—}" "${CC_A_MED:-—}" "$A_NOTE"
    rows "abort wall range" "${STOCK_A_MIN:-—}–${STOCK_A_MAX:-—}" "${CC_A_MIN:-—}–${CC_A_MAX:-—}" ""
    if [[ "$RUN_QUEUE" -eq 1 ]]; then
        rows "queue knobs" "—" "${KNOBS:-—}" "CC queue only"
        rows "join_blocked" "—" "${JOIN:-—}" "must wait for in-flight process"
        rows "detach_blocked" "—" "${DETACH:-—}" "must return before process ends"
    fi
)"

emit_plain() {
    echo "curl DNS port"
    echo "  pin     stock=${STOCK_PIN:-—}  cc=${CC_PIN:-—}"
    echo "  N/R     stock=${STOCK_N:-—}/${STOCK_R:-—}  cc=${CC_N:-—}/${CC_R:-—}"
    echo "  host    stock=${STOCK_HOST:-—}"
    echo "          cc   =${CC_HOST:-—}"
    echo "  date    stock=${STOCK_DATE:-—}"
    echo "          cc   =${CC_DATE:-—}"
    echo "  files   ${STOCK##*/}"
    echo "          ${CC##*/}"
    if [[ "$MATCH" -eq 0 ]]; then
        echo
        echo "  warning: $NOTE_SIZE"
    fi
    echo
    printf '%-24s  %-16s  %-16s  %s\n' "axis" "stock" "cc" "note"
    printf '%-24s  %-16s  %-16s  %s\n' "------------------------" "----------------" "----------------" "----"
    echo "$TABLE" | while IFS=$'\t' read -r a s c n; do
        printf '%-24s  %-16s  %-16s  %s\n' "$a" "$s" "$c" "$n"
    done
}

emit_md() {
    echo "# curl DNS port"
    echo
    echo "| | stock | cc |"
    echo "| --- | --- | --- |"
    echo "| pin | ${STOCK_PIN:-—} | ${CC_PIN:-—} |"
    echo "| N / rounds | ${STOCK_N:-—} / ${STOCK_R:-—} | ${CC_N:-—} / ${CC_R:-—} |"
    echo "| host | ${STOCK_HOST:-—} | ${CC_HOST:-—} |"
    echo "| date | ${STOCK_DATE:-—} | ${CC_DATE:-—} |"
    echo "| file | \`${STOCK##*/}\` | \`${CC##*/}\` |"
    if [[ "$MATCH" -eq 0 ]]; then
        echo
        echo "> warning: $NOTE_SIZE"
    fi
    echo
    echo "| axis | stock | cc | note |"
    echo "| --- | --- | --- | --- |"
    echo "$TABLE" | while IFS=$'\t' read -r a s c n; do
        echo "| $a | $s | $c | $n |"
    done
}

if [[ "$MD" -eq 1 ]]; then
    emit_md
else
    emit_plain
fi
