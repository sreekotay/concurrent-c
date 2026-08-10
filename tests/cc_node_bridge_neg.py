"""cc-node negative / articulate-error pack.

Misuse must fail loudly with a stable message shape; the bridge stays
usable afterward (except after_close). Deterministic booleans for the
paired smoke.
"""
import math
import sys

sys.path.insert(0, "pypi/cc-node")
import cc_node  # noqa: E402


def out(name, cond):
    print(name, "true" if cond else "false")


def raises(fn, *needles):
    try:
        fn()
        return False
    except Exception as e:
        msg = str(e)
        return all(n in msg for n in needles)


js = cc_node.create()
ident = js.eval("(x) => x")

# Reserved wire tags / unsupported types.
out("neg_dollar_key", raises(lambda: ident({"$x": 1}),
                             "reserved", "$"))
out("neg_unsupported_set", raises(lambda: ident({1, 2}),
                                  "unsupported argument"))

# Non-finite scalars are tagged, not rejected (nested in a plain object
# they make the object a handle — scalar door is the documented path).
out("neg_nonfinite_ok",
    math.isnan(ident(math.nan))
    and ident(math.inf) == math.inf
    and ident(-math.inf) == -math.inf)

# Foreign handle as call *argument* (not only release).
other = cc_node.create()
foreign = other.eval("(x) => x")
out("neg_foreign_arg", raises(lambda: ident(foreign), "another bridge"))
other.destroy()

# Use-after-release.
tmp = js.require("os")
js.release(tmp)
out("neg_use_after_release", raises(lambda: str(tmp),
                                    "released"))

# Non-callable handle.
date = js.eval("new Date(0)")
out("neg_call_noncallable", raises(lambda: date(), "not callable"))

# Missing package (already in main suite; pin here as the neg pack's door).
out("neg_missing_package",
    raises(lambda: js.require("definitely_not_a_package_neg_xyz"),
           "Cannot find", "node_modules"))

# Empty {} stays a live handle (not a Python dict) — bag / namespace door.
empty = js.eval("({})")
out("neg_empty_object_handle", isinstance(empty, cc_node.JsHandle))
bag = js.eval("() => ({})")()
out("neg_empty_factory_handle", isinstance(bag, cc_node.JsHandle))
# Non-empty plain objects still cross by value (data returns).
out("neg_nonempty_object_value",
    js.eval("({a: 1})") == {"a": 1})

# After destroy, every door answers closed; bridge stays closed.
js.destroy()
js.destroy()
out("neg_after_close_eval", raises(lambda: js.eval("1+1"), "closed"))
out("neg_after_close_stats", raises(lambda: js.stats(), "closed"))
out("neg_destroy_idempotent", js.closed)

# A fresh domain still works after the previous one was abused.
with cc_node.create() as ok_js:
    out("neg_sibling_alive", ok_js.eval("1+1") == 2)

print("cc-node neg suite done")
