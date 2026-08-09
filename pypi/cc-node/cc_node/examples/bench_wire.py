"""The cc-node wire, measured.  RESULT lines are machine-comparable.

    python -m cc_node.examples.bench_wire
"""
import array
import time

import cc_node

t0 = time.perf_counter()
js = cc_node.create()
js.eval("1")
print("RESULT spawn_ms %d" % round((time.perf_counter() - t0) * 1000))

f = js.eval("(x) => x")
f(1)
t0 = time.perf_counter()
for i in range(500):
    f(i)
print("RESULT rtt_us %d" % round((time.perf_counter() - t0) / 500 * 1e6))

g = js.eval("(cb) => cb(21) * 2")
g(lambda x: x + 1)
t0 = time.perf_counter()
for _ in range(200):
    g(lambda x: x + 1)
print("RESULT callback_roundtrip_us %d"
      % round((time.perf_counter() - t0) / 200 * 1e6))

big = array.array("d", [float(i % 97) for i in range(1 << 20)])  # 8MB
ln = js.eval("(a) => a.length")
ln(big)
t0 = time.perf_counter()
for _ in range(10):
    ln(big)
print("RESULT bulk_8mb_shm_ms %.1f" % ((time.perf_counter() - t0) / 10 * 1000))

biglist = [float(i % 97) for i in range(1 << 20)]
ln(biglist)
t0 = time.perf_counter()
for _ in range(3):
    ln(biglist)
print("RESULT bulk_8mb_json_list_ms %d"
      % round((time.perf_counter() - t0) / 3 * 1000))

js.destroy()
print("done")
