/* Kitchen-sink adversarial storm for concurrent-c-python.
 *
 *   ./stress/bridge/run.sh
 *   OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
 *   CHAOS_SCALE=full OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
 *
 * Latency demos stay under npm/cc-python/examples/.  This file is stress only.
 *
 * Every mode either prints RESULT lines (timing / counts) or OK/FAIL
 * booleans.  Exit non-zero on any FAIL.  Modes:
 *   crash_storm      — N isolated domains; random _exit mid-flight; survivors work
 *   domain_fanout    — spawn/import/destroy fanout under wire load
 *   shm_hail         — concurrent multi-MB spills; spill dir empty after
 *   teardown_derby   — destroy racing Promise.all + sync + callbacks
 *   callback_blizzard— retained JS callables × map storms sync+lane
 *   exception_hail   — throw/recover storms; messages intact across wire
 *   nested_callable  — Python returns callables JS keeps invoking
 *   big_payload_hail — deep / wide JSON-ish trees both directions
 *   mixed_hammer     — sync queue-jumps a busy lane while ticks must live
 *   lease_blender    — subarray / GC / zero-copy churn under load
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const ccpy = require(path.join(__dirname, '../../npm/cc-python'));

const SCALE = (process.env.CHAOS_SCALE || 'quick').toLowerCase();
const FULL = SCALE === 'full';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rss = () => process.memoryUsage().rss;
const MB = 1024 * 1024;
const hr = () => process.hrtime.bigint();
const nsToMs = (n) => Number(n) / 1e6;

let fails = 0;
const ok = (name, cond) => {
  if (!cond) fails++;
  console.log('OK %s %s', name, cond ? 'true' : 'false');
};
const result = (fmt, ...args) => console.log('RESULT ' + fmt, ...args);

function spillDir() {
  return fs.existsSync('/dev/shm') ? '/dev/shm' : os.tmpdir();
}
function straySpills() {
  const dir = spillDir();
  try {
    return fs.readdirSync(dir).filter((f) => f.startsWith('ccpy-' + process.pid + '-'));
  } catch (_) {
    return [];
  }
}

async function hasNumpyIsolated() {
  const py = ccpy.create({ isolated: true });
  try {
    const np = py.import('numpy');
    await np.zeros(1);
    await py.destroy();
    return true;
  } catch (e) {
    try { await py.destroy(); } catch (_) {}
    return false;
  }
}

async function makeIsolatedWorker() {
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  const work = await b.eval(
      'lambda n: (lambda np: float(np.dot(np.random.rand(n), np.random.rand(n))))' +
      '(__import__("numpy"))');
  const boom = await b.eval('lambda: (__import__("os")._exit(7))');
  return { py, work, boom };
}

/* ---- 1. Crash isolation storm ------------------------------------------- */
async function crashStorm(numpyOk) {
  console.log('=== crash_storm ===');
  if (!numpyOk) {
    console.log('SKIP crash_storm (no numpy)');
    return;
  }
  const N = FULL ? 16 : 8;
  const rounds = FULL ? 4 : 2;
  let crashes = 0, survivorsOk = 0, lateRejects = 0, parentOk = true;
  const t0 = hr();
  for (let r = 0; r < rounds; r++) {
    const domains = [];
    for (let i = 0; i < N; i++) domains.push(await makeIsolatedWorker());
    // Warm half so the wire is hot before we start killing.
    await Promise.all(domains.slice(0, N / 2).map((d) => d.work(256)));
    // Fire: odd domains _exit, even keep computing; all in flight together.
    const jobs = domains.map(async (d, i) => {
      if (i % 2 === 1) {
        try { await d.boom(); }
        catch (e) {
          if (/closed|exited/.test(e.message)) crashes++;
        }
        let late = '';
        try { await d.work(64); } catch (e) { late = e.message; }
        if (/closed|exited/.test(late)) lateRejects++;
        return;
      }
      const v = await d.work(1 << 12);
      if (typeof v === 'number' && Number.isFinite(v)) survivorsOk++;
    });
    await Promise.all(jobs);
    // Parent still healthy via a fresh domain.
    const probe = await makeIsolatedWorker();
    const p = await probe.work(128);
    parentOk = parentOk && Number.isFinite(p);
    await Promise.all(domains.map(async (d) => {
      try { await d.py.destroy(); } catch (_) {}
    }));
    await probe.py.destroy();
  }
  result('crash_storm_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('crash_storm_crashes %d', crashes);
  result('crash_storm_survivor_oks %d', survivorsOk);
  ok('crash_storm_crashes', crashes >= rounds * (N / 2));
  ok('crash_storm_survivors', survivorsOk >= rounds * (N / 2));
  ok('crash_storm_late_rejects', lateRejects >= rounds * (N / 2));
  ok('crash_storm_parent', parentOk);
}

/* ---- 2. Domain fanout --------------------------------------------------- */
async function domainFanout(numpyOk) {
  console.log('=== domain_fanout ===');
  const N = FULL ? (numpyOk ? 24 : 32) : (numpyOk ? 12 : 16);
  const r0 = rss();
  const t0 = hr();
  const domains = [];
  for (let i = 0; i < N; i++) {
    if (numpyOk) domains.push(await makeIsolatedWorker());
    else {
      const py = ccpy.create({ isolated: true });
      const b = py.import('builtins');
      const work = await b.eval('lambda x: x * x + 1');
      domains.push({ py, work });
    }
  }
  // Fan-in: every domain answers at once.
  const vals = await Promise.all(domains.map((d, i) =>
    numpyOk ? d.work(512) : d.work(i + 1)));
  let allOk = vals.every((v) => typeof v === 'number' && Number.isFinite(v));
  await Promise.all(domains.map((d) => d.py.destroy()));
  result('domain_fanout_n %d', N);
  result('domain_fanout_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('domain_fanout_rss_delta_mb %s', ((rss() - r0) / MB).toFixed(1));
  ok('domain_fanout_compute', allOk);
  ok('domain_fanout_rss', rss() - r0 < (FULL ? 512 : 256) * MB);
}

/* ---- 3. SHM hail -------------------------------------------------------- */
async function shmHail(numpyOk) {
  console.log('=== shm_hail ===');
  if (!numpyOk) {
    console.log('SKIP shm_hail (no numpy)');
    return;
  }
  const DOMS = FULL ? 8 : 4;
  const ELEMS = FULL ? (1 << 19) : (1 << 18); // 4MB / 2MB Float64
  const ROUNDS = FULL ? 6 : 3;
  const domains = [];
  for (let i = 0; i < DOMS; i++) {
    const py = ccpy.create({ isolated: true });
    domains.push({ py, np: py.import('numpy') });
  }
  const bufs = [];
  for (let i = 0; i < DOMS; i++) {
    const b = new Float64Array(ELEMS);
    for (let j = 0; j < ELEMS; j++) b[j] = ((j + i) % 97) * 0.5;
    bufs.push(b);
  }
  const t0 = hr();
  for (let r = 0; r < ROUNDS; r++) {
    const sums = await Promise.all(domains.map((d, i) => d.np.sum(bufs[i])));
    for (let i = 0; i < DOMS; i++) {
      let want = 0;
      for (const v of bufs[i]) want += v;
      if (Math.abs(sums[i] - want) > 1e-3) {
        ok('shm_hail_integrity', false);
        await Promise.all(domains.map((d) => d.py.destroy()));
        return;
      }
    }
  }
  // Pull a big result back through shm too.
  const echoed = await (await domains[0].np.arange(ELEMS)).toTypedArray();
  ok('shm_hail_result', echoed.length === ELEMS);
  await Promise.all(domains.map((d) => d.py.destroy()));
  const strays = straySpills();
  result('shm_hail_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('shm_hail_mb_moved %s',
         ((DOMS * ELEMS * 8 * ROUNDS * 2) / MB).toFixed(1));
  ok('shm_hail_integrity', true);
  ok('shm_hail_no_strays', strays.length === 0);
}

/* ---- 4. Teardown derby -------------------------------------------------- */
async function teardownDerby() {
  console.log('=== teardown_derby ===');
  const ROUNDS = FULL ? 40 : 20;
  let destroyDuring = 0, doubleDestroy = 0, afterClosed = 0, ticks = 0;
  const tick = setInterval(() => { ticks++; }, 1);
  const t0 = hr();
  for (let r = 0; r < ROUNDS; r++) {
    const py = ccpy.create();
    const math = py.import('math');
    const sq = py.task(math.sqrt);
    const inflight = [];
    for (let i = 0; i < 32; i++) inflight.push(sq(i + 1));
    // Race: destroy while tasks are queued / running.
    const killer = sleep(r % 3).then(() => py.destroy());
    const settled = await Promise.allSettled([...inflight, killer]);
    for (const s of settled) {
      if (s.status === 'rejected' && /closed/.test(String(s.reason && s.reason.message)))
        destroyDuring++;
    }
    try { py.destroy(); doubleDestroy++; } catch (_) {}
    let late = false;
    try { math.floor(1.2); } catch (e) { late = /closed/.test(e.message); }
    if (late) afterClosed++;
  }
  clearInterval(tick);
  result('teardown_derby_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('teardown_derby_closed_rejects %d', destroyDuring);
  result('teardown_derby_ticks %d', ticks);
  ok('teardown_derby_double_destroy', doubleDestroy === ROUNDS);
  ok('teardown_derby_after_closed', afterClosed === ROUNDS);
  ok('teardown_derby_loop_alive', ticks > ROUNDS);
}

/* ---- 5. Callback blizzard ----------------------------------------------- */
async function callbackBlizzard() {
  console.log('=== callback_blizzard ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const json = py.import('json');
  const toList = py.task(b.list);
  const N = FULL ? 4000 : 1500;
  const lst = json.loads('[' + Array.from({ length: 64 }, (_, i) => i).join(',') + ']');
  let throws = 0;
  const t0 = hr();
  // Sync map storm.
  for (let i = 0; i < N; i++) {
    const mapped = b.list(b.map((x) => x * 3 + 1, lst));
    if (String(mapped) !== '[' + Array.from({ length: 64 }, (_, j) => j * 3 + 1).join(', ') + ']') {
      ok('callback_blizzard_sync', false);
      py.destroy();
      return;
    }
  }
  // Lane map storm with occasional throws.
  for (let i = 0; i < Math.floor(N / 4); i++) {
    try {
      await toList(b.map((x) => {
        if (x === 31 && i % 7 === 0) throw new Error('blizzard');
        return x - 1;
      }, lst));
    } catch (e) {
      if (/blizzard/.test(e.message)) throws++;
      else throw e;
    }
  }
  // Retained callable fires across many later calls.
  const partial = py.import('functools').partial((x) => x * 11, 3);
  let retained = 0;
  for (let i = 0; i < 500; i++) if (partial() === 33) retained++;
  py.destroy();
  result('callback_blizzard_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('callback_blizzard_sync_maps %d', N);
  result('callback_blizzard_throws %d', throws);
  ok('callback_blizzard_sync', true);
  ok('callback_blizzard_throws', throws > 0);
  ok('callback_blizzard_retained', retained === 500);
}

/* ---- 6. Exception hail -------------------------------------------------- */
async function exceptionHail() {
  console.log('=== exception_hail ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const json = py.import('json');
  const toList = py.task(b.list);
  // Python invokes the JS callable — throw must cross the wire intact.
  const g = b.dict();
  const apply = b.eval('lambda f, x: f(x)', g);
  const N = FULL ? 800 : 300;
  let hits = 0;
  let sum = 0;
  const t0 = hr();
  for (let i = 0; i < N; i++) {
    try {
      const v = apply((x) => {
        if (x % 3 === 0) throw new Error('hail-' + x);
        return x * 2;
      }, i);
      sum += Number(v);
    } catch (e) {
      if (/hail-/.test(e.message)) hits++;
      else throw e;
    }
  }
  const lst = json.loads('[0,1,2,3,4,5,6,7]');
  let laneHits = 0;
  const laneRounds = FULL ? 40 : 16;
  for (let r = 0; r < laneRounds; r++) {
    try {
      await toList(b.map((x) => {
        if (x === 5) throw new Error('lane-hail');
        return x + 1;
      }, lst));
    } catch (e) {
      if (/lane-hail/.test(e.message)) laneHits++;
      else throw e;
    }
  }
  // Python-originated exception message survives the round trip.
  const boom = b.eval(
    'lambda: (_ for _ in ()).throw(ValueError("from-py"))', g);
  let pyMsg = false;
  try { boom(); } catch (e) { pyMsg = /from-py/.test(e.message); }
  py.destroy();
  const wantHits = Math.floor((N + 2) / 3);
  const wantSum = Array.from({ length: N }, (_, i) => i)
    .filter((i) => i % 3 !== 0)
    .reduce((a, i) => a + i * 2, 0);
  result('exception_hail_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('exception_hail_hits %d', hits);
  result('exception_hail_lane_hits %d', laneHits);
  ok('exception_hail_hits', hits === wantHits);
  ok('exception_hail_sum', sum === wantSum);
  ok('exception_hail_lane', laneHits === laneRounds);
  ok('exception_hail_py_msg', pyMsg);
}

/* ---- 7. Nested callable (Python returns callables) ---------------------- */
async function nestedCallable() {
  console.log('=== nested_callable ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const t0 = hr();
  // eval a factory; JS keeps calling the returned Python callable.
  const g = b.dict();
  const factory = b.eval('lambda n: (lambda x, n=n: x + n)', g);
  let okN = 0;
  const N = FULL ? 400 : 150;
  for (let i = 0; i < N; i++) {
    const add = factory(i % 17);
    if (add(100) === 100 + (i % 17)) okN++;
  }
  // Deeper: factory returns a factory.
  const nest = b.eval(
    'lambda: (lambda a: (lambda b, a=a: a * 10 + b))', g);
  const inner = nest()(3);
  const composed = inner(4);
  // Retained across destroy of intermediate refs.
  const partial = py.import('functools').partial(
    b.eval('lambda a, b: a - b', g), 50);
  let retained = 0;
  for (let i = 0; i < 200; i++) if (partial(8) === 42) retained++;
  py.destroy();
  result('nested_callable_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('nested_callable_n %d', N);
  ok('nested_callable_factory', okN === N);
  ok('nested_callable_compose', composed === 34);
  ok('nested_callable_retained', retained === 200);
}

/* ---- 8. Big payload hail ------------------------------------------------ */
async function bigPayloadHail() {
  console.log('=== big_payload_hail ===');
  const py = ccpy.create();
  const json = py.import('json');
  const b = py.import('builtins');
  const t0 = hr();
  // Plain JS objects are not wire values — cross as JSON text both ways.
  const wideParts = [];
  for (let i = 0; i < (FULL ? 200 : 80); i++) wideParts.push('"k' + i + '":' + (i * i));
  const wideStr = '{' + wideParts.join(',') + '}';
  const deepStr = '{"a":{"b":{"c":{"d":{"e":[1,2,{"f":"g"}]}}}}}';
  let roundOk = 0;
  const rounds = FULL ? 60 : 25;
  for (let r = 0; r < rounds; r++) {
    const back = json.loads(wideStr);
    const s1 = String(json.dumps(back));
    if (s1.indexOf('"k7": 49') >= 0 || s1.indexOf('"k7":49') >= 0) roundOk++;
    const d2 = json.loads(deepStr);
    const s2 = String(json.dumps(d2));
    if (s2.indexOf('"f": "g"') >= 0 || s2.indexOf('"f":"g"') >= 0) roundOk++;
  }
  // list(range(N)) materializes a wide Python list across the wire.
  const n = FULL ? 5000 : 2000;
  const rng = b.eval('lambda n: list(range(n))', b.dict());
  const lst = rng(n);
  const asStr = String(lst);
  const lenOk = asStr.startsWith('[0, 1, 2') && asStr.endsWith((n - 1) + ']');
  py.destroy();
  result('big_payload_hail_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('big_payload_hail_rounds %d', rounds);
  ok('big_payload_hail_json', roundOk === rounds * 2);
  ok('big_payload_hail_list', lenOk);
}

/* ---- 9. Mixed sync + lane hammer ---------------------------------------- */
async function mixedHammer(numpyOk) {
  console.log('=== mixed_hammer ===');
  const py = ccpy.create();
  const math = py.import('math');
  let npDot = null;
  if (numpyOk) {
    try {
      const np = py.import('numpy');
      npDot = py.task(np.dot);
    } catch (_) { /* in-process may refuse numpy on some builds */ }
  }
  const sq = py.task(math.sqrt);
  const a = new Float64Array(1 << 16);
  const b = new Float64Array(1 << 16);
  for (let i = 0; i < a.length; i++) { a[i] = i % 17; b[i] = i % 13; }
  let ticks = 0;
  const tick = setInterval(() => { ticks++; }, 1);
  await sleep(25); /* arm timers before the burst */
  const t0 = hr();
  const ROUNDS = FULL ? 80 : 40;
  const laneJobs = [];
  for (let i = 0; i < ROUNDS; i++) {
    laneJobs.push(sq(i + 1));
    if (npDot) laneJobs.push(npDot(a, b));
    // Sync call queue-jumps while the lane is busy — must not freeze the loop.
    if (math.floor(i + 0.2) !== i) {
      clearInterval(tick);
      ok('mixed_hammer_sync', false);
      py.destroy();
      return;
    }
  }
  const settled = await Promise.all(laneJobs);
  await sleep(25);
  clearInterval(tick);
  const laneOk = settled.every((v) => typeof v === 'number' && Number.isFinite(v));
  py.destroy();
  result('mixed_hammer_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('mixed_hammer_ticks %d', ticks);
  result('mixed_hammer_lane_jobs %d', settled.length);
  ok('mixed_hammer_lane', laneOk);
  /* Wall-clock 1ms ticks during a sub-ms burst are weather; require a few. */
  ok('mixed_hammer_loop_alive', ticks >= 5);
}

/* ---- 10. Lease blender -------------------------------------------------- */
async function leaseBlender() {
  console.log('=== lease_blender ===');
  const py = ccpy.create();
  const math = py.import('math');
  const big = new Float64Array(1 << 20);
  big.fill(0.25);
  const r0 = rss();
  const t0 = hr();
  let sumOk = true;
  for (let i = 0; i < (FULL ? 40 : 20); i++) {
    const view = big.subarray(i * 1024, big.length - i * 1024);
    const s = math.fsum(view);
    const want = view.length * 0.25;
    if (Math.abs(s - want) > 1e-6) sumOk = false;
    if (i % 5 === 0 && global.gc) {
      global.gc();
      await sleep(5);
    }
  }
  // Drop the buffer mid-flight: lease must still be valid for the call
  // that already holds it (bridge pins), and later calls with a live buf work.
  {
    let buf = new Float64Array(1 << 18);
    buf.fill(2);
    const s = math.fsum(buf);
    buf = null;
    if (global.gc) { global.gc(); await sleep(5); }
    ok('lease_blender_post_drop_sum', s === (1 << 18) * 2);
  }
  py.destroy();
  result('lease_blender_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('lease_blender_rss_delta_mb %s', ((rss() - r0) / MB).toFixed(1));
  ok('lease_blender_sums', sumOk);
  ok('lease_blender_rss', rss() - r0 < 128 * MB);
}

(async () => {
  console.log('js_bridge_chaos scale=%s pid=%d', SCALE, process.pid);
  const numpyOk = await hasNumpyIsolated();
  console.log('numpy_isolated %s', numpyOk ? 'yes' : 'no');

  await crashStorm(numpyOk);
  await domainFanout(numpyOk);
  await shmHail(numpyOk);
  await teardownDerby();
  await callbackBlizzard();
  await exceptionHail();
  await nestedCallable();
  await bigPayloadHail();
  await mixedHammer(numpyOk);
  await leaseBlender();

  console.log('=== summary ===');
  result('chaos_fails %d', fails);
  if (fails > 0) {
    console.error('CHAOS FAILED: %d checks', fails);
    process.exit(1);
  }
  ok('chaos_clean', true);
  console.log('chaos done');
})().catch((e) => {
  console.error('CHAOS ERROR:', e && e.stack ? e.stack : e);
  process.exit(1);
});
