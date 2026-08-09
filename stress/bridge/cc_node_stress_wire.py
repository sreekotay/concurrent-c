"""Adversarial multi-domain storm for concurrent-c-node.

    ./stress/bridge/run.sh
    PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py
    CHAOS_SCALE=full PYTHONPATH=pypi/cc-node python3 stress/bridge/cc_node_stress_wire.py

Latency demos stay under pypi/cc-node/cc_node/examples/.  This file is stress only.

Modes: multi-child fanout, callback blizzard, handle boomerang,
exception hail, thenable storm (+ reject), destroy-from-callback,
ledger churn, shm hail, teardown derby, eval storm, cross-domain
barrage.  RESULT lines + OK booleans; non-zero exit on any FAIL.
"""
from __future__ import annotations

import array
import gc
import os
import sys
import threading
import time

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
sys.path.insert(0, os.path.join(_ROOT, "pypi", "cc-node"))
import cc_node  # noqa: E402

# CHAOS_SCALE preferred; CC_NODE_STRESS kept as alias.
SCALE = (os.environ.get("CHAOS_SCALE")
         or os.environ.get("CC_NODE_STRESS")
         or "quick").lower()
FULL = SCALE == "full"
fails = 0


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


def main() -> int:
    print("cc_node_stress_wire scale=%s" % SCALE)
    multi_child_fanout()
    callback_blizzard()
    handle_boomerang()
    exception_hail()
    thenable_storm()
    thenable_reject_storm()
    destroy_from_callback()
    ledger_churn()
    shm_hail()
    teardown_derby()
    eval_storm()
    cross_domain_barrage()
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
