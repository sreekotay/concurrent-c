"""cc-node rungs: Python using JavaScript — builtin Node packages stand
in for npm ones (same resolution path, no install step in CI).
Deterministic booleans; the paired smoke pins them."""
import hashlib
import os
import signal
import sys
import threading
import time

sys.path.insert(0, "pypi/cc-node")
import cc_node  # noqa: E402


def out(name, cond):
    print(name, "true" if cond else "false")


js = cc_node.create()

# 1. Packages: require resolves, chained calls, string results.
path = js.require("path")
out("require_builtin", path.join("a", "b") == "a/b")
crypto = js.require("crypto")
h = crypto.createHash("sha256")
h2 = h.update("cc")  # returns the hash object: handle chaining
digest = h2.digest("hex")
out("handle_chain", digest == hashlib.sha256(b"cc").hexdigest())

# 2. Plain data crosses by value, both directions.
out("array_out", js.eval("[1, 2, 3].map(x => x * 2)") == [2, 4, 6])
out("dict_out", js.eval("({a: 1, b: [true, null, 's']})") ==
    {"a": 1, "b": [True, None, "s"]})
pick = js.eval("(o) => o.a + o.b.length")
out("dict_in", pick({"a": 2, "b": [1, 2, 3]}) == 5)
out("nonfinite_out", str(js.eval("0/0")) == "nan")

# 3. Async JS is free: the broker awaits thenables before replying.
adouble = js.eval("async (x) => { await new Promise(r => setTimeout(r, 20)); return x * 2 }")
out("async_awaited", adouble(21) == 42)

# 4. Python callables cross as JS functions; exceptions cross back.
# JS calling conventions apply: Array.map calls f(value, index, array).
mapper = js.eval("(f, xs) => xs.map(f)")
out("python_callback", mapper(lambda x, *rest: x * 3, [1, 2, 3]) == [3, 6, 9])
# Returning a JS handle from a Python callback must not trip the wire
# (GC release used to nest _req and steal the outer reply).
nest = js.eval("(cb) => cb((x) => x + 1)")
out("callback_return_handle", nest(lambda g: g)(40) == 41)
nest2 = js.eval("(cb) => cb((x) => cb((y) => x + y)(3))(10)")
out("callback_return_handle_nested", nest2(lambda f: f) == 13)
# Thenable reject stays articulate; destroy-from-callback must not hang.
rej = js.eval("() => Promise.reject(new Error('nope-rej'))")
try:
    rej()
    out("thenable_reject", False)
except cc_node.JsError as e:
    out("thenable_reject", "nope-rej" in str(e))
js2 = cc_node.create()
caller = js2.eval("(f) => f()")
out("destroy_from_callback", caller(lambda: (js2.destroy(), 7)[1]) == 7 and js2.closed)
ta = js.eval("async () => new Float64Array([1, 2, 3])")
out("thenable_typed_array", list(ta()) == [1.0, 2.0, 3.0])
fs = js.import_module("node:path")
out("import_module", fs.join("a", "b") == "a/b")
# Cross-thread destroy during a slow thenable: no hang; settle or reject.
js3 = cc_node.create()
slow = js3.eval(
    "async () => { await new Promise(r => setTimeout(r, 120)); return 99 }")
_box = {"v": None, "e": None}

def _worker():
    try:
        _box["v"] = slow()
    except Exception as e:
        _box["e"] = e

_th = threading.Thread(target=_worker)
_th.start()
time.sleep(0.01)
js3.destroy()
_th.join(timeout=5)
out("destroy_during_thenable",
    (not _th.is_alive()) and js3.closed and (
        _box["v"] == 99 or (
            _box["e"] is not None and (
                "closed" in str(_box["e"]).lower()
                or "exit" in str(_box["e"]).lower()))))
thrower = js.eval("(f) => { try { f(); return 'no'; } catch (e) { return 'js saw: ' + e.message; } }")
def boom():
    raise ValueError("from python")
out("py_exc_to_js", thrower(boom) == "js saw: from python")
caller = js.eval("(f) => f()")
try:
    caller(boom)
    out("py_exc_roundtrip", False)
except cc_node.JsError as e:
    out("py_exc_roundtrip", "from python" in str(e))

# 5. Errors are articulate: missing packages, non-callable handles.
try:
    js.require("definitely_not_a_package_xyz")
    out("missing_package", False)
except cc_node.JsError as e:
    out("missing_package",
        "Cannot find" in str(e) and "node_modules" in str(e))
# Empty {} stays a handle; non-empty plain objects stay values.
out("empty_object_handle", isinstance(js.eval("({})"), cc_node.JsHandle))
out("nonempty_object_value",
    js.eval("({a: 1, b: 2})") == {"a": 1, "b": 2})

# 6. The ledger: stats counts handles; release drops them.
before = js.stats()
tmp = js.require("os")
grew = js.stats() == before + 1
js.release(tmp)
out("stats_ledger", grew and js.stats() == before)

# 7. Isolation: a second domain rejects the first one's handles.
other = cc_node.create()
try:
    other.release(path)
    out("cross_domain", False)
except cc_node.JsError as e:
    out("cross_domain", "another bridge" in str(e))
other.destroy()

# 8. Teardown: destroy is idempotent, every door answers after.
js.destroy()
js.destroy()
out("destroy_idempotent", js.closed)
try:
    path.join("x")
    out("after_close", False)
except cc_node.JsError as e:
    out("after_close", "closed" in str(e))

# 9. Context manager scoping.
with cc_node.create() as scoped:
    out("with_scope", scoped.require("os").platform() == sys.platform.replace("linux2", "linux"))
out("with_closes", scoped.closed)

# 10. Typed buffers cross as typed arrays — small inline, big through
#     the shared-memory spill — both directions, and no files stray.
import array
with cc_node.create() as js2:
    f = js2.eval("(a) => a.reduce((s, x) => s + x, 0)")
    small = array.array("d", [1.5, 2.5, 3.0])
    out("buffer_small_inline", abs(f(small) - 7.0) < 1e-9)
    big = array.array("d", [float(i % 89) for i in range(1 << 18)])  # 2MB
    out("buffer_big_shm", abs(f(big) - sum(big)) < 1e-6)
    back = js2.eval("(n) => new Float64Array(n).fill(0.5)")(1 << 18)
    out("buffer_back_shm", len(back) == (1 << 18) and float(back[0]) == 0.5)
    bys = js2.eval("() => new Uint8Array([104, 105])")()
    out("buffer_u8", list(bys) == [104, 105])
    # Spills live in a private per-bridge dir now; strays are leftover
    # FILES inside our bridge dirs (the dirs themselves live until
    # destroy) or flat files under the old naming.
    if os.path.isdir("/dev/shm"):
        stray = []
        for d in os.listdir("/dev/shm"):
            if not d.startswith("ccnode-%d-" % os.getpid()):
                continue
            p = os.path.join("/dev/shm", d)
            stray += os.listdir(p) if os.path.isdir(p) else [d]
        out("buffer_no_strays", len(stray) == 0)
    else:
        out("buffer_no_strays", True)

# 11. Notebook path: eval() does not install require (no extra RTT);
#     eval_cell does, once; bindings are Object.assign; repr is cheap.
with cc_node.create() as js:
    out("eval_no_require", js.eval("typeof require") == "undefined")
    out("eval_cell_require", js.eval_cell("typeof require") == "function")
    out("eval_cell_path",
        js.eval_cell("require('path').join('a', 'b')") == "a/b")
    out("eval_cell_bind", js.eval_cell("x * 2", {"x": 21}) == 42)
    out("eval_cell_persist",
        js.eval_cell("var __p = 41") is None and js.eval_cell("__p + 1") == 42)
    try:
        js.eval_cell("1", {"require": 1})
        out("eval_cell_bind_reserved", False)
    except cc_node.JsError as e:
        out("eval_cell_bind_reserved", "reserved" in str(e))
    date = js.eval("new Date(0)")
    out("repr_cheap",
        "JsHandle #" in repr(date) and "1970" not in repr(date))
    out("repr_html_cheap",
        "JsHandle #" in date._repr_html_() and "1970" not in date._repr_html_())

out("get_is_kernel", cc_node.get is cc_node.kernel)
out("mod_require", cc_node.require("path").join("a", "b") == "a/b")
a = cc_node.get()
b = cc_node.get()
out("get_same", a is b)
out("kernel_same", cc_node.kernel() is a)
cc_node.reset()
c = cc_node.get()
out("get_reset", a.closed and c is not a and not c.closed)
cc_node.reset()

# First Ctrl-C does not abandon the in-flight reply (wire would desync).
# Second door is a hard child kill — cooperative close cannot stop a
# JS CPU loop.
if sys.platform == "win32":
    out("interrupt_first", True)
    out("interrupt_kill", True)
else:
    with cc_node.create() as js:
        slow = js.eval(
            "() => new Promise(r => setTimeout(() => r(7), 250))")

        def _sig1():
            time.sleep(0.05)
            os.kill(os.getpid(), signal.SIGINT)

        th = threading.Thread(target=_sig1)
        th.start()
        raised = False
        try:
            slow()
        except KeyboardInterrupt:
            raised = True
        th.join(timeout=2)
        out("interrupt_first",
            raised and (not th.is_alive()) and js.eval("1+1") == 2)

    js = cc_node.create()
    busy = js.eval("() => { for (;;) {} }")

    def _kill():
        time.sleep(0.08)
        js._kill_child()

    th = threading.Thread(target=_kill)
    th.start()
    killed = False
    try:
        busy()
    except cc_node.JsError as e:
        msg = str(e).lower()
        killed = "exit" in msg or "closed" in msg or "destroyed" in msg
    except KeyboardInterrupt:
        killed = js.closed
    th.join(timeout=2)
    if not js.closed:
        try:
            js.destroy()
        except Exception:
            pass
    out("interrupt_kill", killed and js.closed)

print("cc-node suite done")
