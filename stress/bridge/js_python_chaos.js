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
 *   isolated_handle_boomerang — isolated wire: JS cb returns nested callables
 *   isolated_pipeline_cbs     — Promise.all + nested cbs on one child
 *   isolated_destroy_from_cb  — destroy() inside JS cb while broker waits cbr
 *   callback_buffer_path      — typed arrays on nested cbr (inline+spill)
 *   parking_shm               — Promise.all spill args × nested async cbs
 *   keep_past_return — callee stashes memoryview; articulate under load
 *   asyncio_lane_storm        — coro + awaited cb + destroy-from-await
 *   release_during_suspend    — release/GC while stage-1 cb promise pending
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

/* ---- 9. Isolated handle boomerang --------------------------------------- */
async function isolatedHandleBoomerang() {
  console.log('=== isolated_handle_boomerang ===');
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  const apply = await b.eval('lambda f, x: f(x)');
  const N = FULL ? 200 : 80;
  const t0 = hr();
  let okN = 0;
  for (let i = 0; i < N; i++) {
    const f = await apply((n) => (x) => x + n, i % 17);
    if ((await f(100)) === 100 + (i % 17)) okN++;
    if (i % 9 === 0 && global.gc) global.gc();
  }
  // Multi-arg pick: return one of two nested callables.
  const pick = await b.eval('lambda f: f((lambda x: x + 1), (lambda x: x + 2))(10)');
  const picked = await pick((a, b) => b);
  await py.destroy();
  result('isolated_handle_boomerang_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('isolated_handle_boomerang_n %d', N);
  ok('isolated_handle_boomerang_n', okN === N);
  ok('isolated_handle_boomerang_pick', picked === 12);
}

/* ---- 10. Isolated pipelined cbs ----------------------------------------- */
async function isolatedPipelineCbs() {
  console.log('=== isolated_pipeline_cbs ===');
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  const apply = await b.eval('lambda f, x: f(x)');
  const N = FULL ? 80 : 40;
  const t0 = hr();
  // Many in-flight calls; each JS cb returns a nested callable.
  const jobs = [];
  for (let i = 0; i < N; i++) {
    jobs.push((async () => {
      const f = await apply((n) => (x) => x + n, i);
      return await f(100);
    })());
  }
  const vals = await Promise.all(jobs);
  // Async cbs + pipeline (parent awaits before cbr; broker parks ops).
  const jobs2 = [];
  for (let i = 0; i < N; i++) {
    jobs2.push((async () => {
      const f = await apply(async (n) => {
        await sleep(1);
        return (x) => x + n;
      }, i);
      return await f(50);
    })());
  }
  const vals2 = await Promise.all(jobs2);
  // Throw from pipelined cb must not desync the FIFO.
  let throws = 0;
  const jobs3 = [];
  for (let i = 0; i < N; i++) {
    jobs3.push(apply((x) => {
      if (x % 2 === 0) throw new Error('pipe-hail-' + x);
      return x * 3;
    }, i).then(
      (v) => v,
      (e) => { if (/pipe-hail-/.test(e.message)) { throws++; return null; } throw e; }
    ));
  }
  const vals3 = await Promise.all(jobs3);
  await py.destroy();
  result('isolated_pipeline_cbs_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('isolated_pipeline_cbs_n %d', N);
  ok('isolated_pipeline_cbs_handles',
     vals.every((v, i) => v === 100 + i));
  ok('isolated_pipeline_cbs_async',
     vals2.every((v, i) => v === 50 + i));
  ok('isolated_pipeline_cbs_throws', throws === Math.ceil(N / 2));
  ok('isolated_pipeline_cbs_odds',
     vals3.every((v, i) => (i % 2 === 0 ? v === null : v === i * 3)));
}

/* ---- 11. Keep-past-return lease ----------------------------------------- */
async function keepPastReturn() {
  console.log('=== keep_past_return ===');
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
  const N = FULL ? 60 : 25;
  const t0 = hr();
  let caught = 0;
  let good = 0;
  for (let i = 0; i < N; i++) {
    const buf = new Float64Array(64 + (i % 8));
    buf.fill(2);
    try {
      keep(buf);
    } catch (e) {
      if (/retained by the callee|borrow ends/.test(e.message)) caught++;
      else throw e;
    }
    // Interleave with a legal lease use — domain must stay healthy.
    if (sumOk(buf) === buf.length * 2) good++;
  }
  // Lane path: same articulate error off-thread.
  const keepT = py.task(keep);
  let laneCaught = 0;
  for (let i = 0; i < (FULL ? 20 : 8); i++) {
    const buf = new Float64Array(32);
    buf.fill(1);
    try {
      await keepT(buf);
    } catch (e) {
      if (/retained by the callee|borrow ends/.test(e.message)) laneCaught++;
      else throw e;
    }
  }
  py.destroy();
  result('keep_past_return_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('keep_past_return_n %d', N);
  ok('keep_past_return_caught', caught === N);
  ok('keep_past_return_good', good === N);
  ok('keep_past_return_lane', laneCaught === (FULL ? 20 : 8));
}

/* ---- 12. Isolated destroy-from-JS-cb ------------------------------------ */
async function isolatedDestroyFromCb() {
  console.log('=== isolated_destroy_from_cb ===');
  const rounds = FULL ? 24 : 10;
  const t0 = hr();
  let got = 0, closed = 0, late = 0;
  for (let r = 0; r < rounds; r++) {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const apply = await b.eval('lambda f: f()');
    try {
      const v = await apply(() => { py.destroy(); return 42; });
      if (v === 42 || (typeof v === 'string' && /closed/.test(v))) got++;
    } catch (e) {
      if (/closed|exited/.test(e.message)) got++;
      else throw e;
    }
    if (py.closed) closed++;
    try { await py.import('math').pi; }
    catch (e) { if (/closed/.test(e.message)) late++; }
    try { await py.destroy(); } catch (_) {}
  }
  result('isolated_destroy_from_cb_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('isolated_destroy_from_cb_rounds %d', rounds);
  ok('isolated_destroy_from_cb_got', got === rounds);
  ok('isolated_destroy_from_cb_closed', closed === rounds);
  ok('isolated_destroy_from_cb_late', late === rounds);
}

/* ---- 13. Callback buffer path (in-process + isolated) ------------------- */
async function callbackBufferPath() {
  console.log('=== callback_buffer_path ===');
  const t0 = hr();
  // In-process: copy out of a leased buffer inside Python (no retain).
  {
    const py = ccpy.create();
    const b = py.import('builtins');
    const math = py.import('math');
    const g = b.dict();
    b.exec('def dbl(xs):\n  return [float(x) * 2 for x in xs]\n', g);
    const dbl = b.eval('dbl', g);
    const buf = new Float64Array([1, 2, 3, 4]);
    const mapped = dbl(buf);
    const okMap = String(mapped) === '[2.0, 4.0, 6.0, 8.0]' ||
                  String(mapped) === '[2, 4, 6, 8]';
    const big = new Float64Array((1 << 16) / 8 + 8);
    big.fill(1);
    const s = math.fsum(big);
    py.destroy();
    ok('callback_buffer_path_inproc_map', okMap);
    ok('callback_buffer_path_inproc_spill', s === big.length);
  }
  // Isolated: buffer as cb arg AND return (inline + spill via cbr).
  {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const apply = await b.eval(
      'lambda f, a: (lambda b: float(sum(b)))(f(a))');
    const small = new Float64Array([1, 2, 3, 4]);
    const doubled = await apply((a) => {
      const out = new Float64Array(a.length);
      for (let i = 0; i < a.length; i++) out[i] = a[i] * 2;
      return out;
    }, small);
    const big = new Float64Array((1 << 16) / 8 + 16);
    big.fill(3);
    const bigSum = await apply((a) => {
      // Echo spill-sized buffer back through cbr.
      return a;
    }, big);
    // Python → JS cb as typed array (broker $ta/$shm), not opaque $h.
    const pya = await b.eval('lambda f: f(__import__("array").array("d", [5,6,7]))');
    let fromPy = 0;
    await pya((a) => {
      fromPy = a.length && a[0] === 5 ? a.length : -1;
      return 0;
    });
    await py.destroy();
    ok('callback_buffer_path_iso_small', doubled === 20);
    ok('callback_buffer_path_iso_spill', bigSum === big.length * 3);
    ok('callback_buffer_path_iso_py_ta', fromPy === 3);
  }
  result('callback_buffer_path_ms %s', nsToMs(hr() - t0).toFixed(1));
}

/* ---- 14. Parking × shm -------------------------------------------------- */
async function parkingShm() {
  console.log('=== parking_shm ===');
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  const apply = await b.eval('lambda f, a: float(sum(f(a)))');
  const N = FULL ? 24 : 12;
  const ELEMS = (1 << 16) / 8 + 32; // spill
  const t0 = hr();
  // Promise.all pipelines spill-sized args; async nested cbs park later ops.
  const jobs = [];
  for (let i = 0; i < N; i++) {
    const buf = new Float64Array(ELEMS);
    buf.fill(2);
    jobs.push(apply(async (a) => {
      await sleep(1);
      return a;
    }, buf));
  }
  const vals = await Promise.all(jobs);
  await py.destroy();
  result('parking_shm_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('parking_shm_n %d', N);
  result('parking_shm_elems %d', ELEMS);
  ok('parking_shm_sums', vals.every((v) => v === ELEMS * 2));
}

/* ---- 15. asyncio lane storm --------------------------------------------- */
async function asyncioLaneStorm() {
  console.log('=== asyncio_lane_storm ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const ns = b.dict();
  b.exec(
    'import asyncio\n'
    + 'async def work(tag, n):\n'
    + '    await asyncio.sleep(n)\n'
    + '    return tag\n'
    + 'async def fetch2(cb, x):\n'
    + '    v = await cb(x)\n'
    + '    return v * 2\n'
    + 'async def forever():\n'
    + '    await asyncio.sleep(3600)\n',
    ns
  );
  const work = ns.get('work');
  const fetch2 = ns.get('fetch2');
  const N = FULL ? 40 : 16;
  const t0 = hr();
  const tags = await Promise.all(
    Array.from({ length: N }, (_, i) => py.task(work)('t' + i, 0.005))
  );
  let fetchOk = 0;
  for (let i = 0; i < N; i++) {
    const r = await py.task(fetch2)(async (x) => {
      await sleep(2);
      return x + 1;
    }, i);
    if (r === (i + 1) * 2) fetchOk++;
  }
  // Destroy while an awaited cb is still pending.
  const py2 = ccpy.create();
  const b2 = py2.import('builtins');
  const ns2 = b2.dict();
  b2.exec(
    'import asyncio\n'
    + 'async def fetch2(cb, x):\n'
    + '    v = await cb(x)\n'
    + '    return v\n',
    ns2
  );
  let destroyReject = false;
  const hung = py2.task(ns2.get('fetch2'))(async () => {
    await sleep(500);
    return 1;
  }, 0);
  const got = hung.then(() => 'ok', (e) => (/closed/.test(e.message) ? 'rej' : 'other'));
  await sleep(20);
  await py2.destroy();
  destroyReject = (await got) === 'rej';
  py.destroy();
  result('asyncio_lane_storm_ms %s', nsToMs(hr() - t0).toFixed(1));
  ok('asyncio_lane_storm_work', tags.length === N && String(tags[0]) === 't0');
  ok('asyncio_lane_storm_fetch', fetchOk === N);
  ok('asyncio_lane_storm_destroy', destroyReject);
}

/* ---- 16. release / GC during stage-1 suspension ------------------------- */
async function releaseDuringSuspend() {
  console.log('=== release_during_suspend ===');
  const py = ccpy.create();
  const b = py.import('builtins');
  const math = py.import('math');
  const ns = b.dict();
  b.exec(
    'def score(fetch, user):\n'
    + '    row = fetch(user)\n'
    + '    return user + ":" + str(row)\n',
    ns
  );
  const score = ns.get('score');
  const sqrt = math.sqrt;
  const t0 = hr();
  // Lane so the event loop stays live while Python awaits the JS cb.
  const laneScore = py.task(score);
  let ticks = 0;
  const tick = setInterval(() => { ticks++; }, 1);
  await sleep(15);
  let h2 = math.ceil;
  const r = await laneScore(async (user) => {
    await sleep(40);
    try { py.release(h2); } catch (_) {}
    h2 = null;
    if (global.gc) global.gc();
    return await py.task(sqrt)(49);
  }, 'bob');
  await sleep(15);
  clearInterval(tick);
  const alive = math.floor(3.7) === 3;
  py.destroy();
  result('release_during_suspend_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('release_during_suspend_ticks %d', ticks);
  ok('release_during_suspend_result', String(r) === 'bob:7.0' || String(r) === 'bob:7');
  ok('release_during_suspend_alive', alive);
  ok('release_during_suspend_loop', ticks >= 5);
}

/* ---- 17. Mixed sync + lane hammer --------------------------------------- */
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

/* ---- 18. Lease blender -------------------------------------------------- */
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
  await isolatedHandleBoomerang();
  await isolatedPipelineCbs();
  await isolatedDestroyFromCb();
  await callbackBufferPath();
  await parkingShm();
  await keepPastReturn();
  await asyncioLaneStorm();
  await releaseDuringSuspend();
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
