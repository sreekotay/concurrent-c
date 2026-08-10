"""Wire-integrity rungs for the pypi/cc-node isolated wire, mirroring
tests/bridge_wire.js: the protocol lives on dedicated fds, so JS
writes to fd 1 reach the real stdout (the forged lines below appear in
the pinned output as ordinary prints), and replies pair by request id,
so output shaped exactly like a bridge reply can never be consumed as
one.  The rungs write with fs.writeSync — synchronous, so the lines
land before the reply and the interleaving pins exactly (console.log
routes to the same fd but flushes asynchronously to a pipe)."""
import sys

sys.path.insert(0, "pypi/cc-node")
import cc_node  # noqa: E402


def out(name, cond):
    print(name, "true" if cond else "false", flush=True)


js = cc_node.create()
fs = js.require("fs")
n = fs.writeSync(1, '{"v":777,"id":1}\n')
out("js_forge_ignored", isinstance(n, (int, float)) and n > 0)
n2 = fs.writeSync(1, "js-print-visible\n")
out("js_print_visible", isinstance(n2, (int, float)) and n2 > 0)
out("js_pairing_order",
    [js.eval("2*2"), js.eval("3*3"), js.eval("4*4")] == [4, 9, 16])
err = False
try:
    js.eval("(() => { throw new Error('boom') })()")
except cc_node.JsError as e:
    err = "boom" in str(e)
out("js_error_paired", err)
js.destroy()

# Spill hardening: each bridge owns a private 0700 dir; a >64KB buffer
# round-trips through it, and the dir goes with the bridge.
import array
import os
import tempfile

base = "/dev/shm" if os.path.isdir("/dev/shm") else tempfile.gettempdir()


def my_shm_dirs():
    return [d for d in os.listdir(base)
            if d.startswith("ccnode-%d-" % os.getpid())]


js2 = cc_node.create()
big = array.array("d", [0.5]) * (1 << 15)
total = js2.eval("(a) => a.reduce((s, x) => s + x, 0)")(big)
ds = my_shm_dirs()
mode_ok = (len(ds) == 1 and
           (os.stat(os.path.join(base, ds[0])).st_mode & 0o777) == 0o700)
out("js_shm_private_dir", mode_ok)
out("js_shm_roundtrip", abs(total - 0.5 * (1 << 15)) < 1e-6)
js2.destroy()
out("js_shm_dir_swept", len(my_shm_dirs()) == 0)
