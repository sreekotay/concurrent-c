/* The magic demo, async form: plain node + require('cc-python') +
 * numpy through py.task — what the lane BUYS, measured.
 *
 *   node npm/cc-python/examples/js_numpy_bridge_async.js
 *
 * The sync bridge wins per-call latency (see js_numpy_bridge.js); the
 * lane buys the event loop back.  Rungs: awaited and pipelined lane
 * calls; a liveness meter (1ms ticks that keep firing during bulk
 * numpy, and flatline during the same work sync); and the overlap
 * proof — JS compute running WHILE numpy computes, wall clock near
 * max(js, numpy) instead of their sum. */
'use strict';

const ccpy = require(process.cwd() + '/npm/cc-python');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

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
  const jsDot = () => {
    let s = 0;
    for (let i = 0; i < N; i++) s += a[i] * b[i];
    return s;
  };

  const dotSync = np.dot;
  const dot = py.task(np.dot);

  // Cross-check before timing.
  const want = jsDot();
  const got = await dot(a, b);
  if (Math.abs(got - want) > Math.abs(want) * 1e-9)
    throw new Error('dot mismatch');

  // Awaited each: one event-loop turn per call rides along.
  {
    const iters = 50;
    await dot(a, b);
    const t0 = process.hrtime.bigint();
    for (let i = 0; i < iters; i++) await dot(a, b);
    const ns = Number(process.hrtime.bigint() - t0);
    console.log('RESULT lane_dot_1m_awaited ms %s ns/call %s',
                (ns / 1e6).toFixed(3), Math.round(ns / iters));
  }

  // Pipelined: submissions queue FIFO, turns amortize.
  {
    const B = 25;
    const t0 = process.hrtime.bigint();
    const ps = new Array(B);
    for (let i = 0; i < B; i++) ps[i] = dot(a, b);
    await Promise.all(ps);
    const ns = Number(process.hrtime.bigint() - t0);
    console.log('RESULT lane_dot_1m_pipelined ms %s ns/call %s',
                (ns / 1e6).toFixed(3), Math.round(ns / B));
  }

  // Liveness meter: 1ms ticks during 100ms of bulk numpy, lane vs sync.
  {
    let ticks = 0;
    const iv = setInterval(() => ticks++, 1);
    const t0 = Date.now();
    while (Date.now() - t0 < 100) await dot(a, b);
    clearInterval(iv);
    console.log('RESULT ticks_during_lane_100ms n %d', ticks);

    ticks = 0;
    const iv2 = setInterval(() => ticks++, 1);
    const t1 = Date.now();
    while (Date.now() - t1 < 100) dotSync(a, b);
    clearInterval(iv2);
    console.log('RESULT ticks_during_sync_100ms n %d', ticks);
  }

  // Overlap: JS computes while numpy computes.  BALANCED work — np.std
  // (~1.8ms) against the JS loop (~1.3ms) — so serial = js + numpy and
  // overlapped wall approaches max(js, numpy).  (An unbalanced pair
  // measures nothing: with js at 8x numpy there is nothing to overlap.)
  {
    const stdSync = np.std;
    const stdTask = py.task(np.std);
    const iters = 10;
    // Alternating best-of-3 trials: this box is noisy, and drift would
    // otherwise masquerade as (or hide) the overlap.
    let serial = Infinity;
    let overlapped = Infinity;
    stdSync(a);
    await stdTask(a);
    for (let trial = 0; trial < 3; trial++) {
      let t0 = process.hrtime.bigint();
      for (let i = 0; i < iters; i++) {
        stdSync(a); // numpy on main
        jsDot();    // then JS on main
      }
      serial = Math.min(serial, Number(process.hrtime.bigint() - t0) / iters);

      t0 = process.hrtime.bigint();
      for (let i = 0; i < iters; i++) {
        const p = stdTask(a); // numpy on the lane...
        jsDot();              // ...JS meanwhile, on another core
        await p;
      }
      overlapped =
        Math.min(overlapped, Number(process.hrtime.bigint() - t0) / iters);
    }
    console.log('RESULT serial_js_plus_numpy_ms %s', (serial / 1e6).toFixed(2));
    console.log('RESULT overlapped_js_and_numpy_ms %s', (overlapped / 1e6).toFixed(2));
    console.log('RESULT overlap_gain x %s', (serial / overlapped).toFixed(2));
  }

  // numpy OVERLAPPED WITH numpy, one domain: BLAS releases the GIL, so
  // a sync dot on main runs WHILE a task dot runs on the lane — two
  // numpy computations, two cores, one interpreter.  BLAS's own thread
  // pool COMPETES with this (each call already fans out, and bulk dot
  // is bandwidth-bound), so the pattern wants pinned BLAS:
  //
  //   OPENBLAS_NUM_THREADS=1 node examples/js_numpy_bridge_async.js
  //
  // — measured 2.02x pinned (perfect two-lane scaling) vs 0.44x letting
  // the pools fight.  Alternating best-of-3 as above.
  {
    const iters = 10;
    let serial = Infinity;
    let overlapped = Infinity;
    dotSync(a, b);
    await dot(a, b);
    for (let trial = 0; trial < 3; trial++) {
      let t0 = process.hrtime.bigint();
      for (let i = 0; i < iters; i++) {
        dotSync(a, b); // numpy on main...
        dotSync(a, b); // ...then numpy on main again
      }
      serial = Math.min(serial, Number(process.hrtime.bigint() - t0) / iters);

      t0 = process.hrtime.bigint();
      for (let i = 0; i < iters; i++) {
        const p = dot(a, b); // numpy on the lane (GIL released in BLAS)...
        dotSync(a, b);       // ...numpy on main, same interpreter
        await p;
      }
      overlapped =
        Math.min(overlapped, Number(process.hrtime.bigint() - t0) / iters);
    }
    const pinned = process.env.OPENBLAS_NUM_THREADS === '1' ||
                   process.env.OMP_NUM_THREADS === '1';
    console.log('RESULT numpy_x2_blas_pinned %s', pinned ? 'true' : 'false');
    console.log('RESULT numpy_x2_serial_ms %s', (serial / 1e6).toFixed(2));
    console.log('RESULT numpy_x2_overlapped_ms %s', (overlapped / 1e6).toFixed(2));
    console.log('RESULT numpy_x2_gain x %s', (serial / overlapped).toFixed(2));
  }

  console.log('RESULT live_handles n %d', py.stats());
  await py.destroy();
  console.log('done');
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
