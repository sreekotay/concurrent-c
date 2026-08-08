// Driver for perf/js_baseline.ccs — the JS -> CC boundary priced against
// native JavaScript controls on the same rung.
//
//   ./cc/bin/ccc build perf/js_baseline.ccs
//   node perf/js_baseline.js [iters] [samples]
//
// iters    scalar calls per timed sample   (default 300000)
// samples  timed samples per mode          (default 7; median reported)
//
// Every mode has a native control on the same rung, so each number answers
// "compared to what": a call into CC is read against a JS function call of
// the same shape, not against an inline add that performs no call at all.
//
// Scalar ladder (ns/call) — the work is one add (or one mul-add), chosen
// too small to hide the call:
//
//   c_native       CC calling CC (loop inside the addon)   the C floor
//   js_inline      `r += i + 7` in JS                      no call at all
//   js_call        `r += f(i, 7)`, f a JS function         what the JIT leaves of a call
//   js_call_mega   eight distinct targets, one call site   a call V8 cannot inline away
//   js_to_cc_raw   hand-written napi add, no binding       the napi floor
//   js_to_cc       `r += f(i, 7)`, f = m.add hoisted       crossing into CC
//   js_to_cc_prop  `r += m.add(i, 7)`                      + property lookup per call
//   js_to_cc_str   `r += f(s)`, one string argument        + the UTF-8 copy-in
//   js_to_cc_big   `r += f(B)`, argument beyond 2^53       the BigInt path
//
// js_to_cc reads against TWO controls on purpose.  V8 inlines a visible JS
// call to nothing, so js_call is the optimized floor, not a call;
// js_call_mega forces a megamorphic site with real dispatch, the honest
// "cost of a JS call" rung.  And js_to_cc minus js_to_cc_raw is the price
// of the generated binding alone — arity/keyword binding, _Generic
// marshal, state — versus the crossing cost no binding can go below.
//
// Warmup runs every mode before its samples: V8 tiers through interpreter,
// baseline, and optimizing JIT, and the first samples must not straddle a
// tier-up.  The js_call control is what the optimizer leaves of a JS call —
// V8 typically inlines it to nothing — so read it as the optimized-call
// floor, not a guaranteed call; js_inline can even time slower than it.
// Every mode's answer is cross-checked; a marshalling bug reports as
// MISMATCH, never as a timing.  Snapshots collect under perf/baselines/:
//
//   node perf/js_baseline.js > perf/baselines/js_baseline_node_$(date +%Y%m%d).txt

'use strict';
const path = require('path');
const m = require(path.join(process.cwd(), 'bin', 'js_baseline.node'));

const iters = Math.max(1, parseInt(process.argv[2] || '300000', 10));
const samples = Math.min(64, Math.max(1, parseInt(process.argv[3] || '7', 10)));
const WARM = Math.min(iters, 50000);

console.log(`js_baseline engine=node ${process.version} iters=${iters} samples=${samples}`);

function median(a) {
  const v = [...a].sort((x, y) => x - y);
  const n = v.length;
  return n & 1 ? v[(n - 1) / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

function nowMs() {
  return Number(process.hrtime.bigint()) / 1e6;
}

// Runs `fn(k)` (returning a checksum) warm, then `samples` times timed.
// Returns { ns, sum } with ns the median ns/call and sum the checksum,
// verified identical across samples.
function bench(mode, fn, per) {
  per = per || iters;
  fn(Math.min(per, WARM));
  const ms = [];
  let sum;
  for (let s = 0; s < samples; s++) {
    const t0 = nowMs();
    const r = fn(per);
    ms.push(nowMs() - t0);
    if (sum !== undefined && r !== sum) {
      console.error(`MISMATCH ${mode}: sample disagreement ${r} vs ${sum}`);
      process.exit(1);
    }
    sum = r;
  }
  const md = median(ms);
  const ns = (md * 1e6) / per;
  console.log(`  ${mode.padEnd(14)} ${md.toFixed(2).padStart(9)} ms  ${ns.toFixed(1).padStart(9)} ns/call`);
  console.log(`RESULT scalar ${mode} ns_per_call ${ns.toFixed(1)}`);
  return { ns, sum };
}

console.log('\nscalar (one add / one mul-add per call):');

const c_native = bench('c_native', (k) => m.c_native_sum(k));

const js_inline = bench('js_inline', (k) => {
  let r = 0;
  for (let i = 0; i < k; i++) r += i + 7;
  return r;
});

function jsAdd(x, y) { return x + y; }
const js_call = bench('js_call', (k) => {
  const f = jsAdd;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(i, 7);
  return r;
});

// Eight distinct function objects (distinct SharedFunctionInfos, via
// new Function) rotating through one call site: the site goes
// megamorphic, so V8 keeps a real dispatched call it cannot inline.
const megaFns = Array.from({ length: 8 }, () =>
  new Function('x', 'y', 'return x + y'));
const js_call_mega = bench('js_call_mega', (k) => {
  let r = 0;
  for (let i = 0; i < k; i++) r += megaFns[i & 7](i, 7);
  return r;
});

const js_to_cc_raw = bench('js_to_cc_raw', (k) => {
  const f = m.raw_add;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(i, 7);
  return r;
});

const js_to_cc = bench('js_to_cc', (k) => {
  const f = m.add;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(i, 7);
  return r;
});

const js_to_cc_prop = bench('js_to_cc_prop', (k) => {
  let r = 0;
  for (let i = 0; i < k; i++) r += m.add(i, 7);
  return r;
});

if (js_call.sum !== js_inline.sum || js_call_mega.sum !== js_inline.sum ||
    js_to_cc_raw.sum !== js_inline.sum || js_to_cc.sum !== js_inline.sum ||
    js_to_cc_prop.sum !== js_inline.sum) {
  console.error(`MISMATCH add: inline=${js_inline.sum} call=${js_call.sum} ` +
                `mega=${js_call_mega.sum} raw=${js_to_cc_raw.sum} ` +
                `to_cc=${js_to_cc.sum} prop=${js_to_cc_prop.sum}`);
  process.exit(1);
}

// The C floor's checksum, recomputed once natively.
{
  let want = 0;
  for (let i = 0; i < iters; i++) want += 3 * i + 7;
  if (c_native.sum !== want) {
    console.error(`MISMATCH c_native: got ${c_native.sum} want ${want}`);
    process.exit(1);
  }
}

const STR = 'hello, world';
const js_to_cc_str = bench('js_to_cc_str', (k) => {
  const f = m.str_len;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(STR);
  return r;
});
if (js_to_cc_str.sum !== STR.length * iters) {
  console.error(`MISMATCH str_len: got ${js_to_cc_str.sum}`);
  process.exit(1);
}

const B0 = 9007199254740993n; // 2^53 + 1
const B1 = 9007199254740994n; // 2^53 + 2
const js_to_cc_big = bench('js_to_cc_big', (k) => {
  const f = m.big_echo;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(i & 1 ? B1 : B0);
  return r;
});
{
  const want = Math.floor(iters / 2) * 3 + (iters & 1 ? 1 : 0);
  if (js_to_cc_big.sum !== want) {
    console.error(`MISMATCH big_echo: got ${js_to_cc_big.sum} want ${want}`);
    process.exit(1);
  }
}

// ---- outbound: crossing out of CC, at three arities ----
//
//   cc_to_js_0/1/3  CC calls a global JS function        the other direction
//   js_to_js        the same function called from JS     cc_to_js's control
//
// One addon call carries the whole timed loop (a tenth of iters: these are
// the slowest scalar modes), so ns/call is the inner count's share.
globalThis.noargs_js = () => 1;
globalThis.one_js = (x) => x;
globalThis.mul_add_js = (x, k, z) => x * k + z;
const on = Math.max(1, Math.floor(iters / 10));
console.log(`\noutbound (${on} calls):`);
const cc_to_js_0 = bench('cc_to_js_0', (k) => m.cc_to_js0(k), on);
const cc_to_js_1 = bench('cc_to_js_1', (k) => m.cc_to_js1(k), on);
const cc_to_js_3 = bench('cc_to_js_3', (k) => m.cc_to_js3(k), on);
const js_to_js = bench('js_to_js', (k) => {
  const f = globalThis.mul_add_js;
  let r = 0;
  for (let i = 0; i < k; i++) r += f(3, i, 7);
  return r;
}, on);
if (cc_to_js_3.sum !== js_to_js.sum) {
  console.error(`MISMATCH cc_to_js: ${cc_to_js_3.sum} vs ${js_to_js.sum}`);
  process.exit(1);
}

// ---- bulk: where the data lives decides the form ----
//
//   js_typed_sum   JS loop over a Float64Array     the typed JS floor
//   js_array_sum   JS loop over a plain Array      the boxed JS floor
//   recv_typed     m.sum_f64(Float64Array)         one crossing, zero-copy borrow
//   recv_array     m.sum_f64(Array)                per-element convert
//   send_typed     m.series()                      CC -> fresh Float64Array, one memcpy

const N = Math.max(1, parseInt(process.argv[4] || '1000000', 10));
console.log(`\nbulk (${N} doubles):`);

const A = new Float64Array(N);
for (let i = 0; i < N; i++) A[i] = (i % 1000) * 0.5;
const L = Array.from(A);

function benchBulk(mode, fn) {
  fn();
  const ms = [];
  let got;
  for (let s = 0; s < samples; s++) {
    const t0 = nowMs();
    const r = fn();
    ms.push(nowMs() - t0);
    if (got !== undefined && r !== got) {
      console.error(`MISMATCH ${mode}: sample disagreement`);
      process.exit(1);
    }
    got = r;
  }
  const md = median(ms);
  console.log(`  ${mode.padEnd(14)} ${md.toFixed(2).padStart(9)} ms`);
  console.log(`RESULT bulk ${mode} ms ${md.toFixed(2)}`);
  return { ms: md, got };
}

const js_typed_sum = benchBulk('js_typed_sum', () => {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += A[i];
  return acc;
});
const js_array_sum = benchBulk('js_array_sum', () => {
  let acc = 0;
  for (let i = 0; i < N; i++) acc += L[i];
  return acc;
});
const recv_typed = benchBulk('recv_typed', () => m.sum_f64(A));
const recv_array = benchBulk('recv_array', () => m.sum_f64(L));
if (recv_typed.got !== js_typed_sum.got || recv_array.got !== js_typed_sum.got ||
    js_array_sum.got !== js_typed_sum.got) {
  console.error('MISMATCH bulk sums');
  process.exit(1);
}

if (m.series_prep(N) !== N) { console.error('series_prep failed'); process.exit(1); }
{
  const S = m.series();
  if (!(S instanceof Float64Array) || m.sum_f64(S) !== js_typed_sum.got) {
    console.error('MISMATCH series content');
    process.exit(1);
  }
}
const send_typed = benchBulk('send_typed', () => m.series().length);
if (send_typed.got !== N) { console.error('MISMATCH send_typed'); process.exit(1); }

console.log('\nratios (native control = 1.0):');
const r1 = js_to_cc.ns / js_call_mega.ns;
const r2 = js_to_cc.ns / js_to_cc_raw.ns;
const r3 = js_to_cc_prop.ns / js_to_cc.ns;
const r4 = js_inline.ns / c_native.ns;
const r5 = js_to_cc_str.ns / js_to_cc.ns;
const r6 = js_to_cc_big.ns / js_to_cc.ns;
console.log(`  js_to_cc / js_call_mega ${r1.toFixed(2).padStart(5)}x   crossing into CC vs a real JS call`);
console.log(`  js_to_cc / js_to_cc_raw ${r2.toFixed(2).padStart(5)}x   the generated binding vs the napi floor`);
console.log(`  js_to_cc_prop/js_to_cc  ${r3.toFixed(2).padStart(5)}x   the per-call property lookup`);
console.log(`  js_inline / c_native    ${r4.toFixed(1).padStart(5)}x   the two floors`);
console.log(`  js_to_cc_str/js_to_cc   ${r5.toFixed(2).padStart(5)}x   the string copy-in`);
console.log(`  js_to_cc_big/js_to_cc   ${r6.toFixed(2).padStart(5)}x   the BigInt path`);
console.log(`RESULT ratio js_to_cc_vs_js_call_mega x ${r1.toFixed(2)}`);
console.log(`RESULT ratio binding_vs_napi_floor x ${r2.toFixed(2)}`);
console.log(`RESULT ratio prop_vs_hoisted x ${r3.toFixed(2)}`);
console.log(`RESULT ratio str_vs_add x ${r5.toFixed(2)}`);
console.log(`RESULT ratio big_vs_add x ${r6.toFixed(2)}`);
const r9 = cc_to_js_3.ns / js_to_js.ns;
console.log(`  cc_to_js_3 / js_to_js   ${r9.toFixed(2).padStart(5)}x   crossing out of CC vs a JS call`);
console.log(`RESULT ratio cc_to_js_vs_js_to_js x ${r9.toFixed(2)}`);
const r7 = recv_array.ms / recv_typed.ms;
const r8 = recv_typed.ms / js_typed_sum.ms;
console.log(`  recv_array / recv_typed ${r7.toFixed(1).padStart(5)}x   what the zero-copy borrow removes`);
console.log(`  recv_typed / js_typed   ${r8.toFixed(2).padStart(5)}x   one crossing over the typed JS floor`);
console.log(`RESULT ratio recv_array_vs_typed x ${r7.toFixed(1)}`);
console.log(`RESULT ratio recv_typed_vs_js_typed x ${r8.toFixed(2)}`);

console.log(`\nagree=${js_inline.sum} calls=${m.calls()}`);
