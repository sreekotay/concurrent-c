"""Python → JS / npm: this package vs the usual alternatives.

Two jobs get conflated in this space. Measure them separately.

Real Node (this package)
    `require('fs')`, native addons, the npm in cwd, crash isolation.
    Honest alternatives: drive a `node` child yourself. Spawn-per-call
    (`node -e`) is what people write first. A persistent JSON-stdio loop
    is the DIY that actually competes. PyExecJS was that, abandoned.
    `pythonodejs` claims an embed; treat as optional if importable.

A JS engine inside CPython (not Node)
    pythonmonkey — SpiderMonkey + CommonJS `require` of *pure JS*.
    mini-racer / PyMiniRacer — V8 isolate; no `require`, no `node:*`.
    quickjs, js2py, STPyV8 — same class.
    Can win RTT (in-process). Cannot load `bcrypt` / `sharp` / `sqlite3`
    node addons. Shared fate with the Python process.

    python -m cc_node.benchmarks.vs_alts

Optional engines: import if present, else SKIP. No extra deps on the
wheel. RESULT lines are machine-comparable; SKIP is articulate.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
import cc_node  # noqa: E402

NODE = os.environ.get("CC_NODE_BIN") or shutil.which("node") or "node"

SUM_JS = (
    "(a) => { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; "
    "return s; }"
)
N_BULK = 1 << 20  # 1M floats → 8MB as Float64


def _bulk_list():
    return [float(i % 97) for i in range(N_BULK)]


def _bulk_sum_expect(xs):
    return float(sum(xs))

# Persistent JSON-stdio child. No handles, no shm, protocol on stdout
# (the usual DIY). Not cc-node's dedicated-fd wire.
_DIY_BROKER = r"""
'use strict';
let buf = Buffer.alloc(0);
function reply(obj) {
  process.stdout.write(JSON.stringify(obj) + '\n');
}
function handle(line) {
  let m;
  try { m = JSON.parse(line); } catch (e) {
    reply({e: 'bad json'}); return;
  }
  try {
    let r;
    if (m.op === 'eval') r = (0, eval)(m.src);
    else if (m.op === 'apply') r = globalThis[m.name](...(m.args || []));
    else throw new Error('unknown op');
    if (r && typeof r.then === 'function') {
      r.then((v) => reply({v: v}), (e) => reply({e: String(e && e.message || e)}));
      return;
    }
    reply({v: r});
  } catch (e) {
    reply({e: String(e && e.message !== undefined ? e.message : e)});
  }
}
process.stdin.on('data', (chunk) => {
  buf = Buffer.concat([buf, chunk]);
  for (;;) {
    const i = buf.indexOf(10);
    if (i < 0) break;
    const line = buf.subarray(0, i).toString('utf8');
    buf = buf.subarray(i + 1);
    handle(line);
  }
});
"""


def _result(name, value, fmt=None):
    if isinstance(value, str) and value.startswith("SKIP"):
        print("SKIP %s %s" % (name, value[5:].lstrip()))
        return
    if fmt == "ms1":
        print("RESULT %s %.1f" % (name, value))
    elif fmt == "s3":
        print("RESULT %s %.3f" % (name, value))
    else:
        print("RESULT %s %s" % (name, value))


def _cap(name, ok, detail=""):
    print("CAPABILITY %s %s%s" % (
        name, "true" if ok else "false",
        ("  " + detail) if detail else ""))


class DiyStdio(object):
    def __init__(self):
        self._p = subprocess.Popen(
            [NODE, "-e", _DIY_BROKER],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    def _req(self, obj):
        self._p.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
        self._p.stdin.flush()
        line = self._p.stdout.readline()
        if not line:
            raise RuntimeError("diy stdio: node child exited")
        msg = json.loads(line)
        if "e" in msg:
            raise RuntimeError(msg["e"])
        return msg.get("v")

    def eval(self, src):
        return self._req({"op": "eval", "src": src})

    def apply(self, name, *args):
        return self._req({"op": "apply", "name": name, "args": list(args)})

    def destroy(self):
        try:
            self._p.stdin.close()
        except Exception:
            pass
        try:
            self._p.kill()
        except Exception:
            pass
        try:
            self._p.wait(timeout=2)
        except Exception:
            pass


def _us(dt, n):
    us = dt / float(n) * 1e6
    if us < 0.5:
        return "<1"
    return round(us)


def _time_loop(n, fn):
    t0 = time.perf_counter()
    for i in range(n):
        fn(i)
    return time.perf_counter() - t0


def bench_cc_node():
    t0 = time.perf_counter()
    js = cc_node.create()
    js.eval("1")
    _result("cc_node.spawn_ms", round((time.perf_counter() - t0) * 1000))

    f = js.eval("(x) => x")
    f(1)
    dt = _time_loop(500, f)
    _result("cc_node.rtt_us", _us(dt, 500))

    g = js.eval("(cb) => cb(21) * 2")
    g(lambda x: x + 1)
    dt = _time_loop(200, lambda _i: g(lambda x: x + 1))
    _result("cc_node.callback_roundtrip_us", _us(dt, 200))

    import array
    big = array.array("d", [float(i % 97) for i in range(N_BULK)])
    expect = _bulk_sum_expect(big)
    sm = js.eval(SUM_JS)
    got = sm(big)
    if abs(got - expect) > 1e-3:
        raise RuntimeError("cc-node shm sum mismatch")
    t0 = time.perf_counter()
    for _ in range(10):
        sm(big)
    _result("cc_node.bulk_8mb_shm_ms", (time.perf_counter() - t0) / 10 * 1000,
            fmt="ms1")

    biglist = _bulk_list()
    sm(biglist)
    t0 = time.perf_counter()
    for _ in range(3):
        sm(biglist)
    _result("cc_node.bulk_8mb_json_list_ms",
            round((time.perf_counter() - t0) / 3 * 1000))

    _cap("cc_node.require_path",
         js.require("path").join("a", "b") == "a/b")
    fd, tmp = tempfile.mkstemp()
    try:
        os.write(fd, b"hi")
        os.close(fd)
        got = js.require("fs").readFileSync(tmp, "utf8")
        _cap("cc_node.require_fs", got == "hi")
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    then = js.eval("async (x) => { await Promise.resolve(); return x * 2 }")
    _cap("cc_node.thenable", then(21) == 42)
    mapped = js.eval("(f) => [1, 2, 3].map(f)")(lambda x, *rest: x * 10)
    _cap("cc_node.python_callback", mapped == [10, 20, 30])
    js.destroy()


def bench_diy_stdio():
    t0 = time.perf_counter()
    diy = DiyStdio()
    diy.eval("1")
    _result("diy_stdio.spawn_ms", round((time.perf_counter() - t0) * 1000))
    try:
        diy.eval("globalThis.__id = (x) => x")
        diy.apply("__id", 1)
        dt = _time_loop(500, lambda i: diy.apply("__id", i))
        _result("diy_stdio.rtt_us", _us(dt, 500))

        _result("diy_stdio.callback_roundtrip_us",
                "SKIP no nested callback protocol")

        biglist = _bulk_list()
        diy.eval("globalThis.__sum = " + SUM_JS)
        diy.apply("__sum", biglist)
        t0 = time.perf_counter()
        for _ in range(3):
            diy.apply("__sum", biglist)
        _result("diy_stdio.bulk_8mb_json_list_ms",
                round((time.perf_counter() - t0) / 3 * 1000))
        _result("diy_stdio.bulk_8mb_shm_ms", "SKIP JSON-only DIY")

        _cap("diy_stdio.require_path",
             diy.eval("require('path').join('a', 'b')") == "a/b")
        fd, tmp = tempfile.mkstemp()
        try:
            os.write(fd, b"hi")
            os.close(fd)
            got = diy.eval(
                "require('fs').readFileSync(%s, 'utf8')" % json.dumps(tmp))
            _cap("diy_stdio.require_fs", got == "hi")
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        _cap("diy_stdio.thenable",
             diy.eval("(async () => { await Promise.resolve(); return 42 })()")
             == 42)
        _cap("diy_stdio.python_callback", False,
             "no callback protocol")
    finally:
        diy.destroy()


def bench_spawn_each():
    # What people write first. Includes process spawn every call.
    n = 50
    subprocess.check_output(
        [NODE, "-e", "console.log(JSON.stringify(1))"])
    t0 = time.perf_counter()
    for i in range(n):
        out = subprocess.check_output(
            [NODE, "-e", "console.log(JSON.stringify((x => x)(%d)))" % i])
        if json.loads(out) != i:
            raise RuntimeError("spawn-each mismatch")
    _result("spawn_each.rtt_us", round((time.perf_counter() - t0) / n * 1e6))
    _result("spawn_each.spawn_ms", "SKIP folded into rtt (new process/call)")
    _result("spawn_each.callback_roundtrip_us", "SKIP new process/call")
    _result("spawn_each.bulk_8mb_shm_ms", "SKIP not a persistent child")
    _result("spawn_each.bulk_8mb_json_list_ms", "SKIP argv size")
    _cap("spawn_each.require_path",
         json.loads(subprocess.check_output(
             [NODE, "-e",
              "console.log(JSON.stringify(require('path').join('a','b')))"]))
         == "a/b")
    _cap("spawn_each.require_fs", True, "real node; new process/call")
    _cap("spawn_each.thenable", True, "real node; new process/call")
    _cap("spawn_each.python_callback", False, "no callback protocol")


def bench_pythonmonkey():
    try:
        import pythonmonkey as pm
    except ImportError:
        _result("pythonmonkey.rtt_us", "SKIP not installed")
        _cap("pythonmonkey.require_path", False, "not installed")
        _cap("pythonmonkey.require_fs", False, "not installed")
        return
    t0 = time.perf_counter()
    f = pm.eval("(x) => x")
    f(1)
    _result("pythonmonkey.spawn_ms",
            round((time.perf_counter() - t0) * 1000))
    dt = _time_loop(500, f)
    _result("pythonmonkey.rtt_us", _us(dt, 500))
    try:
        g = pm.eval("(cb) => cb(21) * 2")
        g(lambda x: x + 1)
        dt = _time_loop(200, lambda _i: g(lambda x: x + 1))
        _result("pythonmonkey.callback_roundtrip_us", _us(dt, 200))
        _cap("pythonmonkey.python_callback", True)
    except Exception as e:
        _result("pythonmonkey.callback_roundtrip_us",
                "SKIP %s" % type(e).__name__)
        _cap("pythonmonkey.python_callback", False, str(e)[:80])
    try:
        arr = list(range(N_BULK))  # force a real scan, not .length on a wrapper
        sm = pm.eval(SUM_JS)
        sm(arr)
        t0 = time.perf_counter()
        for _ in range(10):
            sm(arr)
        _result("pythonmonkey.bulk_scan_1m_ms",
                (time.perf_counter() - t0) / 10 * 1000, fmt="ms1")
    except Exception as e:
        _result("pythonmonkey.bulk_scan_1m_ms",
                "SKIP %s" % type(e).__name__)
    _result("pythonmonkey.bulk_8mb_shm_ms",
            "SKIP in-process engine, not a wire")
    biglist = _bulk_list()
    try:
        sm = pm.eval(SUM_JS)
        sm(biglist)
        t0 = time.perf_counter()
        for _ in range(3):
            sm(biglist)
        _result("pythonmonkey.bulk_8mb_list_ms",
                round((time.perf_counter() - t0) / 3 * 1000))
    except Exception as e:
        _result("pythonmonkey.bulk_8mb_list_ms",
                "SKIP %s" % type(e).__name__)
    _cap("pythonmonkey.require_path", False,
         "no node:path (CommonJS of JS files, not Node builtins)")
    _cap("pythonmonkey.require_fs", False,
         "no node:fs")
    try:
        then = pm.eval("async (x) => { await Promise.resolve(); return x * 2 }")
        got = then(21)
        _cap("pythonmonkey.thenable", got == 42 or got == 42.0,
             "" if (got == 42 or got == 42.0) else repr(got)[:60])
    except Exception as e:
        _cap("pythonmonkey.thenable", False, str(e)[:80])


def bench_mini_racer():
    try:
        from py_mini_racer import MiniRacer
    except ImportError:
        _result("mini_racer.rtt_us", "SKIP not installed")
        _cap("mini_racer.require_path", False, "not installed")
        _cap("mini_racer.require_fs", False, "V8 isolate, not Node")
        return
    t0 = time.perf_counter()
    ctx = MiniRacer()
    ctx.eval("const __id = (x) => x;")
    # call() may be sync or need eval
    def _id(i):
        try:
            return ctx.call("__id", i)
        except Exception:
            return ctx.eval("__id(%d)" % i)
    _id(1)
    _result("mini_racer.spawn_ms",
            round((time.perf_counter() - t0) * 1000))
    dt = _time_loop(500, _id)
    _result("mini_racer.rtt_us", _us(dt, 500))
    _result("mini_racer.callback_roundtrip_us",
            "SKIP V8 isolate; no Python callable door in this bench")
    try:
        ctx.eval("const __sum = " + SUM_JS)
        biglist = _bulk_list()
        ctx.call("__sum", biglist)
        t0 = time.perf_counter()
        for _ in range(3):
            ctx.call("__sum", biglist)
        _result("mini_racer.bulk_8mb_json_list_ms",
                round((time.perf_counter() - t0) / 3 * 1000))
    except Exception as e:
        _result("mini_racer.bulk_8mb_json_list_ms",
                "SKIP %s" % type(e).__name__)
    _result("mini_racer.bulk_8mb_shm_ms", "SKIP no shm")
    _cap("mini_racer.require_path", False, "no require")
    _cap("mini_racer.require_fs", False, "V8 isolate, not Node")
    _cap("mini_racer.python_callback", False, "not measured")
    try:
        got = ctx.eval("(async () => 42)()")
        _cap("mini_racer.thenable", False,
             "eval returned %r (isolate; no Node await story)" % (got,))
    except Exception as e:
        _cap("mini_racer.thenable", False, str(e)[:80])


def main():
    if not shutil.which(NODE) and not os.path.isfile(NODE):
        print("SKIP need node on PATH")
        return 1
    bench_cc_node()
    bench_diy_stdio()
    bench_spawn_each()
    bench_pythonmonkey()
    bench_mini_racer()
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
