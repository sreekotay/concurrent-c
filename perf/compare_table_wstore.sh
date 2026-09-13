#!/usr/bin/env bash
# compare_table_wstore.sh — Table store vs wstore5 vs Go death-time twin
#
# Standing cell (quote this):
#   ./perf/compare_table_wstore.sh stand
# get N/N, 5s, θ=0.35, wave=512, repeats=3.
#
# Phase collect (tbl only — where time goes; not for quoting M/s):
#   ./perf/compare_table_wstore.sh phase
#   ./perf/compare_table_wstore.sh phase 5
#
#   ./perf/compare_table_wstore.sh stand
#   ./perf/compare_table_wstore.sh stand 5
#   REPEATS=1 ./perf/compare_table_wstore.sh stand
#
# In zsh without interactivecomments, do not put `# comment` on the same line
# (it becomes argv and seconds=# → 0s runs).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
SEC=1
MODE=bench
if [ "${1:-}" = "stand" ] || [ "${1:-}" = "standing" ]; then
    shift
    MODE=stand
    MIXES="${MIXES:-get}"
    SHAPES="${SHAPES:-n}"
    REPEATS="${REPEATS:-3}"
    SEC="${1:-5}"
elif [ "${1:-}" = "phase" ] || [ "${1:-}" = "collect" ]; then
    shift
    MODE=phase
    MIXES="${MIXES:-get}"
    SHAPES="${SHAPES:-1,n}"
    REPEATS="${REPEATS:-1}"
    SEC="${1:-5}"
elif [ -n "${1:-}" ]; then
    SEC="$1"
fi
# Reject zsh-interactive junk like `stand # comment` (INTERACTIVE_COMMENTS off).
if ! [[ "$SEC" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "seconds must be a number (got '$SEC'). Tip: in zsh, '#' is not a comment unless setopt interactivecomments." >&2
    echo "  ./perf/compare_table_wstore.sh stand" >&2
    echo "  ./perf/compare_table_wstore.sh stand 5" >&2
    exit 1
fi
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if [ "$NCPU" -lt 1 ]; then NCPU=1; fi
if [ "$NCPU" -gt 64 ]; then NCPU=64; fi

MIXES="${MIXES:-write}"
SHAPES="${SHAPES:-1}"
REPEATS="${REPEATS:-1}"
if ! [[ "$REPEATS" =~ ^[1-9][0-9]*$ ]]; then
    echo "REPEATS must be a positive integer (got $REPEATS)" >&2
    exit 1
fi

mkdir -p "$OUT"
JOBLIST="$OUT/table_wstore_this_run.jobs"
ORDER="$OUT/table_wstore_this_run.order"

echo "# compare_table_wstore mode=$MODE seconds=$SEC ncpu=$NCPU mixes=$MIXES shapes=$SHAPES repeats=$REPEATS"
echo "# build"
rm -f "$OUT/wstore_table"
"$CCC" --release -o "$OUT/wstore_table" "$SCRIPT_DIR/wstore_table.ccs"
if [ "$MODE" != "phase" ]; then
    rm -f "$OUT/wstore5" "$OUT/wstore3_go"
    "$CCC" --release -o "$OUT/wstore5" "$SCRIPT_DIR/wstore5.ccs"
    go build -o "$OUT/wstore3_go" "$SCRIPT_DIR/go/wstore3.go"
fi

JOBS=()
IFS=',' read -r -a MIX_ARR <<< "$MIXES"
IFS=',' read -r -a SHAPE_ARR <<< "$SHAPES"
for mix in "${MIX_ARR[@]}"; do
    mix="$(printf '%s' "$mix" | tr -d '[:space:]')"
    [ -n "$mix" ] || continue
    for shape in "${SHAPE_ARR[@]}"; do
        shape="$(printf '%s' "$shape" | tr -d '[:space:]')"
        case "$shape" in
            1) sh=1; cl=1 ;;
            n|N) sh="$NCPU"; cl="$NCPU" ;;
            *) echo "unknown SHAPES entry: $shape (use 1 or n)" >&2; exit 1 ;;
        esac
        if [ "$MODE" = "phase" ]; then
            for sweep in 0 1; do
                JOBS+=("tbl $sweep $sh $cl $mix")
            done
        else
            for tag in tbl cc5 go; do
                for sweep in 0 1; do
                    JOBS+=("$tag $sweep $sh $cl $mix")
                done
            done
        fi
    done
done

printf '%s\n' "${JOBS[@]}" | python3 -c '
import random, sys
rows = [ln for ln in sys.stdin.read().splitlines() if ln.strip()]
random.shuffle(rows)
sys.stdout.write("\n".join(rows) + "\n")
' > "$ORDER"

: > "$JOBLIST"
rep=0
while [ "$rep" -lt "$REPEATS" ]; do
    while IFS= read -r job || [ -n "$job" ]; do
        [ -n "$job" ] || continue
        set -- $job
        if [ "$REPEATS" -gt 1 ]; then
            echo "$1_$5_s$2_$3x$4_r${rep}.txt" >> "$JOBLIST"
        else
            echo "$1_$5_s$2_$3x$4.txt" >> "$JOBLIST"
        fi
    done < "$ORDER"
    rep=$((rep + 1))
done

echo "# ${#JOBS[@]} cells shuffled × ${REPEATS} (${SEC}s each)"

run() {
    local tag="$1" sweep="$2" shards="$3" clients="$4" mix="$5" rec="$6"
    local bin
    case "$tag" in
        tbl) bin="$OUT/wstore_table" ;;
        cc5) bin="$OUT/wstore5" ;;
        go)  bin="$OUT/wstore3_go" ;;
        *) echo "bad tag $tag" >&2; exit 1 ;;
    esac
    echo "# --- $tag mix=$mix sweep=$sweep ${shards}/${clients} rec=$rec ---"
    if [ "$MODE" = "phase" ]; then
        "$bin" "$SEC" "$sweep" 0.35 512 "$shards" "$clients" "$mix" phase \
            > "$OUT/$rec"
        grep -E '^# (ops=|expire:|compact:|mem: (end|trim)|wstore|phase:)' "$OUT/$rec" || true
    else
        "$bin" "$SEC" "$sweep" 0.35 512 "$shards" "$clients" "$mix" \
            > "$OUT/$rec"
        # One-liners only during the run; full summary prints at the end.
        grep -E '^# (ops=|mem: end|wstore)' "$OUT/$rec" || true
    fi
}

while IFS= read -r rec || [ -n "$rec" ]; do
    [ -n "$rec" ] || continue
    base="${rec%.txt}"
    base="${base%_r*}"
    tag="${base%%_*}"
    rest="${base#*_}"
    mix="${rest%%_s*}"
    shape="${rest#*_s}"
    sweep="${shape%%_*}"
    sc="${shape#*_}"
    shards="${sc%%x*}"
    clients="${sc#*x}"
    run "$tag" "$sweep" "$shards" "$clients" "$mix" "$rec"
done < "$JOBLIST"

if [ "$MODE" = "phase" ]; then
python3 - "$OUT" "$SEC" "$NCPU" "$JOBLIST" <<'PY'
import re, sys
from pathlib import Path

out = Path(sys.argv[1])
sec, ncpu = sys.argv[2], sys.argv[3]
wanted = [ln.strip() for ln in Path(sys.argv[4]).read_text().splitlines() if ln.strip()]
pat = re.compile(r"^tbl_(write|get|drain)_s([01])_(\d+)x(\d+)(?:_r(\d+))?\.txt$")
ops_re = re.compile(r"ops=\d+ \(([0-9.]+)M/s\)")
phase_re = re.compile(
    r"phase: sample=1/(\d+) map=([0-9.]+)ns \(([0-9.]+)%\) "
    r"ttl=([0-9.]+)ns \(([0-9.]+)%\) "
    r"alloc=([0-9.]+)ns \(([0-9.]+)%\) "
    r"hold=([0-9.]+)ns \(([0-9.]+)%\) "
    r"keyfmt=([0-9.]+)ns \(([0-9.]+)%\)"
)

print(f"\n# phase collect  seconds={sec} ncpu={ncpu}")
print("# shape  sweep   M/s   map%  hold%  key% alloc%  ttl%   map_ns  hold_ns")
rows = []
for name in wanted:
    m = pat.match(name)
    if not m:
        continue
    mix, sweep, sh, cl = m.group(1), m.group(2), m.group(3), m.group(4)
    text = (out / name).read_text()
    om = ops_re.search(text)
    pm = phase_re.search(text)
    if not om or not pm:
        continue
    rows.append((mix, f"{sh}/{cl}", sweep, float(om.group(1)), pm))
rows.sort(key=lambda r: (r[0], r[1], r[2]))
for mix, shape, sweep, mops, pm in rows:
    print(
        f"# {mix:5} {shape:>5}  {sweep}  {mops:5.2f}  "
        f"{float(pm.group(3)):5.1f}  {float(pm.group(9)):5.1f}  "
        f"{float(pm.group(11)):4.1f}  {float(pm.group(7)):5.1f}  "
        f"{float(pm.group(5)):4.1f}  "
        f"{float(pm.group(2)):7.1f}  {float(pm.group(8)):7.1f}"
    )
print("# M/s here includes timer tax — quote stand, not phase, for throughput.")
PY
    exit 0
fi

python3 - "$OUT" "$SEC" "$NCPU" "$JOBLIST" <<'PY'
import re, sys, statistics
from pathlib import Path

out = Path(sys.argv[1])
sec, ncpu = sys.argv[2], sys.argv[3]
wanted = [ln.strip() for ln in Path(sys.argv[4]).read_text().splitlines() if ln.strip()]
pat = re.compile(r"^(tbl|cc5|go)_(write|get|drain)_s([01])_(\d+)x(\d+)(?:_r(\d+))?\.txt$")
ops_re = re.compile(r"ops=\d+ \(([0-9.]+)M/s\).*get_hits=(\d+)/(\d+)")
exp_re = re.compile(r"expire: dropped=(\d+) drained_recs=(\d+)")
mem_re = re.compile(
    r"mem: (idle|end|trim) commit_KiB=(\d+) rss_KiB=(\d+|\?)"
)
keys_re = re.compile(r"^# keys: live_n=(\d+)(?: map_n=(\d+))?", re.M)
trim_keys_re = re.compile(r"^# keys: trim live_n=(\d+)(?: map_n=(\d+))?", re.M)

NAME = {"tbl": "Table", "cc5": "wstore5", "go": "Go"}
ORDER_LANG = ("tbl", "cc5", "go")

samples = {}
order = []
for name in wanted:
    m = pat.match(name)
    if not m:
        continue
    lang, mix, sweep, sh, cl = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
    text = (out / name).read_text()
    om = ops_re.search(text)
    em = exp_re.search(text)
    if not om:
        continue
    hits, gets = int(om.group(2)), int(om.group(3))
    dropped = int(em.group(1)) if em else 0
    drained = int(em.group(2)) if em else 0
    end_c = end_r = trim_r = None
    end_live = trim_live = None
    for mm in mem_re.finditer(text):
        kind, ck, rk = mm.group(1), int(mm.group(2)), mm.group(3)
        rk = None if rk == "?" else int(rk)
        if kind == "trim":
            trim_r = rk
        elif kind == "end":
            end_c, end_r = ck, rk
    km = keys_re.search(text)
    if km:
        end_live = int(km.group(1))
    tk = trim_keys_re.search(text)
    if tk:
        trim_live = int(tk.group(1))
    shape = (mix, sh, cl)
    if shape not in order:
        order.append(shape)
    key = (mix, sh, cl, lang, sweep)
    samples.setdefault(key, []).append({
        "mops": float(om.group(1)),
        "hit": (100.0 * hits / gets) if gets else 0.0,
        "dropped": dropped,
        "ok": dropped == drained,
        "end_c": end_c,
        "end_r": end_r,
        "live": end_live,
        "trim_r": trim_r,
        "trim_live": trim_live,
    })

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def collapse(recs):
    if not recs:
        return None
    return {
        "mops": med([r["mops"] for r in recs]),
        "mops_lo": min(r["mops"] for r in recs),
        "mops_hi": max(r["mops"] for r in recs),
        "n": len(recs),
        "hit": med([r["hit"] for r in recs]),
        "ok": all(r["ok"] for r in recs),
        "end_c": med([r["end_c"] for r in recs]),
        "end_r": med([r["end_r"] for r in recs]),
        "live": med([r["live"] for r in recs]),
        "trim_r": med([r["trim_r"] for r in recs]),
        "trim_live": med([r["trim_live"] for r in recs]),
    }

rows = {k: collapse(v) for k, v in samples.items()}
nrep = max((len(v) for v in samples.values()), default=1)

def tax(off, on):
    if not off or not off["mops"]:
        return None
    return 100.0 * (on["mops"] - off["mops"]) / off["mops"]

def fmt_mops(x):
    return "   —" if x is None else f"{x:5.2f}"

def fmt_tax(t):
    return "   —" if t is None else f"{t:+4.0f}%"

def fmt_ratio(a, b):
    if not a or not b or not b["mops"]:
        return "   —"
    return f"{a['mops'] / b['mops']:.2f}×"

def fmt_mib(kib):
    if kib is None:
        return "    —"
    return f"{kib / 1024:6.0f}M"

def fmt_keys(n):
    if n is None:
        return "     —"
    if n >= 1_000_000:
        return f"{n / 1e6:5.2f}M"
    if n >= 1000:
        return f"{n / 1e3:5.1f}k"
    return f"{n:6.0f}"

def rng(r):
    if not r or nrep < 2:
        return ""
    return f"  [{r['mops_lo']:.2f}–{r['mops_hi']:.2f}]"

print()
print("# ══════════════════════════════════════════════════════════════")
print(f"#  STANDING SUMMARY   {sec}s × {nrep}  ncpu={ncpu}")
print("# ══════════════════════════════════════════════════════════════")

missing = 0
for mix, sh, cl in order:
    def get(lang, sweep):
        return rows.get((mix, sh, cl, lang, sweep))

    cells = {lang: (get(lang, "0"), get(lang, "1")) for lang in ORDER_LANG}
    if not all(cells[L][0] and cells[L][1] for L in ORDER_LANG):
        missing += 1
        print(f"#\n# {mix} {sh}/{cl}  (incomplete — missing receipts)")
        continue

    t0, t1 = cells["tbl"]
    a0, a1 = cells["cc5"]
    g0, g1 = cells["go"]
    hit = t1["hit"] if t1 else 0

    print(f"#")
    print(f"#  {mix}  {sh}/{cl}   hit {hit:.0f}%")
    print(f"#")
    print(f"#  Throughput (M/s, median)")
    print(f"#                 off      on     tax")
    for lang in ORDER_LANG:
        off, on = cells[lang]
        print(f"#    {NAME[lang]:<8}  {fmt_mops(off['mops'])}  {fmt_mops(on['mops'])}  {fmt_tax(tax(off, on))}"
              f"{rng(on)}")
    print(f"#")
    print(f"#  Sweep-on ratio")
    print(f"#    Table / Go       {fmt_ratio(t1, g1)}")
    print(f"#    Table / wstore5  {fmt_ratio(t1, a1)}")
    print(f"#    wstore5 / Go     {fmt_ratio(a1, g1)}")
    print(f"#")
    print(f"#  Memory at end (commit / RSS / live keys)")
    print(f"#                 sweep-off                      sweep-on")
    print(f"#              commit   RSS   keys           commit   RSS   keys")
    for lang in ORDER_LANG:
        off, on = cells[lang]
        print(
            f"#    {NAME[lang]:<8} "
            f"{fmt_mib(off['end_c'])} {fmt_mib(off['end_r'])} {fmt_keys(off['live'])}         "
            f"{fmt_mib(on['end_c'])} {fmt_mib(on['end_r'])} {fmt_keys(on['live'])}"
        )
    print(f"#")
    tr = (t1["end_r"] / g1["end_r"]) if (t1.get("end_r") and g1.get("end_r")) else None
    ar = (a1["end_r"] / g1["end_r"]) if (a1.get("end_r") and g1.get("end_r")) else None
    tr_s = "—" if tr is None else f"{tr:.2f}×"
    ar_s = "—" if ar is None else f"{ar:.2f}×"
    print(f"#  RSS vs Go (sweep-on):  Table {tr_s}   wstore5 {ar_s}")

print("#")
print("# ──────────────────────────────────────────────────────────────")
print("#  Table = wstore_table (Table + CCTemporalIndex, in-place rows)")
print("#  wstore5 = always-new gens + wheel")
print("#  Go = wstore3.go death-time twin")
print("#  tax = (on − off) / off.  Quote this block, not the per-cell log.")
print("# ══════════════════════════════════════════════════════════════")
if missing:
    sys.exit(1)
if any(
    (r.get("mops") or 0) <= 0
    for r in rows.values()
    if r
):
    print("# WARNING: some cells reported 0 M/s — check seconds (not a shell '#comment').", file=sys.stderr)
    sys.exit(1)
PY
