/* Ordered bisect of cc_python_bridge_mem.js under ASan.
 * Runs rungs 1..N cumulatively; prints BISECT_OK / BISECT_CRASH markers.
 * Usage (inside sanitize Docker / with CC_PYTHON_ADDON + LD_PRELOAD set):
 *   node --expose-gc scripts/asan_mem_bisect.js [maxRung]
 *   BISECT_FROM=6 BISECT_TO=7 node --expose-gc scripts/asan_mem_bisect.js
 */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const gcNow = async () => {
  if (typeof global.gc === 'function') {
    global.gc();
    await sleep(10);
    global.gc();
    await sleep(10);
  }
};
const rss = () => process.memoryUsage().rss;
const MB = 1024 * 1024;

const ccpy = require(process.cwd() + '/npm/cc-python');

const FROM = Number(process.env.BISECT_FROM || 1);
const TO = Number(process.env.BISECT_TO || process.argv[2] || 12);

const rungs = [];

rungs[1] = async () => {
  console.log('=== rung 1 churn_stats ===');
  const py = ccpy.create();
  const math = py.import('math');
  py.release(math.sqrt);
  const base = py.stats();
  const r0 = rss();
  for (let i = 0; i < 20000; i++) py.release(math.sqrt);
  out('churn_stats_flat', py.stats() === base);
  out('churn_rss_flat', rss() - r0 < 48 * MB);
  await py.destroy();
};

rungs[2] = async () => {
  console.log('=== rung 2 gc_churn ===');
  const py = ccpy.create();
  const math = py.import('math');
  void math.sqrt;
  const base = py.stats();
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < 5000; i++) void math.sqrt;
    await gcNow();
  }
  await gcNow();
  out('gc_churn_stats', py.stats() <= base + 50);
  await py.destroy();
};

rungs[3] = async () => {
  console.log('=== rung 3 domain_churn ===');
  const r0 = rss();
  let compute = true;
  for (let i = 0; i < 100; i++) {
    const py = ccpy.create();
    const m = py.import('math');
    compute = compute && m.floor(9.9) === 9;
    await py.destroy();
  }
  await gcNow();
  out('domain_churn_compute', compute);
  out('domain_churn_rss', rss() - r0 < 64 * MB);
};

rungs[4] = async () => {
  console.log('=== rung 4 storm_use_after_close ===');
  const py = ccpy.create();
  const math = py.import('math');
  let keep = [];
  for (let i = 0; i < 2000; i++) keep.push(math.sqrt);
  await py.destroy();
  let closedErr = false;
  try { keep[0](4); } catch (e) { closedErr = /closed/.test(e.message); }
  out('storm_use_after_close', closedErr);
  keep = null;
  await gcNow();
  await gcNow();
  out('storm_survived_gc', true);
};

rungs[5] = async () => {
  console.log('=== rung 5 graph_pins ===');
  let wr;
  let proxy = (() => {
    const py = ccpy.create();
    wr = new WeakRef(py);
    return py.import('math').sqrt;
  })();
  await gcNow();
  out('graph_pins_domain', wr.deref() !== undefined);
  out('graph_alive_works', proxy(9) === 3);
  proxy = null;
  let collected = false;
  for (let i = 0; i < 20 && !collected; i++) {
    await gcNow();
    collected = wr.deref() === undefined;
  }
  out('graph_release_collects', collected);
};

rungs[6] = async () => {
  console.log('=== rung 6 mixed_order_finalizers ===');
  let wr;
  (() => {
    const py = ccpy.create();
    wr = new WeakRef(py);
    const math = py.import('math');
    const keep = [];
    for (let i = 0; i < 500; i++) keep.push(math.sqrt);
  })();
  let collected = false;
  for (let i = 0; i < 20 && !collected; i++) {
    await gcNow();
    collected = wr.deref() === undefined;
  }
  out('mixed_order_finalizers', collected);
};

rungs[7] = async () => {
  console.log('=== rung 7 lease_sum ===');
  const py = ccpy.create();
  const math = py.import('math');
  const buf = new Float64Array(1 << 20);
  buf.fill(0.5);
  const r0 = rss();
  let s = 0;
  for (let i = 0; i < 10; i++) s = math.fsum(buf);
  out('lease_sum', s === buf.length * 0.5);
  out('lease_rss_flat', rss() - r0 < 48 * MB);
  await py.destroy();
};

rungs[8] = async () => {
  console.log('=== rung 8 keep_past_return (sync+alive+lane+destroy) ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const g = b.dict();
  b.exec(
    'G=[]\n'
    + 'def keep(mv):\n'
    + '  G.append(mv)\n'
    + '  return len(mv)\n'
    + 'def sum_ok(mv):\n'
    + '  return float(sum(mv))\n',
    g
  );
  const keep = b.eval('keep', g);
  const sumOk = b.eval('sum_ok', g);
  const buf = new Float64Array(32);
  buf.fill(2);
  let caught = false;
  try { keep(buf); } catch (e) {
    caught = /retained by the callee|borrow ends/.test(e.message);
  }
  out('keep_past_return', caught);
  out('keep_past_return_alive', sumOk(buf) === 64);
  console.log('BISECT_MARK before_lane');
  const keepT = py.task(keep);
  let laneCaught = false;
  try { await keepT(buf); } catch (e) {
    laneCaught = /retained by the callee|borrow ends/.test(e.message);
  }
  out('keep_past_return_lane', laneCaught);
  // Match mem.js: fire-and-forget destroy (do not await) unless
  // BISECT_AWAIT_DESTROY=1 — races the next rung under ASan.
  console.log('BISECT_MARK before_destroy_after_lane');
  if (process.env.BISECT_AWAIT_DESTROY === '1') {
    await py.destroy();
    console.log('BISECT_MARK after_destroy_after_lane awaited');
  } else {
    const d = py.destroy();
    console.log('BISECT_MARK after_destroy_kickoff', typeof d);
  }
};

rungs[9] = async () => {
  console.log('=== rung 9 released_handle ===');
  const py = ccpy.create();
  const math = py.import('math');
  const h = math.sqrt;
  py.release(h);
  let asRecv = false, asArg = false, again = false;
  try { h(4); } catch (e) { asRecv = /released|another bridge/.test(e.message); }
  try { math.hypot(h, 4); } catch (e) { asArg = /released|another bridge/.test(e.message); }
  try { py.release(h); } catch (e) { again = /released|another bridge/.test(e.message); }
  out('released_as_receiver', asRecv);
  out('released_as_argument', asArg);
  out('released_again', again);
  await py.destroy();
};

rungs[10] = async () => {
  console.log('=== rung 10 cross_domain ===');
  const A = ccpy.create();
  const B = ccpy.create();
  const bm = B.import('math');
  let rejected = false;
  try { bm.hypot(A.import('math').floor, 3); }
  catch (e) { rejected = /another bridge/.test(e.message); }
  out('cross_domain_arg', rejected);
  await A.destroy();
  await B.destroy();
};

rungs[11] = async () => {
  console.log('=== rung 11 interleaved ===');
  const doms = [];
  for (let i = 0; i < 20; i++) {
    const py = ccpy.create();
    doms.push([py, py.import('math')]);
  }
  for (let i = 0; i < 20; i += 2) await doms[i][0].destroy();
  let survivors = true;
  for (let i = 1; i < 20; i += 2)
    survivors = survivors && doms[i][1].floor(i + 0.5) === i;
  let closed = true;
  for (let i = 0; i < 20; i += 2) closed = closed && doms[i][0].closed;
  out('interleaved_survivors', survivors);
  out('interleaved_closed', closed);
  for (let i = 1; i < 20; i += 2) await doms[i][0].destroy();
};

rungs[12] = async () => {
  console.log('=== rung 12 chain ===');
  const py = ccpy.create();
  const os = py.import('os');
  out('chain_result', os.path.join('a', 'b') === 'a/b');
  const base = py.stats();
  for (let i = 0; i < 2000; i++) void os.path.join('a', 'b');
  await gcNow();
  await gcNow();
  out('chain_stats_bounded', py.stats() < base + 100);
  await py.destroy();
};

(async () => {
  console.log('BISECT_RANGE', FROM, TO);
  for (let i = FROM; i <= TO; i++) {
    if (!rungs[i]) continue;
    console.log('BISECT_ENTER', i);
    try {
      await rungs[i]();
      console.log('BISECT_OK', i);
    } catch (e) {
      console.error('BISECT_THROW', i, e && e.stack ? e.stack : e);
      process.exit(2);
    }
  }
  console.log('BISECT_DONE', FROM, TO);
})().catch((e) => {
  console.error('BISECT_FATAL', e && e.stack ? e.stack : e);
  process.exit(1);
});
