#!/usr/bin/env bash
# compare_wstore3.sh — CCEpochKeys vs Go death-time twin
#
# Default is four 1s pairs: write 1/1 × {cc, go} × sweep {off, on}.
# That is the tax row. N/N, get, drain, and longer windows are opt-in.
# 1s cells have ~±20% noise; tax columns need e.g.
#   REPEATS=5 ./perf/compare_wstore3.sh 30
# and then median/min, not a single pass.
# Runs are shuffled; the summary table is this pass only.
# Does not set or unset GOMAXPROCS / CC_WORKERS.
#
#   ./perf/compare_wstore3.sh [seconds]
#   SHAPES=1,n ./perf/compare_wstore3.sh
#   MIXES=write,get ./perf/compare_wstore3.sh
#   FULL=1 ./perf/compare_wstore3.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CCC="${CCC:-$REPO_ROOT/cc/bin/ccc}"
OUT="$SCRIPT_DIR/out"
SEC="${1:-1}"
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if [ "$NCPU" -lt 1 ]; then NCPU=1; fi
if [ "$NCPU" -gt 64 ]; then NCPU=64; fi

if [ "${FULL:-0}" = 1 ]; then
    MIXES="${MIXES:-write,get}"
    SHAPES="${SHAPES:-1,n}"
fi
MIXES="${MIXES:-write}"
SHAPES="${SHAPES:-1}"

mkdir -p "$OUT"
JOBLIST="$OUT/this_run.jobs"
ORDER="$OUT/this_run.order"

echo "# compare_wstore3 seconds=$SEC ncpu=$NCPU mixes=$MIXES shapes=$SHAPES GOMAXPROCS=${GOMAXPROCS:-} CC_WORKERS=${CC_WORKERS:-}"
echo "# build"
"$CCC" --release -o "$OUT/wstore3" "$SCRIPT_DIR/wstore3.ccs"
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
        for tag in cc go; do
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
while IFS= read -r job || [ -n "$job" ]; do
    [ -n "$job" ] || continue
    set -- $job
    echo "$1_$5_s$2_$3x$4.txt" >> "$JOBLIST"
done < "$ORDER"

echo "# ${#JOBS[@]} jobs shuffled (${SEC}s each)"

run() {
    local tag="$1" sweep="$2" shards="$3" clients="$4" mix="$5"
    local bin rec
    if [ "$tag" = cc ]; then
        bin="$OUT/wstore3"
    else
        bin="$OUT/wstore3_go"
    fi
    rec="${tag}_${mix}_s${sweep}_${shards}x${clients}.txt"
    echo "# --- $tag mix=$mix sweep=$sweep ${shards}/${clients} ---"
    "$bin" "$SEC" "$sweep" 0.35 512 "$shards" "$clients" "$mix" \
        > "$OUT/$rec"
    grep -E '^# (ops=|expire:|index:|compact:|waves=|mem:|hitch |wstore3|wstore3-go)' "$OUT/$rec"
}

while IFS= read -r job || [ -n "$job" ]; do
    [ -n "$job" ] || continue
    # shellcheck disable=SC2086
    run $job
done < "$ORDER"

python3 - "$OUT" "$SEC" "$NCPU" "$JOBLIST" <<'PY'
import re, sys
from pathlib import Path

out = Path(sys.argv[1])
sec, ncpu = sys.argv[2], sys.argv[3]
wanted = [ln.strip() for ln in Path(sys.argv[4]).read_text().splitlines() if ln.strip()]
pat = re.compile(r"^(cc|go)_(write|get|drain)_s([01])_(\d+)x(\d+)\.txt$")
ops_re = re.compile(r"ops=\d+ \(([0-9.]+)M/s\).*get_hits=(\d+)/(\d+)")
exp_re = re.compile(r"expire: dropped=(\d+) drained_recs=(\d+)")
idx_re = re.compile(r"index: rehashes=(\d+)")
waves_re = re.compile(r"^# waves=(\d+)", re.M)
mem_re = re.compile(r"mem: (idle|end) commit_KiB=(\d+) rss_KiB=(\d+|\?) epochs=(\d+)")

rows = {}
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
    if not om:
        continue
    hits, gets = int(om.group(2)), int(om.group(3))
    dropped = int(em.group(1)) if em else 0
    drained = int(em.group(2)) if em else 0
    idle_c = idle_r = end_c = end_r = epochs = None
    for mm in mem_re.finditer(text):
        kind = mm.group(1)
        ck = int(mm.group(2))
        rk = None if mm.group(3) == "?" else int(mm.group(3))
        epochs = int(mm.group(4))
        if kind == "idle":
            idle_c, idle_r = ck, rk
        else:
            end_c, end_r = ck, rk
    key = (mix, sh, cl)
    if key not in order:
        order.append(key)
    rows[(mix, sh, cl, lang, sweep)] = {
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
    }

def tax(off, on):
    if not off:
        return None
    return 100.0 * (on - off) / off

def fmt_tax(t):
    if t is None:
        return "    ?"
    return f"{t:+6.0f}%"

def fmt_drop(n):
    if n >= 1_000_000:
        return f"{n/1e6:.1f}M"
    if n >= 1000:
        return f"{n/1e3:.0f}k"
    return str(n)

print()
print(f"# summary  seconds={sec} ncpu={ncpu}  (this pass only)")
print(f"# {'mix':<6} {'shape':<6} {'CC off':>7} {'CC on':>7} {'CC tax':>7} "
      f"{'Go off':>7} {'Go on':>7} {'Go tax':>7} {'on CC/Go':>8} "
      f"{'hit%':>5} {'expire':>7} {'rehash':>6} {'waves':>6}")
missing = 0
for mix, sh, cl in order:
    def get(lang, sweep):
        return rows.get((mix, sh, cl, lang, sweep))

    cc0, cc1 = get("cc", "0"), get("cc", "1")
    go0, go1 = get("go", "0"), get("go", "1")
    if not all((cc0, cc1, go0, go1)):
        missing += 1
        print(f"# {mix:<6} {sh + '/' + cl:<6}  (incomplete)")
        continue
    ctax = tax(cc0["mops"], cc1["mops"])
    gtax = tax(go0["mops"], go1["mops"])
    ratio = cc1["mops"] / go1["mops"] if go1["mops"] else 0.0
    expire = fmt_drop(cc1["dropped"])
    mark = "" if cc1["ok"] and go1["ok"] else " !dropped!=drained"
    print(f"# {mix:<6} {sh + '/' + cl:<6} {cc0['mops']:7.2f} {cc1['mops']:7.2f} "
          f"{fmt_tax(ctax):>7} {go0['mops']:7.2f} {go1['mops']:7.2f} "
          f"{fmt_tax(gtax):>7} {ratio:8.2f} {cc1['hit']:5.0f} {expire:>7} "
          f"{cc1['rehash']:6d} {cc1['waves']:6d}{mark}")

def fmt_kib(n):
    if n is None:
        return "     ?"
    if n >= 1024:
        return f"{n/1024:6.1f}M"
    return f"{n:6d}k"

print()
print(f"# {'mix':<6} {'shape':<6} {'sweep':<5} {'CC idle':>8} {'CC end':>8} {'CC rss':>8} "
      f"{'Go idle':>8} {'Go end':>8} {'Go rss':>8} {'epochs':>7}")
for mix, sh, cl in order:
    for sweep, label in (("0", "off"), ("1", "on")):
        cc = rows.get((mix, sh, cl, "cc", sweep))
        go = rows.get((mix, sh, cl, "go", sweep))
        if not cc or not go:
            print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5}  (incomplete)")
            continue
        print(f"# {mix:<6} {sh + '/' + cl:<6} {label:<5} "
              f"{fmt_kib(cc['idle_c']):>8} {fmt_kib(cc['end_c']):>8} {fmt_kib(cc['end_r']):>8} "
              f"{fmt_kib(go['idle_c']):>8} {fmt_kib(go['end_c']):>8} {fmt_kib(go['end_r']):>8} "
              f"{(str(cc['epochs']) + '/' + str(go['epochs'])) if cc['epochs'] is not None and go['epochs'] is not None else '?':>7}")

print("# expire = CC sweep-on dropped (must equal drained_recs). "
      "rehash = CC index grow, not expire.")
print("# tax = (sweep-on − sweep-off) / sweep-off. on CC/Go = sweep-on ratio.")
print("# mem idle = post-init, before preload/clients. CC commit = arena; "
      "Go commit = HeapSys. epochs = buckets with live keys.")
if missing:
    sys.exit(1)
PY
