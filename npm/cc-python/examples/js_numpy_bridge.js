/* The magic demo, pure: plain node + require('cc-python') + numpy.
 * No bespoke module, no build step — the generic bridge, timed.
 *
 *   node npm/cc-python/examples/js_numpy_bridge.js
 *
 * Rungs: bulk dot/sum/std on 1M doubles (zero-copy leases) against the
 * same work in a plain JS loop; hot 16-element dot for per-call
 * overhead, sync and task-lane; a pipelined lane burst.  RESULT lines
 * are machine-comparable; epsilon cross-checks keep the numbers honest. */
'use strict';

const ccpy = require(__dirname + '/..'); // the package (works in-repo and installed)

function time(label, iters, fn) {
  fn(); // warm
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iters; i++) fn();
  const ns = Number(process.hrtime.bigint() - t0);
  const ms = ns / 1e6;
  console.log('RESULT %s ms %s ns/call %s', label, ms.toFixed(3),
              Math.round(ns / iters));
  return ns / iters;
}

async function timeAsync(label, iters, fn) {
  await fn();
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iters; i++) await fn();
  const ns = Number(process.hrtime.bigint() - t0);
  console.log('RESULT %s ms %s ns/call %s', label, (ns / 1e6).toFixed(3),
              Math.round(ns / iters));
  return ns / iters;
}

(async () => {
  const py = ccpy.create();
  const np = py.import('numpy');
  console.log('numpy', String(np.__version__));

  const N = 1 << 20;
  const a = new Float64Array(N);
  const b = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    a[i] = (i % 97) * 0.25;
    b[i] = (i % 89) * 0.5;
  }

  // Held callables: one getattr each, then it's invoke per call.
  const dot = np.dot;
  const npsum = np.sum;
  const npstd = np.std;

  // Cross-check against a JS loop before timing anything.
  let jsDot = 0;
  for (let i = 0; i < N; i++) jsDot += a[i] * b[i];
  const bridgeDot = dot(a, b);
  if (Math.abs(bridgeDot - jsDot) > Math.abs(jsDot) * 1e-9)
    throw new Error('dot mismatch: ' + bridgeDot + ' vs ' + jsDot);

  // Bulk: 1M-element dot, zero-copy leases vs the JS loop.
  const perDot = time('bridge_dot_1m', 50, () => dot(a, b));
  const perJs = time('js_loop_dot_1m', 50, () => {
    let s = 0;
    for (let i = 0; i < N; i++) s += a[i] * b[i];
    return s;
  });
  console.log('RESULT speedup_vs_js_loop x %s', (perJs / perDot).toFixed(2));

  time('bridge_sum_1m', 50, () => npsum(a));
  time('bridge_std_1m', 20, () => npstd(a));

  // Hot small calls: 16-element dot — the crossing itself.
  const a16 = a.subarray(0, 16);
  const b16 = b.subarray(0, 16);
  time('bridge_dot_16', 20000, () => dot(a16, b16));

  // The same call through the lane (Promise per call), then pipelined.
  const dotTask = py.task(dot);
  await timeAsync('lane_dot_16_await_each', 2000, () => dotTask(a16, b16));
  {
    const t0 = process.hrtime.bigint();
    const B = 2000;
    const ps = new Array(B);
    for (let i = 0; i < B; i++) ps[i] = dotTask(a16, b16);
    await Promise.all(ps);
    const ns = Number(process.hrtime.bigint() - t0);
    console.log('RESULT lane_dot_16_pipelined ms %s ns/call %s',
                (ns / 1e6).toFixed(3), Math.round(ns / B));
  }

  // Bulk through the lane: the loop stays free while numpy works.
  await timeAsync('lane_dot_1m', 50, () => dotTask(a, b));

  console.log('RESULT live_handles n %d', py.stats());
  await py.destroy();
  console.log('done');
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
