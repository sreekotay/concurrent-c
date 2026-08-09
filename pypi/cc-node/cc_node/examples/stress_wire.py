"""Adversarial multi-domain storm for concurrent-c-node.

    python -m cc_node.examples.stress_wire
    CC_NODE_STRESS=full python -m cc_node.examples.stress_wire

Modes: multi-child fanout, callback blizzard, concurrent shm hail,
destroy-during-inflight derby, late-use-after-close.  RESULT lines +
OK booleans; non-zero exit on any FAIL.
"""
from __future__ import annotations

import array
import os
import sys
import threading
import time

import cc_node

SCALE = os.environ.get("CC_NODE_STRESS", "quick").lower()
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
    # Nested: JS calls Python which calls JS which calls Python.
    nest = js.eval("(cb) => cb((x) => cb((y) => x + y)(3))(10)")
    nested = nest(lambda f: f)
    js.destroy()
    result("callback_blizzard_ms %.1f", (time.perf_counter() - t0) * 1000)
    result("callback_blizzard_calls %d", n)
    ok("callback_blizzard_total", total == n * sum(i * 2 for i in range(32)))
    ok("callback_blizzard_nested", nested == 13)


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


def main() -> int:
    print("cc_node_stress_wire scale=%s" % SCALE)
    multi_child_fanout()
    callback_blizzard()
    shm_hail()
    teardown_derby()
    eval_storm()
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
