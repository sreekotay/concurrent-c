"""cc-node in one sitting: builtin Node modules (no npm install needed),
chains, callbacks both ways, and async-for-free.

    python -m cc_node.examples.use_node
"""
import cc_node

with cc_node.create() as js:
    # Builtin modules: attribute access is property lookup, calls are calls.
    path = js.require("path")
    print("join      :", path.join("a", "b", "c.txt"))

    crypto = js.require("crypto")
    print("sha256    :", crypto.createHash("sha256")
                               .update("cc-node").digest("hex")[:16], "...")

    # eval for a quick lambda; a Python callable crosses as a JS function
    # (JS conventions apply: Array.map passes value, index, array).
    mapped = js.eval("(f) => [1, 2, 3].map(f)")(lambda x, *rest: x * 10)
    print("callback  :", mapped)

    # Async is free: thenables are awaited in the child before the reply.
    fetchish = js.eval("async (x) => { return { doubled: x * 2 } }")
    print("thenable  :", fetchish(21))

    # Typed buffers cross as typed arrays (big ones via shared memory);
    # results come back as numpy arrays when numpy is installed.
    total = js.eval("(a) => a.reduce((s, x) => s + x, 0)")
    import array
    print("buffer sum:", total(array.array("d", [1.5, 2.5, 3.0])))

    print("handles   :", js.stats())
