#!/usr/bin/env bash
# Compare SERDES shadow path vs production ccc on recipe wall times.
#
# Builds a *native* shadow_lower once (fair product path), and also times the
# current wrapper (`ccc run --no-cache shadow_lower.ccs`) which recompiles the
# lowerer on every invoke.
#
#   ccc_build       — production ./ccc SRC -o bin
#   wrap_lower      — installed shadow_lower wrapper (ccc run each time)
#   nat_lower       — native shadow_lower binary (emit .c only)
#   nat_build       — native lower + host cc -c + link runtime
#   ccc_run/nat_run — binary wall time
#
# Usage: bash examples/serdes/c/shadow/bench_vs_ccc.sh [iters]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"
ITERS="${1:-5}"
export ROOT ITERS
export CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
export SHADOW_WRAP="$ROOT/examples/serdes/c/shadow_lower.sh"
export RT_O="$ROOT/out/cc/obj/runtime/concurrent_c.o"
export TMP="${TMPDIR:-/tmp}/cc_serdes_bench_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$CCC" ]]; then echo "FAIL: missing ccc at $CCC"; exit 1; fi
if [[ ! -f "$RT_O" ]]; then echo "FAIL: missing runtime $RT_O"; exit 1; fi

echo "[bench] building native shadow_lower once..."
"$CCC" "$ROOT/examples/serdes/c/shadow_lower.ccs" -o "$TMP/shadow_lower_native"
export SHADOW_NATIVE="$TMP/shadow_lower_native"

python3 - <<'PY'
import os, statistics, subprocess, sys, time
from pathlib import Path

ROOT = Path(os.environ["ROOT"])
ITERS = int(os.environ["ITERS"])
CCC = os.environ["CCC"]
WRAP = os.environ["SHADOW_WRAP"]
NATIVE = os.environ["SHADOW_NATIVE"]
RT_O = os.environ["RT_O"]
TMP = Path(os.environ["TMP"])

RECIPES = [
    ("hello", "examples/hello.ccs"),
    ("result", "examples/recipe_result_error_handling.ccs"),
    ("arena", "examples/recipe_arena_scope.ccs"),
    ("capture", "examples/recipe_explicit_capture.ccs"),
    ("ufcs", "examples/recipe_ufcs_forms.ccs"),
    ("generics", "examples/recipe_user_generics.ccs"),
    ("ordered", "examples/recipe_ordered_parallel.ccs"),
    ("channel", "examples/recipe_channel_pipeline.ccs"),
    ("exclusive", "examples/recipe_exclusive_named.ccs"),
]

def run(cmd, *, quiet=True):
    kw = {}
    if quiet:
        kw["stdout"] = subprocess.DEVNULL
        kw["stderr"] = subprocess.PIPE
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        err = (r.stderr or b"").decode("utf-8", "replace")
        print(err, file=sys.stderr)
        raise SystemExit(f"FAIL rc={r.returncode}: {' '.join(map(str, cmd))}")

def timed(cmd):
    ms = []
    for _ in range(ITERS):
        t0 = time.perf_counter()
        run(cmd)
        ms.append((time.perf_counter() - t0) * 1000.0)
    return min(ms), statistics.mean(ms)

def nat_build(src: Path, bin_path: Path):
    c = TMP / (src.stem + ".c")
    o = TMP / (src.stem + ".o")
    run([NATIVE, str(src), "-o", str(c)])
    run(["cc", "-std=c11", "-O2", f"-I{ROOT}/out/include", "-c", str(c), "-o", str(o)])
    run(["cc", "-O2", str(o), RT_O, "-o", str(bin_path), "-lpthread", "-lm"])

print(f"iters={ITERS}")
print(f"ccc={CCC}")
print(f"native_lower={NATIVE}")
print(f"wrapper={WRAP}  (ccc run --no-cache each invoke)")
print()
hdr = (
    f"{'recipe':<12} {'ccc_b':>8} {'wrap_lo':>8} {'nat_lo':>8} "
    f"{'nat_b':>8} {'ratio':>7} {'ccc_r':>8} {'nat_r':>8}"
)
print(hdr)
print("-" * len(hdr))

rows = []
for name, rel in RECIPES:
    src = ROOT / rel
    ccc_bin = TMP / f"ccc_{name}.bin"
    nat_bin = TMP / f"nat_{name}.bin"
    # warm
    run([CCC, str(src), "-o", str(ccc_bin)])
    nat_build(src, nat_bin)
    run([WRAP, str(src), "-o", str(TMP / f"{name}_warm.c")])

    ccc_b = timed([CCC, str(src), "-o", str(ccc_bin)])
    wrap_lo = timed([WRAP, str(src), "-o", str(TMP / f"{name}_wrap.c")])
    nat_lo = timed([NATIVE, str(src), "-o", str(TMP / f"{name}_nat.c")])

    nat_b_ms = []
    for _ in range(ITERS):
        t0 = time.perf_counter()
        nat_build(src, nat_bin)
        nat_b_ms.append((time.perf_counter() - t0) * 1000.0)
    nat_b = (min(nat_b_ms), statistics.mean(nat_b_ms))

    ccc_r = timed([str(ccc_bin)])
    nat_r = timed([str(nat_bin)])
    ratio = nat_b[0] / ccc_b[0] if ccc_b[0] > 0 else float("nan")
    rows.append((name, ccc_b, wrap_lo, nat_lo, nat_b, ccc_r, nat_r, ratio))
    print(
        f"{name:<12} {ccc_b[0]:8.1f} {wrap_lo[0]:8.1f} {nat_lo[0]:8.1f} "
        f"{nat_b[0]:8.1f} {ratio:6.2f}x {ccc_r[0]:8.1f} {nat_r[0]:8.1f}"
    )

print()
print("ms best-of-N; ratio = nat_build / ccc_build  (<1 native shadow faster to binary)")
print()
avg_wrap = statistics.mean(r[2][0] for r in rows)
avg_nat_lo = statistics.mean(r[3][0] for r in rows)
avg_ccc = statistics.mean(r[1][0] for r in rows)
avg_nat_b = statistics.mean(r[4][0] for r in rows)
avg_ratio = statistics.mean(r[-1] for r in rows)
print(f"mean ccc_build:          {avg_ccc:7.1f} ms")
print(f"mean wrapper lower:      {avg_wrap:7.1f} ms   ({avg_wrap/avg_ccc:5.1f}x ccc)")
print(f"mean native lower:       {avg_nat_lo:7.1f} ms   ({avg_nat_lo/avg_ccc:5.2f}x ccc)")
print(f"mean native full build:  {avg_nat_b:7.1f} ms   ({avg_ratio:5.2f}x ccc)")
print(
    "run delta ms (native - ccc):",
    ", ".join(f"{n}:{nr[0]-cr[0]:+.1f}" for n, _cb, _w, _nl, _nb, cr, nr, _rt in rows),
)
PY
