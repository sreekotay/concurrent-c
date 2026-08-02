#!/usr/bin/env bash
# Fair SERDES vs production ccc wall times (succession metric).
#
# Succession metric = warm host-cc parity (not libtcc-vs-clang).
#
#   ccc_warm / serdes_warm — cached rebuild after priming
#   ccc_cold / serdes_cold — --no-cache full rebuild
#   tcc_exe                — optional libtcc --exe (fast finish, different backend)
#
# Usage: bash examples/serdes/c/shadow/bench_vs_ccc.sh [iters]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"
ITERS="${1:-5}"
export ROOT ITERS
export CCC="${CCC:-$ROOT/out/cc/bin/ccc}"
export SHADOW="${SHADOW:-$ROOT/out/cc/bin/shadow_lower}"
export RT_LIBDIR="${SHADOW_RUNTIME_LIBDIR:-$ROOT/out/cc/lib}"
if [[ "$(uname -s)" == Darwin ]]; then
  export DYLD_LIBRARY_PATH="$RT_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$RT_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
export TMP="${TMPDIR:-/tmp}/cc_serdes_bench_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$CCC" ]]; then echo "FAIL: missing ccc at $CCC"; exit 1; fi
if [[ ! -x "$SHADOW" ]]; then echo "FAIL: missing shadow_lower at $SHADOW"; exit 1; fi

python3 - <<'PY'
import os, statistics, subprocess, sys, time
from pathlib import Path

ROOT = Path(os.environ["ROOT"])
ITERS = int(os.environ["ITERS"])
CCC = os.environ["CCC"]
SHADOW = os.environ["SHADOW"]
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

print(f"iters={ITERS}")
print(f"ccc={CCC}")
print(f"shadow={SHADOW}")
print("metric: warm host-cc parity (serdes_warm / ccc_warm)")
print()
hdr = (
    f"{'recipe':<12} {'ccc_w':>8} {'ser_w':>8} {'ratio_w':>8} "
    f"{'ccc_c':>8} {'ser_c':>8} {'tcc_e':>8}"
)
print(hdr)
print("-" * len(hdr))

rows = []
for name, rel in RECIPES:
    src = ROOT / rel
    ccc_bin = TMP / f"ccc_{name}.bin"
    ser_bin = TMP / f"ser_{name}.bin"
    tcc_bin = TMP / f"tcc_{name}.bin"

    # Prime caches (warm path)
    run([CCC, str(src), "-o", str(ccc_bin)])
    run([SHADOW, str(src), "-o", str(ser_bin)])
    run([SHADOW, str(src), "--exe", str(tcc_bin)])

    ccc_w = timed([CCC, str(src), "-o", str(ccc_bin)])
    ser_w = timed([SHADOW, str(src), "-o", str(ser_bin)])
    ccc_c = timed([CCC, "--no-cache", str(src), "-o", str(ccc_bin)])
    ser_c = timed([SHADOW, "--no-cache", str(src), "-o", str(ser_bin)])
    tcc_e = timed([SHADOW, str(src), "--exe", str(tcc_bin)])

    ratio_w = ser_w[0] / ccc_w[0] if ccc_w[0] > 0 else float("nan")
    rows.append((name, ccc_w, ser_w, ccc_c, ser_c, tcc_e, ratio_w))
    print(
        f"{name:<12} {ccc_w[0]:8.1f} {ser_w[0]:8.1f} {ratio_w:7.2f}x "
        f"{ccc_c[0]:8.1f} {ser_c[0]:8.1f} {tcc_e[0]:8.1f}"
    )

print()
print("ms best-of-N; ratio_w = serdes_warm / ccc_warm  (<1 serdes faster warm rebuild)")
print()
avg = lambda i: statistics.mean(r[i][0] for r in rows)
print(f"mean ccc_warm:     {avg(1):7.1f} ms")
print(f"mean serdes_warm:  {avg(2):7.1f} ms   ({statistics.mean(r[-1] for r in rows):.2f}x ccc)")
print(f"mean ccc_cold:     {avg(3):7.1f} ms")
print(f"mean serdes_cold:  {avg(4):7.1f} ms")
print(f"mean tcc_exe:      {avg(5):7.1f} ms   (different backend; not succession metric)")
PY
