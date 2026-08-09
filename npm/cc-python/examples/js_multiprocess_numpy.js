/* N x numpy: isolated domains are FULL CPython processes, so numpy
 * loads in every one and N domains are N GILs on N cores.
 *
 *   OPENBLAS_NUM_THREADS=1 node npm/cc-python/examples/js_multiprocess_numpy.js
 *
 * (Pin BLAS for honest scaling numbers — each call otherwise fans out
 * into its own thread pool and the pools fight for the same cores.)
 * The work is numpy sort+dot rounds: compute-bound, no JS in the loop.
 * RESULT lines are machine-comparable. */
'use strict';

const ccpy = require(__dirname + '/..'); // the package (works in-repo and installed)

const ROUNDS = 6;
const N = 1 << 19;

async function makeDomain() {
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  // One callable, held in the child: rounds of sort+dot on its own data.
  const work = await b.eval(
      'lambda r, n: (lambda np: [float(np.dot(np.sort(np.random.rand(n)), ' +
      'np.random.rand(n))) for _ in range(r)][-1])(__import__("numpy"))');
  return { py, work };
}

(async () => {
  const t0 = Date.now();
  const one = await makeDomain();
  await one.work(1, 1024); // proves numpy imported + warm
  console.log('RESULT spawn_plus_numpy_ms %d', Date.now() - t0);

  // Wire round trip: the smallest possible call, timed.
  {
    const b = one.py.import('builtins');
    const idf = await b.eval('lambda x: x');
    await idf(1);
    const iters = 200;
    const t1 = process.hrtime.bigint();
    for (let i = 0; i < iters; i++) await idf(i);
    const ns = Number(process.hrtime.bigint() - t1);
    console.log('RESULT wire_rtt_us %d', Math.round(ns / iters / 1000));
  }

  // Bulk crossing: an 8MB Float64Array argument, through the shm spill
  // (one memcpy per side; the base64 wire measured 153ms for this).
  {
    const np1 = one.py.import('numpy');
    const big = new Float64Array(1 << 20).map((_, i) => i % 97);
    await np1.sum(big);
    const t1 = process.hrtime.bigint();
    for (let i = 0; i < 10; i++) await np1.sum(big);
    const ms = Number(process.hrtime.bigint() - t1) / 10 / 1e6;
    console.log('RESULT bulk_8mb_arg_ms %s', ms.toFixed(1));
  }

  // Scaling: the SAME total work split across 1, 2, 4 domains.
  const domains = [one];
  while (domains.length < 4) domains.push(await makeDomain());
  for (const d of domains) await d.work(1, N); // all warm
  const total = ROUNDS * 4;
  let base = 0;
  for (const nd of [1, 2, 4]) {
    const per = total / nd;
    const t1 = Date.now();
    await Promise.all(domains.slice(0, nd).map((d) => d.work(per, N)));
    const ms = Date.now() - t1;
    if (nd === 1) base = ms;
    console.log('RESULT domains_%d ms %d speedup x %s', nd, ms,
                (base / ms).toFixed(2));
  }

  await Promise.all(domains.map((d) => d.py.destroy()));
  console.log('done');
})().catch((e) => { console.error('ERR', e.message); process.exit(1); });
