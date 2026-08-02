#!/usr/bin/env bash
# Compare SERDES shadow path vs production ccc on recipe wall times.
#
# Builds a *native* shadow_lower once (fair product path). Times:
#
#   ccc_build       — production ./ccc SRC -o bin
#   wrap_lower      — legacy ccc-run wrapper (recompiles lowerer each invoke)
#   nat_lower       — native shadow_lower (emit .c only)
#   tcc_build       — native --exe (emit buffer → libtcc → binary; no .c on disk)
#   host_build      — native lower + host cc -c + link runtime .o (old path)
#   ccc_run/tcc_run — binary wall time
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
export RT_LIBDIR="${SHADOW_RUNTIME_LIBDIR:-$ROOT/out/cc/lib}"
if [[ "$(uname -s)" == Darwin ]]; then
  RT_DYLIB="$RT_LIBDIR/libcc_runtime.dylib"
else
  RT_DYLIB="$RT_LIBDIR/libcc_runtime.so"
fi
export RT_DYLIB RT_LIBDIR
export TMP="${TMPDIR:-/tmp}/cc_serdes_bench_$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$CCC" ]]; then echo "FAIL: missing ccc at $CCC"; exit 1; fi
if [[ ! -f "$RT_O" ]]; then echo "FAIL: missing runtime $RT_O"; exit 1; fi

echo "[bench] building native shadow_lower once..."
cc -std=c11 -O2 -I"$ROOT/examples/serdes/c" -I"$ROOT/third_party/tcc" \
  -DCC_TCC_EXT_AVAILABLE -DCONFIG_CC_EXT \
  -c "$ROOT/examples/serdes/c/shadow_tcc_compile.c" -o "$TMP/shadow_tcc_compile.o"
"$CCC" "$ROOT/examples/serdes/c/shadow_lower.ccs" -o "$TMP/shadow_lower_native" \
  --cc-flags "-I$ROOT/examples/serdes/c -I$ROOT/third_party/tcc -DSHADOW_HAVE_LIBTCC=1" \
  --ld-flags "$TMP/shadow_tcc_compile.o -L$ROOT/third_party/tcc -ltcc"

if [[ ! -f "$RT_DYLIB" ]]; then
  echo "[bench] building runtime dylib $RT_DYLIB..."
  mkdir -p "$RT_LIBDIR"
  if [[ "$(uname -s)" == Darwin ]]; then
    cc -dynamiclib -install_name libcc_runtime.dylib -o "$RT_DYLIB" "$RT_O" -lpthread -lm
  else
    cc -shared -o "$RT_DYLIB" "$RT_O" -lpthread -lm
  fi
fi
export SHADOW_NATIVE="$TMP/shadow_lower_native"
export SHADOW_RUNTIME_LIBDIR="$RT_LIBDIR"
# Leaf soname load — no absolute / @rpath baked into binaries.
if [[ "$(uname -s)" == Darwin ]]; then
  export DYLD_LIBRARY_PATH="$RT_LIBDIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$RT_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

python3 - <<'PY'
import os, statistics, subprocess, sys, time
from pathlib import Path

ROOT = Path(os.environ["ROOT"])
ITERS = int(os.environ["ITERS"])
CCC = os.environ["CCC"]
WRAP = os.environ["SHADOW_WRAP"]
NATIVE = os.environ["SHADOW_NATIVE"]
RT_O = os.environ["RT_O"]
RT_LIBDIR = os.environ["SHADOW_RUNTIME_LIBDIR"]
RT_DYLIB = os.environ["RT_DYLIB"]
TMP = Path(os.environ["TMP"])
os.environ["SHADOW_RUNTIME_LIBDIR"] = RT_LIBDIR

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

def host_build(src: Path, bin_path: Path):
    c = TMP / (src.stem + ".c")
    o = TMP / (src.stem + ".o")
    run([NATIVE, str(src), "-o", str(c)])
    run(["cc", "-std=c11", "-O2", f"-I{ROOT}/out/include", "-c", str(c), "-o", str(o)])
    run(["cc", "-O2", str(o), RT_O, "-o", str(bin_path), "-lpthread", "-lm"])

def tcc_build(src: Path, bin_path: Path):
    run([NATIVE, str(src), "--exe", str(bin_path)])

print(f"iters={ITERS}")
print(f"ccc={CCC}")
print(f"native_lower={NATIVE}")
print(f"runtime_libdir={RT_LIBDIR}  (leaf soname libcc_runtime)")
print(f"wrapper={WRAP}  (ccc run --no-cache each invoke)")
print()
hdr = (
    f"{'recipe':<12} {'ccc_b':>8} {'wrap_lo':>8} {'nat_lo':>8} "
    f"{'tcc_b':>8} {'host_b':>8} {'ratio':>7} {'ccc_r':>8} {'tcc_r':>8}"
)
print(hdr)
print("-" * len(hdr))

rows = []
for name, rel in RECIPES:
    src = ROOT / rel
    ccc_bin = TMP / f"ccc_{name}.bin"
    tcc_bin = TMP / f"tcc_{name}.bin"
    host_bin = TMP / f"host_{name}.bin"
    # warm
    run([CCC, str(src), "-o", str(ccc_bin)])
    tcc_build(src, tcc_bin)
    host_build(src, host_bin)
    run([WRAP, str(src), "-o", str(TMP / f"{name}_warm.c")])

    ccc_b = timed([CCC, str(src), "-o", str(ccc_bin)])
    wrap_lo = timed([WRAP, str(src), "-o", str(TMP / f"{name}_wrap.c")])
    nat_lo = timed([NATIVE, str(src), "-o", str(TMP / f"{name}_nat.c")])

    tcc_b_ms = []
    for _ in range(ITERS):
        t0 = time.perf_counter()
        tcc_build(src, tcc_bin)
        tcc_b_ms.append((time.perf_counter() - t0) * 1000.0)
    tcc_b = (min(tcc_b_ms), statistics.mean(tcc_b_ms))

    host_b_ms = []
    for _ in range(ITERS):
        t0 = time.perf_counter()
        host_build(src, host_bin)
        host_b_ms.append((time.perf_counter() - t0) * 1000.0)
    host_b = (min(host_b_ms), statistics.mean(host_b_ms))

    ccc_r = timed([str(ccc_bin)])
    tcc_r = timed([str(tcc_bin)])
    ratio = tcc_b[0] / ccc_b[0] if ccc_b[0] > 0 else float("nan")
    rows.append((name, ccc_b, wrap_lo, nat_lo, tcc_b, host_b, ccc_r, tcc_r, ratio))
    print(
        f"{name:<12} {ccc_b[0]:8.1f} {wrap_lo[0]:8.1f} {nat_lo[0]:8.1f} "
        f"{tcc_b[0]:8.1f} {host_b[0]:8.1f} {ratio:6.2f}x {ccc_r[0]:8.1f} {tcc_r[0]:8.1f}"
    )

print()
print("ms best-of-N; ratio = tcc_build / ccc_build  (<1 shadow+libtcc faster to binary)")
print()
avg_wrap = statistics.mean(r[2][0] for r in rows)
avg_nat_lo = statistics.mean(r[3][0] for r in rows)
avg_ccc = statistics.mean(r[1][0] for r in rows)
avg_tcc = statistics.mean(r[4][0] for r in rows)
avg_host = statistics.mean(r[5][0] for r in rows)
avg_ratio = statistics.mean(r[-1] for r in rows)
print(f"mean ccc_build:          {avg_ccc:7.1f} ms")
print(f"mean wrapper lower:      {avg_wrap:7.1f} ms   ({avg_wrap/avg_ccc:5.1f}x ccc)")
print(f"mean native lower:       {avg_nat_lo:7.1f} ms   ({avg_nat_lo/avg_ccc:5.2f}x ccc)")
print(f"mean tcc --exe build:    {avg_tcc:7.1f} ms   ({avg_ratio:5.2f}x ccc)")
print(f"mean host-cc build:      {avg_host:7.1f} ms   ({avg_host/avg_ccc:5.2f}x ccc)")
print(
    "run delta ms (tcc - ccc):",
    ", ".join(f"{n}:{tr[0]-cr[0]:+.1f}" for n, _cb, _w, _nl, _tb, _hb, cr, tr, _rt in rows),
)
PY
