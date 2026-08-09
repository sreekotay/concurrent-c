"""cc-node rungs: Python using JavaScript — builtin Node packages stand
in for npm ones (same resolution path, no install step in CI).
Deterministic booleans; the paired smoke pins them."""
import hashlib
import sys

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
    out("missing_package", "Cannot find" in str(e))

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
import os
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
    if os.path.isdir("/dev/shm"):
        stray = [x for x in os.listdir("/dev/shm")
                 if x.startswith("ccnode-%d-" % os.getpid())]
        out("buffer_no_strays", len(stray) == 0)
    else:
        out("buffer_no_strays", True)

print("cc-node suite done")
