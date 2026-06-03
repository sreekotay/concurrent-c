#!/usr/bin/env bash
# buildrunperf.sh — loop build+run on a single .ccs file to measure compiler cost.
#
# Default target exercises UFCS + @string templates:
#   tests/string_template_smoke.ccs
#
# Example:
#   ./scripts/buildrunperf.sh
#   ./scripts/buildrunperf.sh --iters 50 tests/ufcs_in_lifted_closure_smoke.ccs
#   ./scripts/buildrunperf.sh --profile-hotspots
#   ./scripts/buildrunperf.sh --profile-hotspots 30 tests/ufcs_in_lifted_closure_smoke.ccs
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CCC="${CCC:-./cc/bin/ccc}"
SRC="tests/string_template_smoke.ccs"
ITERS=200
WARMUP=3
NO_CACHE=1
DO_RUN=1
BREAKDOWN=1
PROFILE_HOTSPOTS=0
PROFILE_SEC=10

usage() {
    cat <<'EOF'
Usage: buildrunperf.sh [options] [source.ccs]

Loop build (+ optional run) on one Concurrent-C source and report timing stats.
Defaults to tests/string_template_smoke.ccs × 200 with --no-cache (matches cc_test).

Options:
  --iters N            Measured iterations (default: 200)
  --warmup N           Warmup iterations, not counted (default: 3)
  --no-cache           Full rebuild every iteration (default, cc_test behavior)
  --use-cache          Use incremental build cache
  --no-run             Build only; skip executing the binary
  --no-breakdown       Skip one-shot emit-c vs clang+link split
  --profile-hotspots [SEC]
                       Sample .ccc-bin during emit-c-only builds for SEC seconds
                       (default: 20). Skips the timing loop unless --iters is set.
  -h, --help           Show this help

Profiling notes:
  - Targets cc emit-c (the CC compiler frontend), not clang+link.
  - For readable symbols: make -C cc BUILD=debug
EOF
}

ITERS_SET=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iters) ITERS="$2"; ITERS_SET=1; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --no-cache) NO_CACHE=1; shift ;;
        --use-cache) NO_CACHE=0; shift ;;
        --no-run) DO_RUN=0; shift ;;
        --no-breakdown) BREAKDOWN=0; shift ;;
        --breakdown) BREAKDOWN=1; shift ;;
        --profile-hotspots)
            PROFILE_HOTSPOTS=1
            if [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]]; then
                PROFILE_SEC="$2"
                shift 2
            else
                shift
            fi
            ;;
        -h|--help) usage; exit 0 ;;
        --*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            SRC="$1"
            shift
            ;;
    esac
done

# --profile-hotspots alone skips the timing loop.
if [[ "$PROFILE_HOTSPOTS" -eq 1 && "$ITERS_SET" -eq 0 ]]; then
    ITERS=0
    WARMUP=0
    DO_RUN=0
    BREAKDOWN=0
fi

CCC_BIN="${CCC_BIN:-$ROOT_DIR/cc/bin/.ccc-bin}"

if [[ ! -x "$CCC" ]]; then
    echo "buildrunperf: missing $CCC (build the compiler first: make -C cc)" >&2
    exit 1
fi

if [[ ! -f "$SRC" ]]; then
    echo "buildrunperf: source not found: $SRC" >&2
    exit 1
fi

STEM="$(basename "$SRC" .ccs)"
STEM="${STEM%.c}"
WORK_ROOT="$ROOT_DIR/tmp/buildrunperf/$$"
OUT_DIR="$WORK_ROOT/out"
BIN_DIR="$WORK_ROOT/bin"
BIN="$BIN_DIR/$STEM"
EMIT_DIR="$WORK_ROOT/emit"

mkdir -p "$OUT_DIR" "$BIN_DIR" "$EMIT_DIR"
trap 'rm -rf "$WORK_ROOT"' EXIT

CACHE_FLAG=()
if [[ "$NO_CACHE" -eq 1 ]]; then
    CACHE_FLAG=(--no-cache)
fi

# Skip header-lowering churn after the first ccc invocation in this process tree.
export CCC_SKIP_HEADER_CHECK=1

run_build() {
    local out_dir="$1"
    local bin_dir="$2"
    local bin_out="$3"
    "$CCC" build "${CACHE_FLAG[@]}" \
        --out-dir "$out_dir" \
        --bin-dir "$bin_dir" \
        --link "$SRC" \
        -o "$bin_out" \
        >/dev/null 2>&1
}

echo "=========================================================="
echo "buildrunperf"
echo "=========================================================="
echo "Source:     $SRC"
echo "Stem:       $STEM"
echo "Iterations: $ITERS measured (+ $WARMUP warmup)"
echo "Cache:      $([[ "$NO_CACHE" -eq 1 ]] && echo 'disabled (--no-cache)' || echo 'enabled')"
echo "Run binary: $([[ "$DO_RUN" -eq 1 ]] && echo yes || echo no)"
if [[ "$PROFILE_HOTSPOTS" -eq 1 ]]; then
    echo "Profile:    emit-c hotspots for ${PROFILE_SEC}s"
fi
echo "Host:       $(uname -s) $(uname -m)"
echo "CCC:        $CCC"
echo "CCC_BIN:    $CCC_BIN"
echo "=========================================================="
echo

if [[ "$PROFILE_HOTSPOTS" -eq 1 ]]; then
    if [[ ! -x "$CCC_BIN" ]]; then
        echo "buildrunperf: missing $CCC_BIN (build the compiler first: make -C cc)" >&2
        exit 1
    fi
    echo "=== Emit-c hotspot profile (${PROFILE_SEC}s) ==="
    export BRP_HOT_CCC_BIN="$CCC_BIN"
    export BRP_HOT_SRC="$SRC"
    export BRP_HOT_SEC="$PROFILE_SEC"
    export BRP_HOT_NO_CACHE="$NO_CACHE"
    export BRP_HOT_ROOT="$ROOT_DIR"
    python3 - <<'PY'
import os, re, subprocess, sys, time, signal
from collections import Counter

ccc_bin = os.environ["BRP_HOT_CCC_BIN"]
src = os.environ["BRP_HOT_SRC"]
sample_sec = int(os.environ["BRP_HOT_SEC"])
no_cache = os.environ["BRP_HOT_NO_CACHE"] == "1"
root = os.environ["BRP_HOT_ROOT"]
cache_flag = ["--no-cache"] if no_cache else []
pgrep_needle = ".ccc-bin"

work = os.path.join(root, "tmp", "buildrunperf", f"hot_{os.getpid()}")
out_dir = os.path.join(work, "out")
os.makedirs(out_dir, exist_ok=True)

loop_script = f"""#!/usr/bin/env python3
import os, subprocess, sys
ccc_bin = {ccc_bin!r}
src = {src!r}
out_dir = {out_dir!r}
cache_flag = {cache_flag!r}
while True:
    subprocess.run(
        [ccc_bin, "build", *cache_flag, "--emit-c-only", "--out-dir", out_dir, src],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
"""
loop_path = os.path.join(work, "loop.py")
with open(loop_path, "w") as f:
    f.write(loop_script)
os.chmod(loop_path, 0o755)

def bucket(name):
    n = name.lower()
    if "reparse" in n or n in ("preprocess", "parse_include", "tcc_open", "parse_define"):
        return "reparse / preprocess / includes"
    if "tcc" in n or n in ("next", "decl", "next_nomacro", "tccgen_compile", "tcc_compile",
                           "expr_cond", "unary", "block", "struct_decl", "skip_or_save_block"):
        return "TCC parse + codegen"
    if "ufcs" in n or "comptime" in n:
        return "UFCS + comptime hooks"
    if "visit" in n or n.startswith("pass_") or n.startswith("cc_visit") or "codegen" in n:
        return "CC visitor / codegen passes"
    if "compile" in n or n in ("main", "run_build_mode"):
        return "driver / orchestration"
    if "string_template" in n or "rewrite_string" in n:
        return "string template rewrite"
    if "preprocess" in n or "canonical" in n or "rewrite_" in n:
        return "text preprocess / rewrites"
    return "other"

funcs = Counter()
files = Counter()
sampled = 0
loop = subprocess.Popen([loop_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
deadline = time.time() + sample_sec
try:
    while time.time() < deadline:
        r = subprocess.run(["pgrep", "-f", pgrep_needle], capture_output=True, text=True)
        ps = [p for p in r.stdout.strip().split() if p != str(loop.pid)]
        if not ps:
            time.sleep(0.001)
            continue
        pid = ps[-1]
        p = subprocess.run(["sample", "-mayDie", "-fullPaths", pid, "1"],
                           capture_output=True, text=True)
        if p.returncode != 0 and not p.stdout:
            time.sleep(0.001)
            continue
        sampled += 1
        for line in p.stdout.splitlines():
            if "(in .ccc-bin)" not in line or "???" in line:
                continue
            m = re.search(r"(\d+)\s+(\S+)\s+\(in \.ccc-bin\)", line)
            if m:
                funcs[m.group(2)] += int(m.group(1))
            m2 = re.search(r"\[([^:]+\.(?:c|cpp|h)):(\d+)\]", line)
            if m2 and "concurrent-c" in m2.group(1):
                files[os.path.basename(m2.group(1))] += 1
finally:
    loop.send_signal(signal.SIGTERM)
    try:
        loop.wait(timeout=2)
    except subprocess.TimeoutExpired:
        loop.kill()

try:
    import shutil
    shutil.rmtree(work, ignore_errors=True)
except Exception:
    pass

total = sum(funcs.values()) or 1
print(f"Captured {sampled} process samples over {sample_sec}s")
print()
print("Top functions:")
for name, count in funcs.most_common(40):
    print(f"  {count:6d} ({100*count/total:5.1f}%)  {name}")

buckets = Counter()
for name, count in funcs.items():
    buckets[bucket(name)] += count
print()
print("Subsystem buckets (by function name):")
for name, count in buckets.most_common():
    print(f"  {count:6d} ({100*count/total:5.1f}%)  {name}")

if files:
    print()
    print("Top source files:")
    ftotal = sum(files.values()) or 1
    for name, count in files.most_common(15):
        print(f"  {count:6d} ({100*count/ftotal:5.1f}%)  {name}")

print()
print("Likely wins:")
print("  - cut reparse count in visit_codegen.c (cc__reparse_source_to_ast_ctx)")
print("  - skip cc_preprocess_emit_splice on already-expanded reparsed text")
print("  - cache comptime UFCS hook dylibs across builds")
PY
    echo
fi

if [[ "$BREAKDOWN" -eq 1 && "$ITERS" -gt 0 ]]; then
    echo "=== One-shot build phase breakdown ==="
    export BRP_BREAKDOWN_CCC="$CCC"
    export BRP_BREAKDOWN_SRC="$SRC"
    export BRP_BREAKDOWN_BIN="$BIN"
    export BRP_BREAKDOWN_OUT="$OUT_DIR"
    export BRP_BREAKDOWN_BIN_DIR="$BIN_DIR"
    export BRP_BREAKDOWN_EMIT="$EMIT_DIR/breakdown_emit"
    export BRP_BREAKDOWN_NO_CACHE="$NO_CACHE"
    export BRP_BREAKDOWN_DO_RUN="$DO_RUN"
    python3 - <<'PY'
import os, subprocess, sys, time

ccc = os.environ["BRP_BREAKDOWN_CCC"]
src = os.environ["BRP_BREAKDOWN_SRC"]
bin_path = os.environ["BRP_BREAKDOWN_BIN"]
out_dir = os.environ["BRP_BREAKDOWN_OUT"]
bin_dir = os.environ["BRP_BREAKDOWN_BIN_DIR"]
emit_dir = os.environ["BRP_BREAKDOWN_EMIT"]
no_cache = os.environ["BRP_BREAKDOWN_NO_CACHE"] == "1"
do_run = os.environ["BRP_BREAKDOWN_DO_RUN"] == "1"
cache_flag = ["--no-cache"] if no_cache else []

def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        print("command failed:", " ".join(cmd), file=sys.stderr)
        sys.exit(1)

os.makedirs(emit_dir, exist_ok=True)
t0 = time.perf_counter()
run([ccc, "build", *cache_flag, "--emit-c-only", "--out-dir", emit_dir, src])
emit_ms = (time.perf_counter() - t0) * 1000.0

t1 = time.perf_counter()
run([ccc, "build", *cache_flag, "--out-dir", out_dir, "--bin-dir", bin_dir,
     "--link", src, "-o", bin_path])
full_ms = (time.perf_counter() - t1) * 1000.0

run1_ms = run2_ms = 0.0
if do_run:
    t2 = time.perf_counter()
    run([bin_path])
    run1_ms = (time.perf_counter() - t2) * 1000.0
    t3 = time.perf_counter()
    run([bin_path])
    run2_ms = (time.perf_counter() - t3) * 1000.0

clang_ms = max(0.0, full_ms - emit_ms)

def pct(part, whole):
    return 0.0 if whole <= 0 else 100.0 * part / whole

print(f"  cc emit-c:   {emit_ms/1000:6.3f}s  ({pct(emit_ms, full_ms):5.1f}% of build)")
print(f"  clang+link:  {clang_ms/1000:6.3f}s  ({pct(clang_ms, full_ms):5.1f}% of build)")
print(f"  build total: {full_ms/1000:6.3f}s")
if do_run:
    total = full_ms + run1_ms
    print(f"  run (1st exec after link): {run1_ms/1000:6.3f}s  ({pct(run1_ms, total):5.1f}% of build+run)")
    print(f"  run (2nd exec, steady-state): {run2_ms/1000:6.3f}s")
    print()
    print("  Note: with --no-cache each loop iteration relinks, so run time includes")
    print("  macOS dyld cold-start (~100-150ms). Steady-state run is the 2nd exec line.")
print()
PY
    echo
fi

if [[ "$ITERS" -le 0 ]]; then
    echo "Done."
    exit 0
fi

echo "=== Loop ($ITERS iterations) ==="

export BRP_SRC="$SRC"
export BRP_BIN="$BIN"
export BRP_OUT_DIR="$OUT_DIR"
export BRP_BIN_DIR="$BIN_DIR"
export BRP_CCC="$CCC"
export BRP_ITERS="$ITERS"
export BRP_WARMUP="$WARMUP"
export BRP_DO_RUN="$DO_RUN"
export BRP_NO_CACHE="$NO_CACHE"

python3 - <<'PY'
import os, subprocess, sys, time

src = os.environ["BRP_SRC"]
bin_path = os.environ["BRP_BIN"]
out_dir = os.environ["BRP_OUT_DIR"]
bin_dir = os.environ["BRP_BIN_DIR"]
ccc = os.environ["BRP_CCC"]
iters = int(os.environ["BRP_ITERS"])
warmup = int(os.environ["BRP_WARMUP"])
do_run = os.environ["BRP_DO_RUN"] == "1"
no_cache = os.environ["BRP_NO_CACHE"] == "1"

cache_flag = ["--no-cache"] if no_cache else []

def build_cmd():
    return [
        ccc, "build", *cache_flag,
        "--out-dir", out_dir,
        "--bin-dir", bin_dir,
        "--link", src,
        "-o", bin_path,
    ]

def stats(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return {}
    def pct(p):
        if n == 1:
            return xs[0]
        i = (n - 1) * p
        lo = int(i)
        hi = min(lo + 1, n - 1)
        frac = i - lo
        return xs[lo] * (1 - frac) + xs[hi] * frac
    return {
        "n": n,
        "sum": sum(xs),
        "avg": sum(xs) / n,
        "min": xs[0],
        "max": xs[-1],
        "p50": pct(0.50),
        "p95": pct(0.95),
    }

def fmt_ms(s):
    return (f"total={s['sum']/1000:7.2f}s  "
            f"avg={s['avg']:7.1f}ms  "
            f"p50={s['p50']:7.1f}ms  "
            f"p95={s['p95']:7.1f}ms  "
            f"min={s['min']:7.1f}ms  "
            f"max={s['max']:7.1f}ms")

build_ms = []
run_ms = []
total_ms = []

total_count = warmup + iters
for i in range(total_count):
    measuring = i >= warmup
    label = "measure" if measuring else "warmup "
    idx = i - warmup + 1 if measuring else i + 1
    count = iters if measuring else warmup

    t0 = time.perf_counter()
    r = subprocess.run(build_cmd(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t1 = time.perf_counter()
    if r.returncode != 0:
        print(f"build failed on iteration {i + 1}", file=sys.stderr)
        sys.exit(1)

    r_elapsed = 0.0
    if do_run:
        t2 = time.perf_counter()
        rr = subprocess.run([bin_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t3 = time.perf_counter()
        if rr.returncode != 0:
            print(f"run failed on iteration {i + 1}", file=sys.stderr)
            sys.exit(1)
        r_elapsed = (t3 - t2) * 1000.0

    b_elapsed = (t1 - t0) * 1000.0
    tot = b_elapsed + r_elapsed

    if measuring:
        build_ms.append(b_elapsed)
        run_ms.append(r_elapsed)
        total_ms.append(tot)

    if (idx % 20 == 0) or (idx == count):
        print(f"  [{label} {idx:3d}/{count}]  build={b_elapsed:6.1f}ms  run={r_elapsed:6.1f}ms  total={tot:6.1f}ms", flush=True)

print()
bs = stats(build_ms)
ts = stats(total_ms)
print(f"Build: {fmt_ms(bs)}")
if do_run:
    rs = stats(run_ms)
    print(f"Run:   {fmt_ms(rs)}")
print(f"Total: {fmt_ms(ts)}")
if do_run and ts["sum"] > 0:
    print()
    print(f"Build share of build+run: {100.0 * bs['sum'] / ts['sum']:.1f}%")
    print(f"Run share of build+run:   {100.0 * rs['sum'] / ts['sum']:.1f}%")
print()
print(f"Throughput: {iters / (ts['sum']/1000.0):.2f} build+run/s")
if do_run and no_cache:
    print()
    print("Note: run times include dyld cold-start after each fresh link (--no-cache).")
    print("      Use --no-run for compile-only focus, or --use-cache to reuse binaries.")
PY

echo "Done."
