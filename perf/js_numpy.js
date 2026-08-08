// Driver for perf/js_numpy.ccs — numpy computing on Node's own arrays.
//
//   ./cc/bin/ccc build perf/js_numpy.ccs
//   node perf/js_numpy.js [n] [samples]
//
// Every rung passes the SAME Float64Array a JS loop would read: the module
// borrows it zero-copy into CC and re-lends it to numpy as a memoryview,
// so numpy's kernels run directly on V8's heap.  Controls are the naive
// JS loop on the same rung; small-n rungs price the double boundary
// (JS -> CC -> Python -> numpy) per call.
//
// Sums cross-check within relative epsilon — numpy's pairwise summation
// rounds differently than a sequential loop, identity is not expected.

'use strict';
const path = require('path');
const m = require(path.join(process.cwd(), 'bin', 'np.node'));

const N = Math.max(1, parseInt(process.argv[2] || '1000000', 10));
const samples = Math.min(64, Math.max(1, parseInt(process.argv[3] || '7', 10)));

if (!m.init()) {
  console.log('js_numpy SKIP (no libpython or no numpy)');
  process.exit(0);
}
console.log(`js_numpy engine=node ${process.version} n=${N} samples=${samples}`);

function median(a) {
  const v = [...a].sort((x, y) => x - y);
  const n = v.length;
  return n & 1 ? v[(n - 1) / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
function nowMs() { return Number(process.hrtime.bigint()) / 1e6; }
function close(a, b) {
  const d = Math.abs(a - b);
  return d <= 1e-9 * Math.max(1, Math.abs(a), Math.abs(b));
}

function bench(mode, fn, per) {
  per = per || 1;
  fn();
  const ms = [];
  let got;
  for (let s = 0; s < samples; s++) {
    const t0 = nowMs();
    const r = fn();
    ms.push(nowMs() - t0);
    got = r;
  }
  const md = median(ms);
  if (per > 1) {
    const ns = (md * 1e6) / per;
    console.log(`  ${mode.padEnd(14)} ${md.toFixed(3).padStart(9)} ms  ${ns.toFixed(0).padStart(8)} ns/call`);
    console.log(`RESULT scalar ${mode} ns_per_call ${ns.toFixed(0)}`);
  } else {
    console.log(`  ${mode.padEnd(14)} ${md.toFixed(3).padStart(9)} ms`);
    console.log(`RESULT bulk ${mode} ms ${md.toFixed(3)}`);
  }
  return { ms: md, got };
}

const A = new Float64Array(N);
const B = new Float64Array(N);
for (let i = 0; i < N; i++) {
  A[i] = (i % 1000) * 0.5;
  B[i] = ((i + 3) % 1000) * 0.25;
}

console.log(`\nbulk (${N} doubles, one Float64Array end to end):`);

const js_sum = bench('js_sum', () => {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += A[i];
  return acc;
});
const np_sum = bench('np_sum', () => m.sum(A));
if (!close(js_sum.got, np_sum.got)) {
  console.error(`MISMATCH sum: js=${js_sum.got} np=${np_sum.got}`);
  process.exit(1);
}

const js_dot = bench('js_dot', () => {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += A[i] * B[i];
  return acc;
});
const np_dot = bench('np_dot', () => m.dot(A, B));
if (!close(js_dot.got, np_dot.got)) {
  console.error(`MISMATCH dot: js=${js_dot.got} np=${np_dot.got}`);
  process.exit(1);
}

const js_std = bench('js_std', () => {
  let mean = 0;
  for (let i = 0; i < N; i++) mean += A[i];
  mean /= N;
  let acc = 0;
  for (let i = 0; i < N; i++) { const d = A[i] - mean; acc += d * d; }
  return Math.sqrt(acc / N);
});
const np_std = bench('np_std', () => m.std(A));
if (!close(js_std.got, np_std.got)) {
  console.error(`MISMATCH std: js=${js_std.got} np=${np_std.got}`);
  process.exit(1);
}

// Small-n: the whole JS -> CC -> Python -> numpy round trip per call.
const SN = 16;
const SA = new Float64Array(SN), SB = new Float64Array(SN);
for (let i = 0; i < SN; i++) { SA[i] = i + 0.5; SB[i] = 2 * i + 0.25; }
const CALLS = 20000;
console.log(`\nboundary (${SN}-element arrays, ${CALLS} calls):`);
const np_dot_small = bench('np_dot_16', () => {
  let acc = 0;
  for (let i = 0; i < CALLS; i++) acc += m.dot(SA, SB);
  return acc;
}, CALLS);
const js_dot_small = bench('js_dot_16', () => {
  let acc = 0;
  for (let i = 0; i < CALLS; i++) {
    let d = 0;
    for (let k = 0; k < SN; k++) d += SA[k] * SB[k];
    acc += d;
  }
  return acc;
}, CALLS);
if (!close(np_dot_small.got, js_dot_small.got)) {
  console.error(`MISMATCH dot16: np=${np_dot_small.got} js=${js_dot_small.got}`);
  process.exit(1);
}

console.log('\nratios (naive JS loop = 1.0):');
const r1 = np_sum.ms / js_sum.ms;
const r2 = np_dot.ms / js_dot.ms;
const r3 = np_std.ms / js_std.ms;
console.log(`  np_sum / js_sum   ${r1.toFixed(2).padStart(5)}x`);
console.log(`  np_dot / js_dot   ${r2.toFixed(2).padStart(5)}x`);
console.log(`  np_std / js_std   ${r3.toFixed(2).padStart(5)}x`);
console.log(`RESULT ratio np_sum_vs_js x ${r1.toFixed(2)}`);
console.log(`RESULT ratio np_dot_vs_js x ${r2.toFixed(2)}`);
console.log(`RESULT ratio np_std_vs_js x ${r3.toFixed(2)}`);
console.log('\nnumpy is computing directly on the Float64Array\'s memory: the');
console.log('borrow (JS->CC) and the memoryview (CC->numpy) both carry the');
console.log('same pointer; no copy exists on the path.');
