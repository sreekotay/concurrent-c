"""N node children on N cores — measured with real overlap.

    python -m cc_node.benchmarks.multi_domain

Wire RTT / shm live in ``cc_node.examples.bench_wire``. This file only
answers the multi-domain question: sequential calls on one child vs the
same work fanned across threads into separate children.
"""
import time
from concurrent.futures import ThreadPoolExecutor

import cc_node

WORK = """
(iterations) => {
  let s = 0;
  for (let i = 0; i < iterations; i++)
    s += Math.sqrt(i) * Math.sin(i / 1000);
  return s;
}
"""


def main():
    n_dom = 3
    iters = 40_000_000

    with cc_node.create() as js:
        f = js.eval(WORK)
        f(iters // 20)
        t0 = time.perf_counter()
        for _ in range(n_dom):
            f(iters)
        seq = time.perf_counter() - t0
    print("RESULT seq_s %.3f" % seq)

    domains = [cc_node.create() for _ in range(n_dom)]
    try:
        fns = [d.eval(WORK) for d in domains]
        for f in fns:
            f(iters // 20)

        def run(f):
            return f(iters)

        t0 = time.perf_counter()
        with ThreadPoolExecutor(max_workers=n_dom) as pool:
            list(pool.map(run, fns))
        par = time.perf_counter() - t0
    finally:
        for d in domains:
            d.destroy()

    print("RESULT par_s %.3f" % par)
    print("RESULT speedup %.2f" % (seq / par if par else 0))
    print("done")


if __name__ == "__main__":
    main()
