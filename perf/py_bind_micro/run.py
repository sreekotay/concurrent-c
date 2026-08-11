#!/usr/bin/env python3
"""Build and race py_module vs nanobind (nanobind-style func microbenchmark).

Python is the host: both sides are extension modules imported here.

    # full suite (720 methods, median of 5 builds + 5 runtimes)
    python3 perf/py_bind_micro/run.py > perf/baselines/py_bind_micro_$(date +%Y%m%d).txt

    # smoke (fewer decls / fewer samples)
    python3 perf/py_bind_micro/run.py --limit 24 --samples 1 --iters 200000

Requires: ccc on PATH or repo cc/bin/ccc; cmake; a venv with nanobind
(created automatically under perf/py_bind_micro/.venv).
"""
from __future__ import annotations

import argparse
import importlib
import importlib.machinery
import importlib.util
import os
import shutil
import statistics
import subprocess
import sys
import sysconfig
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
GEN = HERE / "generated"
OUT = HERE / "out"
BUILD = HERE / "build"
VENV = HERE / ".venv"


def median(xs):
    return statistics.median(xs)


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def ensure_venv_nanobind() -> Path:
    """Return the Python that has nanobind; create .venv if needed."""
    py = VENV / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not py.is_file():
        run([sys.executable, "-m", "venv", str(VENV)])
    try:
        run([str(py), "-c", "import nanobind"], capture_output=True)
    except subprocess.CalledProcessError:
        run([str(py), "-m", "pip", "install", "-q", "nanobind"])
    run([str(py), "-c", "import nanobind"], capture_output=True)
    return py


def find_ccc() -> Path:
    env = os.environ.get("CCC")
    if env and Path(env).is_file():
        return Path(env)
    for cand in (REPO / "cc" / "bin" / "ccc", REPO / "out" / "cc" / "bin" / "ccc"):
        if cand.is_file():
            return cand
    which = shutil.which("ccc")
    if which:
        return Path(which)
    sys.exit("ccc not found (set CCC= or build cc/bin/ccc)")


def time_cmd(cmd, cwd=None, env=None) -> float:
    t0 = time.perf_counter()
    run(cmd, cwd=cwd, env=env)
    return time.perf_counter() - t0


def strip_file(path: Path) -> None:
    strip = shutil.which("strip")
    if not strip:
        return
    # Mach-O: strip -x is common; GNU strip accepts bare path.
    try:
        run([strip, "-x", str(path)], capture_output=True)
    except subprocess.CalledProcessError:
        run([strip, str(path)], capture_output=True)


def load_extension(name: str, path: Path):
    """Import a .so / .abi3.so by file path under a stable module name."""
    # Ensure the directory is on sys.path and the basename matches name.
    d = str(path.parent)
    if d not in sys.path:
        sys.path.insert(0, d)
    # Drop a cached module so rebuilds are visible.
    sys.modules.pop(name, None)
    return importlib.import_module(name)


def build_cc(ccc: Path, samples: int) -> tuple[list[float], Path]:
    src = GEN / "func_cc.ccs"
    # Default layout: bin/<export>.abi3.so next to cwd=REPO.
    art = REPO / "bin" / "func_cc.abi3.so"
    times = []
    for i in range(samples):
        if art.exists():
            art.unlink()
        # --no-cache so compile time includes a real rebuild each sample.
        cmd = [
            str(ccc),
            "build",
            "--release",
            "--no-cache",
            str(src),
        ]
        times.append(time_cmd(cmd, cwd=str(REPO)))
        if not art.is_file():
            sys.exit(f"cc build produced no {art}")
    strip_file(art)
    # Copy into out/ for a stable PYTHONPATH alongside nanobind.
    OUT.mkdir(parents=True, exist_ok=True)
    dest = OUT / "func_cc.abi3.so"
    shutil.copy2(art, dest)
    return times, dest


def build_nanobind(py: Path, samples: int) -> tuple[list[float], Path]:
    cmake = shutil.which("cmake")
    if not cmake:
        sys.exit("cmake not found (brew install cmake / apt install cmake)")
    nb_cmake = subprocess.check_output(
        [str(py), "-c", "import nanobind; print(nanobind.cmake_dir())"],
        text=True,
    ).strip()
    OUT.mkdir(parents=True, exist_ok=True)
    times = []
    env = os.environ.copy()
    env["CMAKE_PREFIX_PATH"] = (
        nb_cmake + os.pathsep + env.get("CMAKE_PREFIX_PATH", "")
    )
    # Configure once; rebuild from clean each sample for fair compile times.
    for i in range(samples):
        if BUILD.exists():
            shutil.rmtree(BUILD)
        BUILD.mkdir(parents=True)
        conf = [
            cmake,
            "-S",
            str(HERE),
            "-B",
            str(BUILD),
            f"-DPython_EXECUTABLE={py}",
            f"-Dnanobind_DIR={nb_cmake}",
            "-DCMAKE_BUILD_TYPE=Release",
        ]
        t0 = time.perf_counter()
        run(conf, env=env, capture_output=True)
        run(
            [cmake, "--build", str(BUILD), "--config", "Release", "-j"],
            env=env,
            capture_output=True,
        )
        times.append(time.perf_counter() - t0)
    # Locate the produced extension.
    suffix = importlib.machinery.EXTENSION_SUFFIXES[0]
    candidates = list(BUILD.rglob(f"func_nanobind{suffix}"))
    if not candidates:
        candidates = list(BUILD.rglob("func_nanobind*.so")) + list(
            BUILD.rglob("func_nanobind*.dylib")
        )
    if not candidates:
        sys.exit(f"nanobind build produced no func_nanobind*{suffix} under {BUILD}")
    art = candidates[0]
    strip_file(art)
    dest = OUT / f"func_nanobind{suffix}"
    shutil.copy2(art, dest)
    # Also alias without the multiarch tag when needed — importlib uses suffix.
    return times, dest


def runtime_loop(fn, iters: int) -> float:
    # Warm.
    for _ in range(min(iters, 10000)):
        fn(1, 2, 3, 4, 5.0, 6.0)
    t0 = time.perf_counter()
    for _ in range(iters):
        fn(1, 2, 3, 4, 5.0, 6.0)
    return time.perf_counter() - t0


def pure_python_test_0000(a, b, c, d, e, f):
    return float(
        (float(a) + float(b) + float(c) + float(d) + float(e) + float(f))
    )


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=None, help="cap permutations")
    ap.add_argument("--samples", type=int, default=5, help="median-of-N builds/runs")
    ap.add_argument("--iters", type=int, default=10_000_000, help="calls per runtime sample")
    args = ap.parse_args(argv)

    py = ensure_venv_nanobind()
    ccc = find_ccc()
    nb_ver = subprocess.check_output(
        [str(py), "-c", "import nanobind; print(nanobind.__version__)"],
        text=True,
    ).strip()

    gen_cmd = [sys.executable, str(HERE / "gen.py")]
    if args.limit is not None:
        gen_cmd += ["--limit", str(args.limit)]
    run(gen_cmd)

    n_methods = sum(1 for _ in (GEN / "func_cc.ccs").open() if "Bench_test_" in _)

    print(f"# perf/py_bind_micro — nanobind-style func suite (release only)")
    print(f"# host: {sys.platform} {os.uname().machine if hasattr(os, 'uname') else ''}")
    print(f"# python: {sys.version.split()[0]} ({sys.executable})")
    print(f"# venv_python: {py}")
    print(f"# ccc: {ccc}")
    print(f"# nanobind: {nb_ver}")
    print(f"# methods: {n_methods}")
    print(f"# samples: {args.samples}  iters: {args.iters}")
    print(f"# types: int32_t uint32_t int64_t uint64_t float double")
    print(f"# note: uint16_t from nanobind's suite replaced by double (CC_PY_IN)")
    print("#")

    print("building py_module (ccc --release --no-cache)…", file=sys.stderr)
    cc_times, cc_art = build_cc(ccc, args.samples)
    print("building nanobind (cmake Release)…", file=sys.stderr)
    nb_times, nb_art = build_nanobind(py, args.samples)

    cc_compile = median(cc_times)
    nb_compile = median(nb_times)
    cc_size = cc_art.stat().st_size
    nb_size = nb_art.stat().st_size

    print(f"compile_cc_s_samples {' '.join(f'{t:.3f}' for t in cc_times)}")
    print(f"compile_nanobind_s_samples {' '.join(f'{t:.3f}' for t in nb_times)}")
    print(f"RESULT func compile_s cc {cc_compile:.3f}")
    print(f"RESULT func compile_s nanobind {nb_compile:.3f}")
    print(f"RESULT func compile_ratio_cc_over_nb {cc_compile / nb_compile:.3f}")
    print(f"RESULT func size_bytes cc {cc_size}")
    print(f"RESULT func size_bytes nanobind {nb_size}")
    print(f"RESULT func size_ratio_cc_over_nb {cc_size / nb_size:.3f}")

    # Runtime — import from OUT.
    if str(OUT) not in sys.path:
        sys.path.insert(0, str(OUT))
    # Prefer the venv interpreter's extension suffix for nanobind artifact name.
    m_cc = load_extension("func_cc", cc_art)
    # nanobind module name is func_nanobind; file may be func_nanobind.cpython-*.so
    sys.modules.pop("func_nanobind", None)
    m_nb = importlib.import_module("func_nanobind")

    f_cc = m_cc.test_0000
    f_nb = m_nb.test_0000
    # Correctness cross-check.
    v_cc = f_cc(1, 2, 3, 4, 5.0, 6.0)
    v_nb = f_nb(1, 2, 3, 4, 5.0, 6.0)
    v_py = pure_python_test_0000(1, 2, 3, 4, 5.0, 6.0)
    if abs(v_cc - v_py) > 1e-5 or abs(v_nb - v_py) > 1e-5:
        sys.exit(f"MISMATCH values cc={v_cc} nb={v_nb} py={v_py}")

    rt_cc, rt_nb, rt_py = [], [], []
    for _ in range(args.samples):
        rt_cc.append(runtime_loop(f_cc, args.iters))
        rt_nb.append(runtime_loop(f_nb, args.iters))
        rt_py.append(runtime_loop(pure_python_test_0000, args.iters))

    m_cc_t, m_nb_t, m_py_t = median(rt_cc), median(rt_nb), median(rt_py)
    ns_cc = m_cc_t * 1e9 / args.iters
    ns_nb = m_nb_t * 1e9 / args.iters
    ns_py = m_py_t * 1e9 / args.iters

    print(f"runtime_cc_s_samples {' '.join(f'{t:.3f}' for t in rt_cc)}")
    print(f"runtime_nanobind_s_samples {' '.join(f'{t:.3f}' for t in rt_nb)}")
    print(f"runtime_python_s_samples {' '.join(f'{t:.3f}' for t in rt_py)}")
    print(f"RESULT func runtime_s cc {m_cc_t:.3f}")
    print(f"RESULT func runtime_s nanobind {m_nb_t:.3f}")
    print(f"RESULT func runtime_s python {m_py_t:.3f}")
    print(f"RESULT func runtime_ns_per_call cc {ns_cc:.1f}")
    print(f"RESULT func runtime_ns_per_call nanobind {ns_nb:.1f}")
    print(f"RESULT func runtime_ns_per_call python {ns_py:.1f}")
    print(f"RESULT func runtime_ratio_cc_over_nb {m_cc_t / m_nb_t:.3f}")
    print(f"RESULT func runtime_ratio_cc_over_python {m_cc_t / m_py_t:.3f}")
    print(f"RESULT func runtime_ratio_nb_over_python {m_nb_t / m_py_t:.3f}")

    print("#")
    print(
        f"# summary: compile cc={cc_compile:.2f}s nb={nb_compile:.2f}s "
        f"({cc_compile / nb_compile:.2f}x); "
        f"size cc={cc_size} nb={nb_size} ({cc_size / nb_size:.2f}x); "
        f"runtime cc={ns_cc:.1f}ns nb={ns_nb:.1f}ns py={ns_py:.1f}ns "
        f"(cc/nb={m_cc_t / m_nb_t:.2f}x)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
