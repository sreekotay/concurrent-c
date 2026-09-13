#!/usr/bin/env bash
# compare_table_wstore.sh — Table store vs wstore5 vs Go death-time twin
#
# Standing cell (quote this):
#   ./perf/compare_table_wstore.sh stand
# get N/N, 5s, θ=0.35, wave=512, repeats=3.
#
#   ./perf/compare_table_wstore.sh stand
#   ./perf/compare_table_wstore.sh stand 5
#   REPEATS=1 ./perf/compare_table_wstore.sh stand
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
SEC=1
if [ "${1:-}" = "stand" ] || [ "${1:-}" = "standing" ]; then
    shift
    MIXES="${MIXES:-get}"
    SHAPES="${SHAPES:-n}"
    REPEATS="${REPEATS:-3}"
    SEC="${1:-5}"
elif [ -n "${1:-}" ]; then
    SEC="$1"
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

echo "# compare_table_wstore seconds=$SEC ncpu=$NCPU mixes=$MIXES shapes=$SHAPES repeats=$REPEATS"
echo "# build"
"$CCC" --release -o "$OUT/wstore_table" "$SCRIPT_DIR/wstore_table.ccs"
"$CCC" --release -o "$OUT/wstore5" "$SCRIPT_DIR/wstore5.ccs"
go build -o "$OUT/wstore3_go" "$SCRIPT_DIR/go/wstore3.go"

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
        for tag in tbl cc5 go; do
            for sweep in 0 1; do
                JOBS+=("$tag $sweep $sh $cl $mix")
            done
        done
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
    "$bin" "$SEC" "$sweep" 0.35 512 "$shards" "$clients" "$mix" \
        > "$OUT/$rec"
    grep -E '^# (ops=|expire:|index:|compact:|keys:|waves=|mem:|hitch |wstore)' "$OUT/$rec" || true
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
    idle_c = end_c = end_r = trim_r = None
    end_live = trim_live = None
    for mm in mem_re.finditer(text):
        kind, ck, rk = mm.group(1), int(mm.group(2)), mm.group(3)
        rk = None if rk == "?" else int(rk)
        if kind == "idle":
            idle_c = ck
        elif kind == "trim":
            trim_r = rk
        else:
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
        "idle_c": idle_c,
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
        "dropped": med([r["dropped"] for r in recs]),
        "ok": all(r["ok"] for r in recs),
        "idle_c": med([r["idle_c"] for r in recs]),
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

def fmt_tax(t):
    return "    ?" if t is None else f"{t:+6.0f}%"

def fmt_drop(n):
    if n is None: return "?"
    if n >= 1_000_000: return f"{n/1e6:.1f}M"
    if n >= 1000: return f"{n/1e3:.0f}k"
    return f"{n:.0f}"

def fmt_kib(n):
    if n is None: return "     ?"
    if n >= 1024: return f"{n/1024:6.1f}M"
    return f"{n:6.0f}k"

def fmt_keys(n):
    if n is None: return "     ?"
    if n >= 1_000_000: return f"{n/1e6:5.2f}M"
    if n >= 1000: return f"{n/1e3:5.1f}k"
    return f"{n:6.0f}"

print()
print(f"# summary  seconds={sec} ncpu={ncpu}  repeats={nrep}")
print(f"# {'mix':<6} {'shape':<6} {'tbl off':>7} {'tbl on':>7} {'tax':>7} "
      f"{'5 off':>7} {'5 on':>7} {'5 tax':>7} {'on t/5':>8} "
      f"{'Go off':>7} {'Go on':>7} {'Go tax':>7} {'on t/Go':>8} {'hit%':>5}")
missing = 0
for mix, sh, cl in order:
    def get(lang, sweep):
        return rows.get((mix, sh, cl, lang, sweep))
    t0, t1 = get("tbl", "0"), get("tbl", "1")
    a0, a1 = get("cc5", "0"), get("cc5", "1")
    g0, g1 = get("go", "0"), get("go", "1")
    if not all((t0, t1, a0, a1, g0, g1)):
        missing += 1
        print(f"# {mix:<6} {sh + '/' + cl:<6}  (incomplete)")
        continue
    ttax, atax, gtax = tax(t0, t1), tax(a0, a1), tax(g0, g1)
    r5 = t1["mops"] / a1["mops"] if a1["mops"] else 0.0
    rg = t1["mops"] / g1["mops"] if g1["mops"] else 0.0
    print(f"# {mix:<6} {sh + '/' + cl:<6} {t0['mops']:7.2f} {t1['mops']:7.2f} "
          f"{fmt_tax(ttax):>7} {a0['mops']:7.2f} {a1['mops']:7.2f} "
          f"{fmt_tax(atax):>7} {r5:8.2f} {g0['mops']:7.2f} {g1['mops']:7.2f} "
          f"{fmt_tax(gtax):>7} {rg:8.2f} {t1['hit']:5.0f}")
    if nrep > 1:
        print(f"# {'':<6} {'':<6}  [{t0['mops_lo']:.2f}-{t0['mops_hi']:.2f}] "
              f"[{t1['mops_lo']:.2f}-{t1['mops_hi']:.2f}]         "
              f"[{a0['mops_lo']:.2f}-{a0['mops_hi']:.2f}] "
              f"[{a1['mops_lo']:.2f}-{a1['mops_hi']:.2f}]")

print()
print(f"# {'mix':<6} {'shape':<6} {'sweep':<5} "
      f"{'tbl end':>8} {'tbl rss':>8} {'tbl keys':>8} "
      f"{'5 end':>8} {'5 rss':>8} {'5 keys':>8} "
      f"{'Go end':>8} {'Go rss':>8} {'Go keys':>8}")
for mix, sh, cl in order:
    for sweep, label in (("0", "off"), ("1", "on")):
        t = rows.get((mix, sh, cl, "tbl", sweep))
        a = rows.get((mix, sh, cl, "cc5", sweep))
        g = rows.get((mix, sh, cl, "go", sweep))
        if not all((t, a, g)):
            print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5}  (incomplete)")
            continue
        print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5} "
              f"{fmt_kib(t['end_c']):>8} {fmt_kib(t['end_r']):>8} {fmt_keys(t['live']):>8} "
              f"{fmt_kib(a['end_c']):>8} {fmt_kib(a['end_r']):>8} {fmt_keys(a['live']):>8} "
              f"{fmt_kib(g['end_c']):>8} {fmt_kib(g['end_r']):>8} {fmt_keys(g['live']):>8}")

print("# tbl = wstore_table (Table rows, in-place, dense TTL scan).")
print("# 5   = wstore5 (always-new gens + wheel).")
print("# Go  = wstore3.go death-time twin.")
print("# tax = (sweep-on − sweep-off) / sweep-off. on t/5 and on t/Go = sweep-on ratio.")
print("# Feature gaps: table has no wheel/never-pages/gen-reclaim; expect different mem+expire tax.")
if missing:
    sys.exit(1)
PY
