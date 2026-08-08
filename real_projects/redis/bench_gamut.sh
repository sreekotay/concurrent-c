#!/bin/bash
# bench_gamut.sh — clients × keyspace × (optional) value-size matrix for
# upstream / idiomatic / go. Servers stay up; each cell shuffles binary order,
# discards one warmup round, records REPEATS measured rounds.
#
# Environment:
#   CLIENTS_SWEEP   default "1 10 50 100"
#   KEYS_SWEEP      -r keyspace sizes (default "1000 10000 100000 1000000")
#   DATA_SWEEP      -d value bytes; empty = skip data axis (default "3 64 256 1024")
#                   When DATA_SWEEP is set, KEYS_SWEEP is crossed with it at each
#                   clients value — that explodes quickly. Prefer one axis at a
#                   time: DATA_SWEEP="" for keyspace×clients, or KEYS_SWEEP with
#                   a single value when sweeping data.
#   AXIS            auto|keys|data — auto uses both when both sweeps have >1
#                   value; set keys or data to force a single axis (other fixed).
#   REPEATS         measured rounds per cell (default 2)
#   REQUESTS        -n (default 100000)
#   PIPELINE        -P (default 16)
#   BENCH_TESTS     default set,get  (-d only affects SET/GET)
#   INCLUDE_GO      default 1
#   OUT             results log path (default benchmarks/gamut_<ts>.txt)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH_BIN="$SCRIPT_DIR/redis_c/src/redis-benchmark"
UPSTREAM_BIN="$SCRIPT_DIR/redis_c/src/redis-server"
IDIOMATIC_BIN="${IDIOMATIC_BIN:-$SCRIPT_DIR/out/redis_idiomatic}"
GO_BIN="${GO_BIN:-$SCRIPT_DIR/out/redis_go}"
INCLUDE_GO="${INCLUDE_GO:-1}"

CLIENTS_SWEEP="${CLIENTS_SWEEP:-1 10 50 100}"
KEYS_SWEEP="${KEYS_SWEEP:-1000 10000 100000 1000000}"
DATA_SWEEP="${DATA_SWEEP:-3 64 256 1024}"
AXIS="${AXIS:-auto}"
REPEATS="${REPEATS:-2}"
REQUESTS="${REQUESTS:-100000}"
PIPELINE="${PIPELINE:-16}"
BENCH_TESTS="${BENCH_TESTS:-set,get}"
UPSTREAM_PORT="${UPSTREAM_PORT:-6391}"
IDIOMATIC_PORT="${IDIOMATIC_PORT:-6393}"
GO_PORT="${GO_PORT:-6397}"
FIXED_KEYS="${FIXED_KEYS:-100000}"
FIXED_DATA="${FIXED_DATA:-3}"

TS="$(date +%Y%m%d_%H%M%S)"
OUT="${OUT:-$SCRIPT_DIR/benchmarks/gamut_${TS}.txt}"
mkdir -p "$(dirname "$OUT")"

need() { [[ -x "$1" ]] || { echo "missing $2 at $1" >&2; exit 1; }; }
need "$BENCH_BIN" redis-benchmark
need "$UPSTREAM_BIN" redis-server
need "$IDIOMATIC_BIN" redis_idiomatic
[[ "$INCLUDE_GO" == "1" ]] && need "$GO_BIN" redis_go

LABELS=(upstream idiomatic)
[[ "$INCLUDE_GO" == "1" ]] && LABELS+=(go)
LABELS_PY="$(printf "'%s'," "${LABELS[@]}")"
LABELS_PY="[${LABELS_PY%,}]"

# Resolve axes
n_keys=$(wc -w <<<"$KEYS_SWEEP" | tr -d ' ')
n_data=$(wc -w <<<"$DATA_SWEEP" | tr -d ' ')
case "$AXIS" in
  keys) DATA_SWEEP="$FIXED_DATA"; n_data=1 ;;
  data) KEYS_SWEEP="$FIXED_KEYS"; n_keys=1 ;;
  auto)
    if [[ "$n_keys" -gt 1 && "$n_data" -gt 1 ]]; then
      # Prefer clients×keys first (payload fixed); run data as second pass note.
      echo "[gamut] AXIS=auto: sweeping clients×keys with -d=$FIXED_DATA (set DATA_SWEEP='' or AXIS=data for payload)" >&2
      DATA_SWEEP="$FIXED_DATA"
      n_data=1
    fi
    ;;
  *) echo "AXIS must be auto|keys|data" >&2; exit 2 ;;
esac

TMP_DIR="$(mktemp -d -t bench_gamut.XXXXXX)"
echo "tmp dir: $TMP_DIR" >&2
echo "log: $OUT" >&2
exec > >(tee "$OUT") 2>&1

trap 'for p in ${ALL_PIDS:-}; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done' EXIT

wait_port() {
    python3 - "$1" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
deadline = time.time() + 12.0
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", p), timeout=0.2):
            sys.exit(0)
    except OSError:
        time.sleep(0.05)
print(f"port {p} did not open", file=sys.stderr)
sys.exit(1)
PY
}

"$UPSTREAM_BIN" --save "" --appendonly no --port "$UPSTREAM_PORT" >"$TMP_DIR/upstream.log" 2>&1 &
UPSTREAM_PID=$!
"$IDIOMATIC_BIN" "$IDIOMATIC_PORT" >"$TMP_DIR/idiomatic.log" 2>&1 &
IDIOMATIC_PID=$!
ALL_PIDS="$UPSTREAM_PID $IDIOMATIC_PID"
GO_PID=""
if [[ "$INCLUDE_GO" == "1" ]]; then
    "$GO_BIN" "127.0.0.1:$GO_PORT" >"$TMP_DIR/go.log" 2>&1 &
    GO_PID=$!
    ALL_PIDS="$ALL_PIDS $GO_PID"
fi
wait_port "$UPSTREAM_PORT"
wait_port "$IDIOMATIC_PORT"
[[ "$INCLUDE_GO" == "1" ]] && wait_port "$GO_PORT"

port_for() {
    case "$1" in
        upstream) echo "$UPSTREAM_PORT" ;;
        idiomatic) echo "$IDIOMATIC_PORT" ;;
        go) echo "$GO_PORT" ;;
        *) exit 1 ;;
    esac
}
pid_for() {
    case "$1" in
        upstream) echo "$UPSTREAM_PID" ;;
        idiomatic) echo "$IDIOMATIC_PID" ;;
        go) echo "$GO_PID" ;;
        *) exit 1 ;;
    esac
}

extract_rps() {
    grep -E "${1}: [0-9.]+ requests per second" "$2" | tail -1 \
        | sed -E 's/.*: ([0-9.]+) requests per second.*/\1/'
}

sample_rss() {
    ps -p "$1" -o rss= 2>/dev/null | tr -d ' ' || echo 0
}

IFS=',' read -ra CMDS <<< "$BENCH_TESTS"
CSV="$TMP_DIR/results.csv"
echo "clients,keys,data,cmd,label,rps,round" >"$CSV"

echo "== bench_gamut =="
echo "clients=[$CLIENTS_SWEEP] keys=[$KEYS_SWEEP] data=[$DATA_SWEEP]"
echo "repeats=$REPEATS requests=$REQUESTS pipeline=$PIPELINE tests=$BENCH_TESTS include_go=$INCLUDE_GO"
echo "labels=${LABELS[*]}"
echo

for c in $CLIENTS_SWEEP; do
  for rkeys in $KEYS_SWEEP; do
    for d in $DATA_SWEEP; do
      cell="c=${c} r=${rkeys} d=${d}"
      echo "[cell $cell]"
      TOTAL=$((REPEATS + 1))
      for round in $(seq 0 "$REPEATS"); do
        tag=$([[ $round == 0 ]] && echo warmup || echo "r$round")
        for cmd in "${CMDS[@]}"; do
          order_str="$(python3 -c "import random; labels=$LABELS_PY; random.shuffle(labels); print(' '.join(labels))")"
          read -ra ORDER <<< "$order_str"
          for label in "${ORDER[@]}"; do
            pid="$(pid_for "$label")"
            if ! kill -0 "$pid" 2>/dev/null; then
              echo "FAIL: $label died during $cell" >&2
              exit 1
            fi
            port="$(port_for "$label")"
            out="$TMP_DIR/${label}_${cmd}_c${c}_r${rkeys}_d${d}_${tag}.log"
            "$BENCH_BIN" -h 127.0.0.1 -p "$port" -n "$REQUESTS" -c "$c" \
              -P "$PIPELINE" -q -r "$rkeys" -d "$d" -t "$cmd" >"$out" 2>&1 || {
                echo "FAIL: bench $label $cmd $cell; log=$out" >&2
                exit 1
              }
            upper="$(echo "$cmd" | tr '[:lower:]' '[:upper:]')"
            rps="$(extract_rps "$upper" "$out" || true)"
            [[ -n "${rps:-}" ]] || { echo "FAIL: parse rps $out" >&2; exit 1; }
            printf "  %-9s %-4s %-6s rps=%s\n" "$label" "$cmd" "$tag" "$rps"
            if [[ $round -gt 0 ]]; then
              echo "$c,$rkeys,$d,$cmd,$label,$rps,$round" >>"$CSV"
            fi
          done
        done
      done
      # RSS snapshot after cell (idle-ish between cells)
      printf "  rss_kb:"
      for label in "${LABELS[@]}"; do
        printf " %s=%s" "$label" "$(sample_rss "$(pid_for "$label")")"
      done
      printf "\n"
    done
  done
done

python3 - <<PY
import csv, statistics
from collections import defaultdict

rows = list(csv.DictReader(open("$CSV")))
by = defaultdict(list)
for r in rows:
    key = (int(r["clients"]), int(r["keys"]), int(r["data"]), r["cmd"], r["label"])
    by[key].append(float(r["rps"]))

labels = $LABELS_PY
cmds = sorted({r["cmd"] for r in rows}, key=lambda c: ["set","get","incr"].index(c) if c in ("set","get","incr") else 99)
clients = sorted({int(r["clients"]) for r in rows})
keys = sorted({int(r["keys"]) for r in rows})
datas = sorted({int(r["data"]) for r in rows})

def med(vals):
    return statistics.median(vals) if vals else float("nan")

def fmt(x):
    if x != x:
        return "n/a"
    return f"{x/1e6:.3f}M"

print()
print("== gamut summary (median RPS over measured rounds) ==")
for cmd in cmds:
    print(f"\n[{cmd.upper()}]")
    # Pivot: rows = (keys or data), cols = clients × label — keep compact
    if len(datas) > 1 and len(keys) == 1:
        axis_name, axis_vals = "d", datas
        fixed = f"keys={keys[0]}"
    else:
        axis_name, axis_vals = "r", keys
        fixed = f"data={datas[0]}"
    print(f"  fixed: {fixed}  pipeline=$PIPELINE  n=$REQUESTS")
    hdr = f"  {axis_name:>10}"
    for c in clients:
        for lab in labels:
            hdr += f"  {lab[:3]}@c{c:>3}"
    # ratios vs upstream at end of each axis row for c=max
    print(hdr)
    for ax in axis_vals:
        line = f"  {ax:>10}"
        for c in clients:
            for lab in labels:
                if axis_name == "d":
                    vals = by.get((c, keys[0], ax, cmd, lab), [])
                else:
                    vals = by.get((c, ax, datas[0], cmd, lab), [])
                line += f"  {fmt(med(vals)):>8}"
        print(line)
    # Ratio table: idiomatic/upstream and go/upstream at each cell (median)
    print(f"  ratios vs upstream (idiomatic, go):")
    for ax in axis_vals:
        bits = [f"  {axis_name}={ax}"]
        for c in clients:
            if axis_name == "d":
                up = med(by.get((c, keys[0], ax, cmd, "upstream"), []))
                idm = med(by.get((c, keys[0], ax, cmd, "idiomatic"), []))
                go = med(by.get((c, keys[0], ax, cmd, "go"), []))
            else:
                up = med(by.get((c, ax, datas[0], cmd, "upstream"), []))
                idm = med(by.get((c, ax, datas[0], cmd, "idiomatic"), []))
                go = med(by.get((c, ax, datas[0], cmd, "go"), []))
            idr = f"{idm/up:.2f}x" if up == up and up else "n/a"
            gor = f"{go/up:.2f}x" if up == up and up and go == go else "n/a"
            bits.append(f"c{c}:{idr}/{gor}")
        print("  " + "  ".join(bits))

print()
print(f"csv: $CSV")
print(f"log: $OUT")
PY
