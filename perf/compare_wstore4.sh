#!/usr/bin/env bash
# compare_wstore4.sh — wstore3 (value-in-wheel) vs wstore4 (CCTemporalIndex)
#
# Standing cell (quote this; do not invent another command):
#   ./perf/compare_wstore4.sh stand
# get N/N, 5s, θ=0.35, wave=512, repeats=3. Override REPEATS / seconds after stand.
#
# Default without stand is the tax smoke: write 1/1 × {cc3, cc4} × sweep {off, on}, 1s.
#
#   ./perf/compare_wstore4.sh stand
#   ./perf/compare_wstore4.sh stand 5
#   REPEATS=1 ./perf/compare_wstore4.sh stand
#   ./perf/compare_wstore4.sh [seconds]
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

if [ "${FULL:-0}" = 1 ]; then
    MIXES="${MIXES:-write,get}"
    SHAPES="${SHAPES:-1,n}"
fi
MIXES="${MIXES:-write}"
SHAPES="${SHAPES:-1}"
REPEATS="${REPEATS:-1}"
if ! [[ "$REPEATS" =~ ^[1-9][0-9]*$ ]]; then
    echo "REPEATS must be a positive integer (got $REPEATS)" >&2
    exit 1
fi

mkdir -p "$OUT"
JOBLIST="$OUT/wstore4_this_run.jobs"
ORDER="$OUT/wstore4_this_run.order"

echo "# compare_wstore4 seconds=$SEC ncpu=$NCPU mixes=$MIXES shapes=$SHAPES repeats=$REPEATS CC_WORKERS=${CC_WORKERS:-}"
echo "# build"
"$CCC" --release -o "$OUT/wstore3" "$SCRIPT_DIR/wstore3.ccs"
"$CCC" --release -o "$OUT/wstore4" "$SCRIPT_DIR/wstore4.ccs"

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
        for tag in cc3 cc4; do
            for sweep in 0 1; do
                JOBS+=("$tag $sweep $sh $cl $mix")
            done
        done
    done
done

if [ "${#JOBS[@]}" -eq 0 ]; then
    echo "no jobs (MIXES=$MIXES SHAPES=$SHAPES)" >&2
    exit 1
fi

printf '%s\n' "${JOBS[@]}" | python3 -c '
import random, sys
rows = [ln for ln in sys.stdin.read().splitlines() if ln.strip()]
random.shuffle(rows)
sys.stdout.write("\n".join(rows))
sys.stdout.write("\n")
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
    if [ "$tag" = cc3 ]; then
        bin="$OUT/wstore3"
    else
        bin="$OUT/wstore4"
    fi
    echo "# --- $tag mix=$mix sweep=$sweep ${shards}/${clients} rec=$rec ---"
    "$bin" "$SEC" "$sweep" 0.35 512 "$shards" "$clients" "$mix" \
        > "$OUT/$rec"
    grep -E '^# (ops=|expire:|index:|compact:|keys:|waves=|mem:|hitch |wstore3|wstore4)' "$OUT/$rec"
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
pat = re.compile(r"^(cc3|cc4)_(write|get|drain)_s([01])_(\d+)x(\d+)(?:_r(\d+))?\.txt$")
ops_re = re.compile(r"ops=\d+ \(([0-9.]+)M/s\).*get_hits=(\d+)/(\d+)")
exp_re = re.compile(r"expire: dropped=(\d+) drained_recs=(\d+)")
idx_re = re.compile(r"index: rehashes=(\d+)")
waves_re = re.compile(r"^# waves=(\d+)", re.M)
mem_re = re.compile(
    r"mem: (idle|end|trim) commit_KiB=(\d+) rss_KiB=(\d+|\?) epochs=(\d+)"
    r"(?: live_n=(\d+)(?: map_n=(\d+))?)?"
)
keys_re = re.compile(r"^# keys: live_n=(\d+)(?: map_n=(\d+))?", re.M)
trim_keys_re = re.compile(r"^# keys: trim live_n=(\d+)(?: map_n=(\d+))?", re.M)
live_re = re.compile(r"compact: .* live_n=(\d+)")

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
    im = idx_re.search(text)
    wm = waves_re.search(text)
    km = keys_re.search(text)
    tk = trim_keys_re.search(text)
    lm = live_re.search(text)
    if not om:
        continue
    hits, gets = int(om.group(2)), int(om.group(3))
    dropped = int(em.group(1)) if em else 0
    drained = int(em.group(2)) if em else 0
    idle_c = idle_r = end_c = end_r = epochs = None
    trim_c = trim_r = None
    end_live = end_map = trim_live = trim_map = None
    for mm in mem_re.finditer(text):
        kind = mm.group(1)
        ck = int(mm.group(2))
        rk = None if mm.group(3) == "?" else int(mm.group(3))
        epochs = int(mm.group(4))
        ln = int(mm.group(5)) if mm.group(5) is not None else None
        mn = int(mm.group(6)) if mm.group(6) is not None else None
        if kind == "idle":
            idle_c, idle_r = ck, rk
        elif kind == "trim":
            trim_c, trim_r = ck, rk
            if ln is not None:
                trim_live = ln
            if mn is not None:
                trim_map = mn
        else:
            end_c, end_r = ck, rk
            if ln is not None:
                end_live = ln
            if mn is not None:
                end_map = mn
    if km:
        end_live = int(km.group(1))
        if km.group(2) is not None:
            end_map = int(km.group(2))
    elif end_live is None and lm:
        end_live = int(lm.group(1))
    if tk:
        trim_live = int(tk.group(1))
        if tk.group(2) is not None:
            trim_map = int(tk.group(2))
    shape = (mix, sh, cl)
    if shape not in order:
        order.append(shape)
    key = (mix, sh, cl, lang, sweep)
    samples.setdefault(key, []).append({
        "mops": float(om.group(1)),
        "hit": (100.0 * hits / gets) if gets else 0.0,
        "dropped": dropped,
        "ok": dropped == drained,
        "rehash": int(im.group(1)) if im else 0,
        "waves": int(wm.group(1)) if wm else 0,
        "idle_c": idle_c,
        "idle_r": idle_r,
        "end_c": end_c,
        "end_r": end_r,
        "epochs": epochs,
        "live": end_live,
        "mapn": end_map,
        "trim_c": trim_c,
        "trim_r": trim_r,
        "trim_live": trim_live,
        "trim_map": trim_map,
    })

def med(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    return statistics.median(xs)

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
        "rehash": med([r["rehash"] for r in recs]),
        "waves": med([r["waves"] for r in recs]),
        "idle_c": med([r["idle_c"] for r in recs]),
        "idle_r": med([r["idle_r"] for r in recs]),
        "end_c": med([r["end_c"] for r in recs]),
        "end_r": med([r["end_r"] for r in recs]),
        "epochs": med([r["epochs"] for r in recs]),
        "live": med([r["live"] for r in recs]),
        "mapn": med([r["mapn"] for r in recs]),
        "trim_c": med([r["trim_c"] for r in recs]),
        "trim_r": med([r["trim_r"] for r in recs]),
        "trim_live": med([r["trim_live"] for r in recs]),
        "trim_map": med([r["trim_map"] for r in recs]),
    }

rows = {k: collapse(v) for k, v in samples.items()}
nrep = max((len(v) for v in samples.values()), default=1)

def tax(off, on):
    if not off or not off["mops"]:
        return None
    return 100.0 * (on["mops"] - off["mops"]) / off["mops"]

def fmt_tax(t):
    if t is None:
        return "    ?"
    return f"{t:+6.0f}%"

def fmt_drop(n):
    if n is None:
        return "?"
    if n >= 1_000_000:
        return f"{n/1e6:.1f}M"
    if n >= 1000:
        return f"{n/1e3:.0f}k"
    return f"{n:.0f}"

def fmt_kib(n):
    if n is None:
        return "     ?"
    if n >= 1024:
        return f"{n/1024:6.1f}M"
    return f"{n:6.0f}k"

def fmt_keys(n):
    if n is None:
        return "     ?"
    if n >= 1_000_000:
        return f"{n/1e6:5.2f}M"
    if n >= 1000:
        return f"{n/1e3:5.1f}k"
    return f"{n:6.0f}"

print()
print(f"# summary  seconds={sec} ncpu={ncpu}  repeats={nrep}  "
      f"{'median' if nrep > 1 else 'this pass only'}")
print(f"# {'mix':<6} {'shape':<6} {'3 off':>7} {'3 on':>7} {'3 tax':>7} "
      f"{'4 off':>7} {'4 on':>7} {'4 tax':>7} {'on 4/3':>8} "
      f"{'hit%':>5} {'expire':>7} {'waves':>6}")
missing = 0
for mix, sh, cl in order:
    def get(lang, sweep):
        return rows.get((mix, sh, cl, lang, sweep))

    a0, a1 = get("cc3", "0"), get("cc3", "1")
    b0, b1 = get("cc4", "0"), get("cc4", "1")
    if not all((a0, a1, b0, b1)):
        missing += 1
        print(f"# {mix:<6} {sh + '/' + cl:<6}  (incomplete)")
        continue
    atax = tax(a0, a1)
    btax = tax(b0, b1)
    ratio = b1["mops"] / a1["mops"] if a1["mops"] else 0.0
    expire = fmt_drop(b1["dropped"])
    mark = "" if a1["ok"] and b1["ok"] else " !dropped!=drained"
    print(f"# {mix:<6} {sh + '/' + cl:<6} {a0['mops']:7.2f} {a1['mops']:7.2f} "
          f"{fmt_tax(atax):>7} {b0['mops']:7.2f} {b1['mops']:7.2f} "
          f"{fmt_tax(btax):>7} {ratio:8.2f} {b1['hit']:5.0f} {expire:>7} "
          f"{b1['waves']:6.0f}{mark}")
    if nrep > 1:
        print(f"# {'':<6} {'':<6}  [{a0['mops_lo']:.2f}-{a0['mops_hi']:.2f}] "
              f"[{a1['mops_lo']:.2f}-{a1['mops_hi']:.2f}]         "
              f"[{b0['mops_lo']:.2f}-{b0['mops_hi']:.2f}] "
              f"[{b1['mops_lo']:.2f}-{b1['mops_hi']:.2f}]")

print()
print(f"# {'mix':<6} {'shape':<6} {'sweep':<5} "
      f"{'3 idle':>8} {'3 end':>8} {'3 rss':>8} {'3 keys':>8} "
      f"{'4 idle':>8} {'4 end':>8} {'4 rss':>8} {'4 keys':>8}")
for mix, sh, cl in order:
    for sweep, label in (("0", "off"), ("1", "on")):
        a = rows.get((mix, sh, cl, "cc3", sweep))
        b = rows.get((mix, sh, cl, "cc4", sweep))
        if not a or not b:
            print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5}  (incomplete)")
            continue
        mark = ""
        if a["live"] is not None and a["mapn"] is not None and a["live"] != a["mapn"]:
            mark += " !3 live!=map"
        if b["live"] is not None and b["mapn"] is not None and b["live"] != b["mapn"]:
            mark += " !4 live!=map"
        print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5} "
              f"{fmt_kib(a['idle_c']):>8} {fmt_kib(a['end_c']):>8} {fmt_kib(a['end_r']):>8} "
              f"{fmt_keys(a['live']):>8} "
              f"{fmt_kib(b['idle_c']):>8} {fmt_kib(b['end_c']):>8} {fmt_kib(b['end_r']):>8} "
              f"{fmt_keys(b['live']):>8}{mark}")

print()
print(f"# {'mix':<6} {'shape':<6} {'sweep':<5} "
      f"{'3 trim':>8} {'3 t-rss':>8} {'3 B/k':>7} "
      f"{'4 trim':>8} {'4 t-rss':>8} {'4 B/k':>7}")

def b_per_key(rss_kib, keys):
    if rss_kib is None or keys is None or keys <= 0:
        return None
    return (rss_kib * 1024.0) / keys

def fmt_bkey(n):
    if n is None:
        return "     ?"
    return f"{n:6.0f}"

for mix, sh, cl in order:
    for sweep, label in (("0", "off"), ("1", "on")):
        a = rows.get((mix, sh, cl, "cc3", sweep))
        b = rows.get((mix, sh, cl, "cc4", sweep))
        if not a or not b:
            print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5}  (incomplete)")
            continue
        mark = ""
        if a["trim_live"] is not None and a["trim_map"] is not None and a["trim_live"] != a["trim_map"]:
            mark += " !3 live!=map"
        if b["trim_live"] is not None and b["trim_map"] is not None and b["trim_live"] != b["trim_map"]:
            mark += " !4 live!=map"
        print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5} "
              f"{fmt_keys(a['trim_live']):>8} {fmt_kib(a['trim_r']):>8} "
              f"{fmt_bkey(b_per_key(a['trim_r'], a['trim_live'])):>7} "
              f"{fmt_keys(b['trim_live']):>8} {fmt_kib(b['trim_r']):>8} "
              f"{fmt_bkey(b_per_key(b['trim_r'], b['trim_live'])):>7}{mark}")

print("# ops table and mem table are one result. Quote both.")
print("# 3 = wstore3 (value-in-wheel). 4 = wstore4 (values + CCTemporalIndex).")
print("# tax = (sweep-on − sweep-off) / sweep-off. on 4/3 = sweep-on ratio.")
print("# expire = wstore4 sweep-on dropped (must equal drained_recs).")
print("# mem idle = post-init, before preload/clients. keys = mix-end live_n. "
      "trim = expire to 1M after mix.")
if nrep > 1:
    print("# repeats: ops/mem cells are medians; bracket lines are min-max M/s.")
if missing:
    sys.exit(1)
PY
