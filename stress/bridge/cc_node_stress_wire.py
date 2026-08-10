"""Adversarial multi-domain storm for concurrent-c-node.

    ./stress/bridge/run.sh
    PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
    CHAOS_SCALE=full PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
    CHAOS_SCALE=soak PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py

Latency demos stay under pypi/cc-node/cc_node/examples/.  This file is stress only.

RESULT lines + OK booleans; non-zero exit on any FAIL.
"""
from __future__ import annotations

import array
import gc
import os
import platform
import resource
import sys
import threading
import time

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
sys.path.insert(0, os.path.join(_ROOT, "pypi", "cc-node"))
import cc_node  # noqa: E402

# CHAOS_SCALE: quick < full < soak (soak implies full sizes + long RSS).
SCALE = (os.environ.get("CHAOS_SCALE")
         or os.environ.get("CC_NODE_STRESS")
         or "quick").lower()
SOAK = SCALE == "soak"
FULL = SCALE in ("full", "soak")
fails = 0
MB = 1024 * 1024


def rss_bytes() -> int:
    """Best-effort current RSS (macOS maxrss is bytes; Linux VmRSS)."""
    if platform.system() == "Darwin":
        return int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) * 1024
    except OSError:
        pass
    return int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss) * 1024


def ok(name: str, cond: bool) -> None:
    global fails
    if not cond:
        fails += 1
    print("OK %s %s" % (name, "true" if cond else "false"))


def result(fmt: str, *args) -> None:
    print("RESULT " + fmt % args)


def make_id(js):
    return js.eval("(x) => x")


def make_cb_doubler(js):
    return js.eval("(cb, n) => { let s = 0; for (let i = 0; i < n; i++) s += cb(i); return s; }")


def make_len(js):
    return js.eval("(a) => a.length")


def multi_child_fanout() -> None:
    print("=== multi_child_fanout ===")
    n = 6 if FULL else 4
    t0 = time.perf_counter()
    kids = [cc_node.create() for _ in range(n)]
    ids = [make_id(js) for js in kids]
    # Concurrent calls across children from worker threads.
    out = [None] * n
    errs = []

    def worker(i):
        try:
            f = ids[i]
            s = 0
            for k in range(200 if FULL else 80):
                s += f(k)
            out[i] = s
        except Exception as e:
            errs.append(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    for js in kids:
        js.destroy()
    result("multi_child_fanout_n %d", n)
    result("multi_child_fanout_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("multi_child_fanout_no_err", not errs)
    want = sum(range(200 if FULL else 80))
    ok("multi_child_fanout_sums", all(v == want for v in out))


def callback_blizzard() -> None:
    print("=== callback_blizzard ===")
    js = cc_node.create()
    doubler = make_cb_doubler(js)
    doubler(lambda x: x + 1, 8)  # warm
    n = 400 if FULL else 150
    t0 = time.perf_counter()
    total = 0
    for _ in range(n):
        total += doubler(lambda x: x * 2, 32)
    # Nested: JS hands Python a JS function; returning that handle used
    # to trip protocol violation when __del__ released mid-flight.
    nest = js.eval("(cb) => cb((x) => cb((y) => x + y)(3))(10)")
    nested = nest(lambda f: f)
    # Same shape as bench_wire: JS calls Python callback.
    doubler2 = js.eval("(cb) => cb(21) * 2")
    roundtrip = doubler2(lambda x: x + 1)
    js.destroy()
    result("callback_blizzard_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("callback_blizzard_calls %d", n)
    ok("callback_blizzard_total", total == n * sum(i * 2 for i in range(32)))
    ok("callback_blizzard_nested", nested == 13)
    ok("callback_blizzard_roundtrip", roundtrip == 44)


def shm_hail() -> None:
    print("=== shm_hail ===")
    n_kids = 4 if FULL else 3
    elems = 1 << 19 if FULL else 1 << 18  # 4MB / 2MB
    rounds = 5 if FULL else 3
    kids = [cc_node.create() for _ in range(n_kids)]
    lens = [make_len(js) for js in kids]
    bufs = [array.array("d", [float((i + j) % 97) for j in range(elems)])
            for i in range(n_kids)]
    for ln, buf in zip(lens, bufs):
        ln(buf)  # warm spill path
    t0 = time.perf_counter()
    errs = []
    out = [0] * n_kids

    def worker(i):
        try:
            s = 0
            for _ in range(rounds):
                s += lens[i](bufs[i])
            out[i] = s
        except Exception as e:
            errs.append(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_kids)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    for js in kids:
        js.destroy()
    result("shm_hail_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("shm_hail_mb_moved %.1f", (n_kids * elems * 8 * rounds) / (1024 * 1024))
    ok("shm_hail_no_err", not errs)
    ok("shm_hail_lens", all(v == elems * rounds for v in out))


def teardown_derby() -> None:
    print("=== teardown_derby ===")
    rounds = 30 if FULL else 15
    closed_rejects = 0
    double_ok = 0
    t0 = time.perf_counter()
    for r in range(rounds):
        js = cc_node.create()
        f = make_id(js)
        # Fire work from a side thread while main destroys.
        box = {"err": None, "n": 0}

        def hammer():
            try:
                for k in range(200):
                    box["n"] += f(k)
            except Exception as e:
                box["err"] = e

        th = threading.Thread(target=hammer)
        th.start()
        time.sleep(0.001 * (r % 3))
        js.destroy()
        th.join(timeout=10)
        if box["err"] is not None and (
            "closed" in str(box["err"]).lower() or "exit" in str(box["err"]).lower()
        ):
            closed_rejects += 1
        js.destroy()  # idempotent
        double_ok += 1
        try:
            f(1)
        except Exception as e:
            if "closed" in str(e).lower():
                closed_rejects += 1
    result("teardown_derby_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("teardown_derby_closed_rejects %d", closed_rejects)
    ok("teardown_derby_double", double_ok == rounds)
    ok("teardown_derby_rejects", closed_rejects >= rounds)


def eval_storm() -> None:
    print("=== eval_storm ===")
    js = cc_node.create()
    n = 800 if FULL else 300
    t0 = time.perf_counter()
    acc = 0
    for i in range(n):
        # Fresh function each time — exercises parse + handle ledger.
        f = js.eval("(x) => x + %d" % (i % 17))
        acc += f(i)
        if i % 50 == 0:
            js.release(f)
    stats = js.stats()
    js.destroy()
    result("eval_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("eval_storm_n %d", n)
    result("eval_storm_stats %s", stats)
    ok("eval_storm_acc", acc == sum(i + (i % 17) for i in range(n)))


def handle_boomerang() -> None:
    """Return JS handles from Python callbacks while GC releases race the wire."""
    print("=== handle_boomerang ===")
    js = cc_node.create()
    # Each iter: JS → Python with a fresh fn; Python returns it; JS calls it.
    boom = js.eval(
        "(cb, n) => { let s = 0; for (let i = 0; i < n; i++) "
        "s += cb((x) => x + i)(1); return s; }"
    )
    # Compose: Python returns a curried JS fn built from another callback.
    compose = js.eval(
        "(cb) => { const f = cb((a) => (b) => a * 10 + b); return f(3)(4); }"
    )
    n = 250 if FULL else 100
    t0 = time.perf_counter()
    total = boom(lambda g: g, n)
    composed = compose(lambda g: g)
    # Fan the same shape across many short-lived handles + forced GC.
    deep = js.eval(
        "(cb) => cb((x) => cb((y) => cb((z) => x + y + z)(1))(2))(3)"
    )
    deep_ok = 0
    for i in range(n):
        if deep(lambda f: f) == 6:
            deep_ok += 1
        if i % 7 == 0:
            gc.collect()
    # Multi-arg return: hand back one of several handles.
    pick = js.eval(
        "(cb) => { const a = (x) => x + 1; const b = (x) => x + 2; "
        "return cb(a, b)(10); }"
    )
    picked = pick(lambda a, b: b)
    js.destroy()
    result("handle_boomerang_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("handle_boomerang_n %d", n)
    ok("handle_boomerang_total", total == sum(1 + i for i in range(n)))
    ok("handle_boomerang_compose", composed == 34)
    ok("handle_boomerang_deep", deep_ok == n)
    ok("handle_boomerang_pick", picked == 12)


def exception_hail() -> None:
    """Alternate throws and values from Python callbacks; messages intact."""
    print("=== exception_hail ===")
    js = cc_node.create()
    runner = js.eval(
        "(f, n) => { let hits = 0, sum = 0; for (let i = 0; i < n; i++) { "
        "try { sum += f(i); } catch (e) { "
        "if (String(e.message).indexOf('hail') >= 0) hits++; else throw e; } "
        "} return [hits, sum]; }"
    )
    n = 400 if FULL else 160

    def flaky(i):
        if i % 3 == 0:
            raise ValueError("hail-%d" % i)
        return i * 2

    t0 = time.perf_counter()
    hits, s = runner(flaky, n)
    # Throw while returning-handle path is warm (should not corrupt wire).
    boom_ret = js.eval(
        "(cb, n) => { let ok = 0; for (let i = 0; i < n; i++) { "
        "try { cb((x) => x); ok++; } catch (e) { "
        "if (String(e.message).indexOf('nope') < 0) throw e; } } return ok; }"
    )

    state = {"i": 0}

    def sometimes(g):
        state["i"] += 1
        if state["i"] % 2 == 0:
            raise RuntimeError("nope")
        return g

    recovered = boom_ret(sometimes, n)
    js.destroy()
    want_hits = (n + 2) // 3  # i % 3 == 0
    want_sum = sum(i * 2 for i in range(n) if i % 3 != 0)
    result("exception_hail_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("exception_hail_hits %d", hits)
    ok("exception_hail_hits", hits == want_hits)
    ok("exception_hail_sum", s == want_sum)
    ok("exception_hail_recover", recovered == (n + 1) // 2)


def thenable_storm() -> None:
    """Awaited thenables interleaved with sync callbacks + handle returns."""
    print("=== thenable_storm ===")
    js = cc_node.create()
    af = js.eval(
        "async (cb, n) => { let s = 0; for (let i = 0; i < n; i++) { "
        "s += await Promise.resolve(cb(i)); } return s; }"
    )
    a_boom = js.eval(
        "async (cb, n) => { let s = 0; for (let i = 0; i < n; i++) { "
        "const f = await Promise.resolve(cb((x) => x + i)); "
        "s += f(1); } return s; }"
    )
    n = 200 if FULL else 80
    t0 = time.perf_counter()
    s1 = af(lambda i: i * 3, n)
    s2 = a_boom(lambda g: g, n)
    # Mix: sync call sandwiched between async results on same domain.
    sync = js.eval("(x) => x * x")
    mid = sync(12)
    s3 = af(lambda i: i + 1, n)
    js.destroy()
    result("thenable_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("thenable_storm_sum", s1 == sum(i * 3 for i in range(n)))
    ok("thenable_storm_boom", s2 == sum(1 + i for i in range(n)))
    ok("thenable_storm_sync_mid", mid == 144)
    ok("thenable_storm_sum2", s3 == sum(i + 1 for i in range(n)))


def thenable_reject_storm() -> None:
    """Promise.reject / async throw interleaved with handle-return cbs."""
    print("=== thenable_reject_storm ===")
    js = cc_node.create()
    n = 200 if FULL else 80
    mix = js.eval(
        "async (cb, n) => { let ok = 0, bad = 0; "
        "for (let i = 0; i < n; i++) { try { "
        "if (i % 2 === 0) await Promise.reject(new Error('rej-' + i)); "
        "else { const f = await Promise.resolve(cb((x) => x + i)); "
        "ok += f(1); } "
        "} catch (e) { "
        "if (String(e.message).indexOf('rej-') >= 0) bad++; else throw e; } "
        "} return [ok, bad]; }"
    )
    async_throw = js.eval(
        "async (n) => { let hits = 0; for (let i = 0; i < n; i++) { "
        "try { await (async () => { throw new Error('athrow-' + i); })(); } "
        "catch (e) { if (String(e.message).indexOf('athrow-') >= 0) hits++; "
        "else throw e; } } return hits; }"
    )
    t0 = time.perf_counter()
    ok_sum, bad = mix(lambda g: g, n)
    hits = async_throw(n)
    # After rejects, a plain success must still demux cleanly.
    alive = js.eval("(x) => x + 1")(41)
    js.destroy()
    want_ok = sum(1 + i for i in range(n) if i % 2 == 1)
    want_bad = (n + 1) // 2
    result("thenable_reject_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("thenable_reject_storm_ok", ok_sum == want_ok)
    ok("thenable_reject_storm_bad", bad == want_bad)
    ok("thenable_reject_storm_athrow", hits == n)
    ok("thenable_reject_storm_alive", alive == 42)


def destroy_from_callback() -> None:
    """destroy() inside a JS→Python callback must not hang the wire."""
    print("=== destroy_from_callback ===")
    rounds = 40 if FULL else 16
    t0 = time.perf_counter()
    got = 0
    closed = 0
    late = 0
    for _ in range(rounds):
        js = cc_node.create()
        caller = js.eval(
            "(f) => { try { return f(); } catch (e) { "
            "return 'err:' + e.message; } }"
        )

        def boom():
            js.destroy()
            return 42

        v = caller(boom)
        if v == 42 or (isinstance(v, str) and v.startswith("err:")):
            got += 1
        if js.closed:
            closed += 1
        try:
            js.eval("1+1")
        except Exception as e:
            if "closed" in str(e).lower() or "exit" in str(e).lower():
                late += 1
        # Idempotent
        js.destroy()
    result("destroy_from_callback_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("destroy_from_callback_rounds %d", rounds)
    ok("destroy_from_callback_got", got == rounds)
    ok("destroy_from_callback_closed", closed == rounds)
    ok("destroy_from_callback_late", late == rounds)


def ledger_churn() -> None:
    """Flood the handle table; deferred GC release must not desync the wire."""
    print("=== ledger_churn ===")
    js = cc_node.create()
    n = 600 if FULL else 250
    t0 = time.perf_counter()
    before = js.stats()
    acc = 0
    for i in range(n):
        # Dropping the only Python ref schedules deferred release.
        f = js.eval("(x) => x + 1")
        acc += f(i)
        if i % 11 == 0:
            gc.collect()
        if i % 40 == 0:
            # stats is a sync op that must still demux cleanly amid releases.
            _ = js.stats()
    gc.collect()
    # A few explicit releases + require churn.
    path = js.require("path")
    joined = path.join("a", "b", "c")
    js.release(path)
    after = js.stats()
    js.destroy()
    result("ledger_churn_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("ledger_churn_n %d", n)
    result("ledger_churn_stats_before %s", before)
    result("ledger_churn_stats_after %s", after)
    ok("ledger_churn_acc", acc == sum(i + 1 for i in range(n)))
    ok("ledger_churn_require", joined == "a/b/c")


def callback_buffer_path() -> None:
    """Typed arrays cross the nested cbr path (inline + spill)."""
    print("=== callback_buffer_path ===")
    js = cc_node.create()
    # JS → Python cb with a buffer; Python returns a transformed buffer.
    roundtrip = js.eval(
        "(cb, a) => { const b = cb(a); return b instanceof Float64Array "
        "? b.reduce((s, x) => s + x, 0) : -1; }"
    )
    small = array.array("d", [1.0, 2.0, 3.0, 4.0])
    # Spill-sized
    n_big = (1 << 16) // 8 + 8  # just over 64KiB of f64
    big = array.array("d", [1.0] * n_big)
    t0 = time.perf_counter()

    def double(a):
        # a may be numpy or array.array
        if hasattr(a, "tolist"):
            xs = a.tolist() if not hasattr(a, "__len__") else list(a)
        else:
            xs = list(a)
        return array.array("d", [x * 2 for x in xs])

    s_small = roundtrip(double, small)
    s_big = roundtrip(double, big)
    # Return buffer from Python cb that JS then maps.
    mapped = js.eval("(cb) => Array.from(cb()).reduce((s, x) => s + x, 0)")
    s_ret = mapped(lambda: array.array("d", [10.0, 20.0, 30.0]))
    js.destroy()
    result("callback_buffer_path_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("callback_buffer_path_small", s_small == 20.0)
    ok("callback_buffer_path_big", s_big == float(n_big * 2))
    ok("callback_buffer_path_ret", s_ret == 60.0)


def child_crash_storm() -> None:
    """Node child dies mid-call / mid-callback: host rejects, no hang."""
    print("=== child_crash_storm ===")
    import signal as _signal
    rounds = 20 if FULL else 10
    t0 = time.perf_counter()
    mid_call = 0
    mid_cb = 0
    late = 0
    for i in range(rounds):
        js = cc_node.create()
        if i % 2 == 0:
            boom = js.eval("() => process.exit(9)")
            try:
                boom()
            except Exception as e:
                if "exit" in str(e).lower() or "closed" in str(e).lower():
                    mid_call += 1
        else:
            # Kill the child while the broker is blocked on cbr.
            pid = js.eval("() => process.pid")()
            caller = js.eval("(f) => f(1)")

            def boom(_x):
                os.kill(int(pid), _signal.SIGKILL)
                return 1

            try:
                caller(boom)
            except Exception as e:
                if "exit" in str(e).lower() or "closed" in str(e).lower():
                    mid_cb += 1
        if js.closed:
            late += 1
        else:
            try:
                js.eval("1+1")
            except Exception as e:
                if "closed" in str(e).lower() or "exit" in str(e).lower():
                    late += 1
            try:
                js.destroy()
            except Exception:
                pass
    result("child_crash_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("child_crash_storm_rounds %d", rounds)
    ok("child_crash_storm_mid_call", mid_call == (rounds + 1) // 2)
    ok("child_crash_storm_mid_cb", mid_cb == rounds // 2)
    ok("child_crash_storm_late", late == rounds)


def thenable_typed_array() -> None:
    """Awaited thenables that resolve to typed arrays (inline + spill)."""
    print("=== thenable_typed_array ===")
    js = cc_node.create()
    small = js.eval("async () => new Float64Array([1, 2, 3, 4])")
    n_big = (1 << 16) // 8 + 16
    big = js.eval(
        "async () => { const a = new Float64Array(%d); a.fill(2); return a; }"
        % n_big
    )
    t0 = time.perf_counter()
    a = small()
    b = big()
    # Mix with a rejecting thenable so encode path stays demuxed.
    try:
        js.eval("async () => { throw new Error('ta-boom'); }")()
        rej_ok = False
    except cc_node.JsError as e:
        rej_ok = "ta-boom" in str(e)
    js.destroy()
    sa = list(a) if hasattr(a, "__iter__") else []
    sb = list(b) if hasattr(b, "__iter__") else []
    result("thenable_typed_array_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("thenable_typed_array_small", sa == [1.0, 2.0, 3.0, 4.0])
    ok("thenable_typed_array_big", len(sb) == n_big and sb[0] == 2.0)
    ok("thenable_typed_array_rej", rej_ok)


def import_module_storm() -> None:
    """ESM import_module door under ledger churn (vs require)."""
    print("=== import_module_storm ===")
    js = cc_node.create()
    n = 40 if FULL else 16
    t0 = time.perf_counter()
    # node: namespace is ESM-friendly on modern Node.
    fs = js.import_module("node:fs")
    path = js.import_module("node:path")
    joined = path.join("a", "b")
    # existsSync should be a bound method handle.
    exists = fs.existsSync
    here = exists(__file__) if callable(exists) else False
    # Churn: re-import + release + stats.
    ok_n = 0
    for i in range(n):
        m = js.import_module("node:path")
        if m.join("x", "y") == "x/y":
            ok_n += 1
        if i % 5 == 0:
            js.release(m)
        _ = js.stats()
    # Thenable namespace: fs.promises.readFile is async — await is free.
    promises = fs.promises
    read = promises.readFile
    # Use a tiny existing file.
    text = read(__file__, "utf8")
    js.destroy()
    result("import_module_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("import_module_storm_n %d", n)
    ok("import_module_storm_join", joined == "a/b")
    ok("import_module_storm_exists", here is True)
    ok("import_module_storm_churn", ok_n == n)
    ok("import_module_storm_read", isinstance(text, str) and "import_module" in text)


def cross_domain_barrage() -> None:
    """Many domains; cross-handle misuse stays articulate; no wire bleed."""
    print("=== cross_domain_barrage ===")
    n = 8 if FULL else 5
    kids = [cc_node.create() for _ in range(n)]
    ids = [make_id(js) for js in kids]
    t0 = time.perf_counter()
    cross_errs = 0
    # Each domain computes; then try every foreign handle on domain 0.
    sums = []
    for i, f in enumerate(ids):
        sums.append(sum(f(k) for k in range(20)))
    for i in range(1, n):
        try:
            kids[0].release(ids[i])
        except cc_node.JsError as e:
            if "another bridge" in str(e):
                cross_errs += 1
        try:
            # Foreign handle as call target via wrong domain's encode path:
            # release is the public door; also try stats isolation.
            kids[i].stats()
        except Exception:
            pass
    for js in kids:
        js.destroy()
    # After destroy, every door rejects.
    late = 0
    for f in ids:
        try:
            f(1)
        except Exception as e:
            if "closed" in str(e).lower():
                late += 1
    result("cross_domain_barrage_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("cross_domain_barrage_n %d", n)
    ok("cross_domain_barrage_sums", all(v == sum(range(20)) for v in sums))
    ok("cross_domain_barrage_errs", cross_errs == n - 1)
    ok("cross_domain_barrage_late", late == n)


def destroy_during_thenable() -> None:
    """Cross-thread destroy while another thread blocks on a slow thenable.

    Contract: no hang; bridge ends closed; the in-flight call either
    completes or rejects with closed/exit — never silent success after
    a hard teardown that killed the child mid-reply.
    """
    print("=== destroy_during_thenable ===")
    rounds = 30 if SOAK else (20 if FULL else 10)
    delay_ms = 300 if SOAK else 150
    t0 = time.perf_counter()
    settled = 0
    rejected = 0
    hung = 0
    for _ in range(rounds):
        js = cc_node.create()
        slow = js.eval(
            "async () => { await new Promise(r => setTimeout(r, %d)); "
            "return 99; }" % delay_ms
        )
        box = {"v": None, "e": None}

        def worker():
            try:
                box["v"] = slow()
            except Exception as e:
                box["e"] = e

        th = threading.Thread(target=worker)
        th.start()
        time.sleep(0.01)
        js.destroy()
        th.join(timeout=10)
        if th.is_alive():
            hung += 1
            continue
        if box["v"] == 99:
            settled += 1
        elif box["e"] is not None and (
            "closed" in str(box["e"]).lower()
            or "exit" in str(box["e"]).lower()
        ):
            rejected += 1
        if not js.closed:
            try:
                js.destroy()
            except Exception:
                pass
    result("destroy_during_thenable_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("destroy_during_thenable_settled %d", settled)
    result("destroy_during_thenable_rejected %d", rejected)
    ok("destroy_during_thenable_no_hang", hung == 0)
    ok("destroy_during_thenable_accounted",
       settled + rejected == rounds and hung == 0)


def mixed_thenable_hammer() -> None:
    """Overlap a slow thenable on domain A with sync traffic on domain B.

    Same-bridge multi-thread _req would race the wire; two domains are
    the supported shape (mirrors cc-python multi-domain overlap).
    """
    print("=== mixed_thenable_hammer ===")
    js_a = cc_node.create()
    js_b = cc_node.create()
    slow = js_a.eval(
        "async (n) => { await new Promise(r => setTimeout(r, n)); return n; }"
    )
    sync = js_b.eval("(x) => x * x")
    n_sync = 80 if SOAK else (40 if FULL else 20)
    t0 = time.perf_counter()
    box = {"v": None, "e": None}

    def worker():
        try:
            box["v"] = slow(80)
        except Exception as e:
            box["e"] = e

    th = threading.Thread(target=worker)
    th.start()
    acc = 0
    for i in range(n_sync):
        acc += sync(i)
    th.join(timeout=10)
    js_a.destroy()
    js_b.destroy()
    result("mixed_thenable_hammer_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("mixed_thenable_hammer_sync", acc == sum(i * i for i in range(n_sync)))
    ok("mixed_thenable_hammer_async",
       box["v"] == 80 and box["e"] is None and not th.is_alive())


def big_payload_hail() -> None:
    """Mirror cc-python big_payload: wide/deep plain data both directions."""
    print("=== big_payload_hail ===")
    js = cc_node.create()
    wide_n = 200 if SOAK else (120 if FULL else 60)
    wide = {"k%d" % i: i * i for i in range(wide_n)}
    deep = {"a": {"b": {"c": {"d": {"e": [1, 2, {"f": "g"}]}}}}}
    echo = js.eval("(o) => o")
    pick = js.eval("(o) => o.k7")
    dig = js.eval("(o) => o.a.b.c.d.e[2].f")
    rounds = 40 if SOAK else (20 if FULL else 10)
    t0 = time.perf_counter()
    ok_n = 0
    for _ in range(rounds):
        back = echo(wide)
        if pick(back) == 49 and dig(deep) == "g":
            ok_n += 1
    # Large plain list (no shm) — exercises JSON line size.
    n_list = 3000 if SOAK else (1500 if FULL else 500)
    lst = js.eval("(a) => a.length")(list(range(n_list)))
    js.destroy()
    result("big_payload_hail_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("big_payload_hail_rounds %d", rounds)
    ok("big_payload_hail_json", ok_n == rounds)
    ok("big_payload_hail_list", lst == n_list)


def retained_callback_storm() -> None:
    """Mirror retained callable: one Python cb kept alive across many JS calls."""
    print("=== retained_callback_storm ===")
    js = cc_node.create()
    apply = js.eval("(f, n) => { let s = 0; for (let i = 0; i < n; i++) s += f(i); return s; }")
    n = 500 if SOAK else (300 if FULL else 120)
    t0 = time.perf_counter()

    def retained(x):
        return x * 3

    total = apply(retained, n)
    # Same function object again — must not double-register / desync.
    total2 = apply(retained, n)
    js.destroy()
    want = sum(i * 3 for i in range(n))
    result("retained_callback_storm_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("retained_callback_storm_total", total == want and total2 == want)


def rss_soak() -> None:
    """Long create/churn/destroy loop; RSS must not climb without bound."""
    print("=== rss_soak ===")
    # Quick: short; full: medium; soak: multi-second (override via SOAK_SECONDS).
    if SOAK:
        seconds, ops_per = float(os.environ.get("SOAK_SECONDS", "8")), 40
        limit_mb = 512
    elif FULL:
        seconds, ops_per = 3.0, 25
        limit_mb = 384
    else:
        seconds, ops_per = 1.0, 15
        limit_mb = 256
    r0 = rss_bytes()
    t0 = time.perf_counter()
    cycles = 0
    acc = 0
    while time.perf_counter() - t0 < seconds:
        js = cc_node.create()
        f = js.eval("(x) => x + 1")
        for i in range(ops_per):
            acc += f(i)
            if i % 7 == 0:
                tmp = js.eval("(x) => x")
                acc += tmp(i)
        js.destroy()
        cycles += 1
        if cycles % 5 == 0:
            gc.collect()
    # Force a few more GC passes so deferred releases land.
    gc.collect()
    r1 = rss_bytes()
    delta = max(0, r1 - r0)
    result("rss_soak_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("rss_soak_cycles %d", cycles)
    result("rss_soak_rss0_mb %.1f", r0 / MB)
    result("rss_soak_rss1_mb %.1f", r1 / MB)
    result("rss_soak_delta_mb %.1f", delta / MB)
    ok("rss_soak_cycles", cycles >= (20 if SOAK else (8 if FULL else 3)))
    ok("rss_soak_acc", acc > 0)
    ok("rss_soak_rss", delta < limit_mb * MB)


def handle_leak_soak() -> None:
    """Long-lived domain: mint/drop handles; ledger + RSS must re-settle."""
    print("=== handle_leak_soak ===")
    if SOAK:
        seconds = float(os.environ.get("SOAK_SECONDS", "10"))
        burst = 80
        limit_mb = 384
    elif FULL:
        seconds, burst, limit_mb = 4.0, 50, 256
    else:
        seconds, burst, limit_mb = 1.2, 30, 192
    js = cc_node.create()
    base = js.stats()
    r0 = rss_bytes()
    t0 = time.perf_counter()
    bursts = 0
    acc = 0
    peak_stats = base
    while time.perf_counter() - t0 < seconds:
        keep = []
        for i in range(burst):
            f = js.eval("(x) => x + %d" % (i % 9))
            acc += f(i)
            if i % 3 == 0:
                keep.append(f)
            # else: drop — deferred release must drain
        peak_stats = max(peak_stats, js.stats())
        del keep
        gc.collect()
        bursts += 1
    gc.collect()
    time.sleep(0.05)
    gc.collect()
    after = js.stats()
    # Explicit release of anything still held, then destroy.
    js.destroy()
    r1 = rss_bytes()
    delta = max(0, r1 - r0)
    result("handle_leak_soak_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("handle_leak_soak_bursts %d", bursts)
    result("handle_leak_soak_peak_stats %s", peak_stats)
    result("handle_leak_soak_after_stats %s", after)
    result("handle_leak_soak_delta_mb %.1f", delta / MB)
    ok("handle_leak_soak_bursts", bursts >= (8 if SOAK else (3 if FULL else 1)))
    ok("handle_leak_soak_acc", acc > 0)
    # After GC, live handles should be near the kept-per-burst residue,
    # not grow without bound across bursts (peak >> after is the signal).
    ok("handle_leak_soak_ledger", peak_stats > after and after < burst)
    ok("handle_leak_soak_rss", delta < limit_mb * MB)


def everything_concurrent() -> None:
    """Mixed kitchen sink: several storm shapes on separate domains at once."""
    print("=== everything_concurrent ===")
    n_dom = 4 if FULL else 3
    t0 = time.perf_counter()
    errs = []
    boxes = [{"acc": 0, "ok": False} for _ in range(n_dom)]

    def worker(i):
        try:
            js = cc_node.create()
            if i % 4 == 0:
                f = js.eval("(x) => x * 2")
                s = sum(f(k) for k in range(40 if FULL else 20))
                boxes[i]["acc"] = s
                boxes[i]["ok"] = s == sum(k * 2 for k in range(40 if FULL else 20))
            elif i % 4 == 1:
                boom = js.eval(
                    "(cb, n) => { let s = 0; for (let j = 0; j < n; j++) "
                    "s += cb((x) => x + j)(1); return s; }"
                )
                s = boom(lambda g: g, 30 if FULL else 12)
                boxes[i]["acc"] = s
                boxes[i]["ok"] = s == sum(1 + j for j in range(30 if FULL else 12))
            elif i % 4 == 2:
                af = js.eval(
                    "async (n) => { let s = 0; for (let j = 0; j < n; j++) "
                    "s += await Promise.resolve(j); return s; }"
                )
                s = af(25 if FULL else 10)
                boxes[i]["acc"] = s
                boxes[i]["ok"] = s == sum(range(25 if FULL else 10))
            else:
                ln = js.eval("(a) => a.length")
                buf = array.array("d", [1.0] * ((1 << 16) // 8 + 8))
                s = ln(buf)
                boxes[i]["acc"] = s
                boxes[i]["ok"] = s == len(buf)
            js.destroy()
        except Exception as e:
            errs.append(e)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_dom)]
    for th in threads:
        th.start()
    for th in threads:
        th.join(timeout=60)
    hung = any(th.is_alive() for th in threads)
    result("everything_concurrent_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("everything_concurrent_n %d", n_dom)
    ok("everything_concurrent_no_hang", not hung)
    ok("everything_concurrent_no_err", not errs)
    ok("everything_concurrent_ok", all(b["ok"] for b in boxes))


def escaped_closure() -> None:
    """Capture a JS callable, destroy the domain, invoke later → closed."""
    print("=== escaped_closure ===")
    t0 = time.perf_counter()
    closed = 0
    escaped = None
    js = cc_node.create()
    fn = js.eval("(x) => x + 1")
    escaped = fn
    js.destroy()
    gc.collect()
    try:
        escaped(1)
    except Exception as e:
        if "closed" in str(e).lower() or "exit" in str(e).lower():
            closed = 1
    result("escaped_closure_ms %.1f", (time.perf_counter() - t0) * 1000)
    ok("escaped_closure_closed", closed == 1)


def fanout_destroy() -> None:
    """Many children in flight; destroy a subset mid-flight.

    Cooperative destroy may let in-flight work fulfill (drain). Pin
    composition: no hang, every worker settles, doomed domains closed,
    survivors still usable. Hard-cancel is abort_inject / child_crash.
    """
    print("=== fanout_destroy ===")
    n = 8 if FULL else 6
    t0 = time.perf_counter()
    domains = [cc_node.create() for _ in range(n)]
    delay_ms = 80 if FULL else 50
    sleeper = [
        d.eval(
            "(n) => new Promise((r) => setTimeout(() => r(n), n))"
        )
        for d in domains
    ]
    boxes = [{"v": None, "err": None} for _ in range(n)]

    def worker(i):
        try:
            boxes[i]["v"] = sleeper[i](delay_ms)
        except Exception as e:
            boxes[i]["err"] = e

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(n)]
    for th in threads:
        th.start()
    time.sleep(0.01)
    doomed = list(range(1, n, 2))
    for i in doomed:
        try:
            domains[i].destroy()
        except Exception:
            pass
    for th in threads:
        th.join(timeout=30)
    hung = any(th.is_alive() for th in threads)
    fulfilled = sum(1 for b in boxes if b["v"] == delay_ms)
    rejected = sum(
        1
        for b in boxes
        if b["err"] is not None
        and ("closed" in str(b["err"]).lower() or "exit" in str(b["err"]).lower())
    )
    doomed_closed = sum(1 for i in doomed if domains[i].closed)
    survivors_ok = 0
    for i in range(0, n, 2):
        try:
            if domains[i].eval("1+1") == 2:
                survivors_ok += 1
            domains[i].destroy()
        except Exception:
            pass
    result("fanout_destroy_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("fanout_destroy_fulfilled %d", fulfilled)
    result("fanout_destroy_rejected %d", rejected)
    ok("fanout_destroy_no_hang", not hung)
    ok("fanout_destroy_accounted", fulfilled + rejected == n)
    ok("fanout_destroy_doomed_closed", doomed_closed == len(doomed))
    ok("fanout_destroy_survivors", survivors_ok == (n + 1) // 2)


def abort_inject() -> None:
    """SIGABRT the Node child mid-flight; host rejects, no hang, no zombies."""
    print("=== abort_inject ===")
    import signal as _signal
    rounds = 16 if SOAK else (10 if FULL else 6)
    t0 = time.perf_counter()
    rejected = 0
    late = 0
    for i in range(rounds):
        js = cc_node.create()
        pid = js.eval("() => process.pid")()
        if i % 2 == 0:
            # Abort during a plain call (timer fires abort from inside Node).
            boom = js.eval(
                "() => { setTimeout(() => process.abort(), 5); "
                "return new Promise(() => {}); }"
            )
            try:
                boom()
            except Exception as e:
                if "exit" in str(e).lower() or "closed" in str(e).lower():
                    rejected += 1
        else:
            # Abort while broker waits on cbr.
            caller = js.eval("(f) => f(1)")

            def boom(_x):
                os.kill(int(pid), _signal.SIGABRT)
                return 1

            try:
                caller(boom)
            except Exception as e:
                if "exit" in str(e).lower() or "closed" in str(e).lower():
                    rejected += 1
        if js.closed:
            late += 1
        else:
            try:
                js.eval("1")
            except Exception as e:
                if "closed" in str(e).lower() or "exit" in str(e).lower():
                    late += 1
            try:
                js.destroy()
            except Exception:
                pass
    result("abort_inject_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("abort_inject_rounds %d", rounds)
    ok("abort_inject_rejected", rejected == rounds)
    ok("abort_inject_late", late == rounds)


def main() -> int:
    print("cc_node_stress_wire scale=%s" % SCALE)
    multi_child_fanout()
    callback_blizzard()
    handle_boomerang()
    exception_hail()
    thenable_storm()
    thenable_reject_storm()
    destroy_from_callback()
    destroy_during_thenable()
    mixed_thenable_hammer()
    callback_buffer_path()
    child_crash_storm()
    thenable_typed_array()
    import_module_storm()
    big_payload_hail()
    retained_callback_storm()
    ledger_churn()
    shm_hail()
    teardown_derby()
    eval_storm()
    cross_domain_barrage()
    everything_concurrent()
    escaped_closure()
    fanout_destroy()
    rss_soak()
    handle_leak_soak()
    # Abort last: process.abort / SIGABRT is intentionally noisy.
    abort_inject()
    print("=== summary ===")
    result("stress_fails %d", fails)
    ok("stress_clean", fails == 0)
    if fails:
        print("STRESS FAILED: %d checks" % fails, file=sys.stderr)
        return 1
    print("stress_wire done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
