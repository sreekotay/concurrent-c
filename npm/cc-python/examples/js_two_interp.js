/* Two interpreters, one buffer: JS is the neutral ground.
 *
 *   node npm/cc-python/examples/js_two_interp.js
 *
 * Domains reject each other's handles (isolation is real), so data
 * moves between interpreters BY LIVING IN JS: one Float64Array leases
 * zero-copy into both, scalars hop A -> JS -> B.  Domain A is the
 * process interpreter (numpy loads there); domain B is an isolated
 * subinterpreter — numpy refuses subinterpreters (C-extension rule,
 * surfaced articulately), so B computes in pure Python, and the two
 * lanes genuinely overlap under their own GILs. */
'use strict';

const ccpy = require(__dirname + '/..'); // the package (works in-repo and installed)

(async () => {
  const A = ccpy.create();
  const B = ccpy.create();
  const np = A.import('numpy');
  try {
    B.import('numpy');
    console.log('RESULT b_numpy loaded (unexpected)');
  } catch (e) {
    console.log('RESULT b_numpy_refused true'); // subinterpreters refuse C extensions
  }
  const fsum = B.task(B.import('math').fsum);

  const N = 1 << 20;
  const buf = new Float64Array(N);
  for (let i = 0; i < N; i++) buf[i] = (i % 97) * 0.25;

  // One buffer, two interpreters, zero copies.
  const viaA = np.sum(buf);
  const viaB = await fsum(buf);
  console.log('RESULT same_bytes_both_interps %s',
              Math.abs(viaA - viaB) < 1e-6 ? 'true' : 'false');

  // A scalar hops A -> JS -> B.
  const dotA = np.dot(buf, buf);
  const rootB = await B.task(B.import('math').sqrt)(dotA);
  console.log('RESULT scalar_hop_a_js_b %s',
              Math.abs(rootB * rootB - dotA) < 1e-3 ? 'true' : 'false');

  // Two lanes, two GILs: numpy on A while pure Python grinds on B.
  const stdA = A.task(np.std);
  await stdA(buf);
  let t0 = process.hrtime.bigint();
  await stdA(buf);
  const msA = Number(process.hrtime.bigint() - t0) / 1e6;
  t0 = process.hrtime.bigint();
  await fsum(buf);
  const msB = Number(process.hrtime.bigint() - t0) / 1e6;
  t0 = process.hrtime.bigint();
  await Promise.all([stdA(buf), fsum(buf)]);
  const msBoth = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log('RESULT lane_a_ms %s lane_b_ms %s together_ms %s parallel %s',
              msA.toFixed(1), msB.toFixed(1), msBoth.toFixed(1),
              msBoth < msA + msB * 0.8 ? 'true' : 'false');

  await Promise.all([A.destroy(), B.destroy()]);
  console.log('done');
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
