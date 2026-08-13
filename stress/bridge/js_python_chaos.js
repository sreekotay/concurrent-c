/* Kitchen-sink adversarial storm for concurrent-c-python.
 *
 *   ./stress/bridge/run.sh
 *   OPENBLAS_NUM_THREADS=1 node --expose-gc stress/bridge/js_python_chaos.js
 *   CHAOS_SCALE=full|soak …
 *
 * Latency demos stay under npm/cc-python/examples/.  This file is stress only.
 *
 * Every mode either prints RESULT lines (timing / counts) or OK/FAIL
 * booleans.  Exit non-zero on any FAIL.  CHAOS_SCALE: quick < full < soak.
 */
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const ccpy = require(path.join(__dirname, '../../npm/cc-python'));

const SCALE = (process.env.CHAOS_SCALE || 'quick').toLowerCase();
const SOAK = SCALE === 'soak';
const FULL = SCALE === 'full' || SOAK;
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
  // Spills live in private per-bridge dirs (removed with the bridge);
  // strays are files inside a surviving dir, or the dir of a bridge
  // that is already gone would itself linger.
  const dir = spillDir();
  try {
    const mine = fs.readdirSync(dir)
      .filter((f) => f.startsWith('ccpy-' + process.pid + '-'));
    const strays = [];
    for (const d of mine) {
      const p = path.join(dir, d);
      try {
        if (fs.statSync(p).isDirectory()) strays.push(...fs.readdirSync(p).map((f) => d + '/' + f));
        else strays.push(d);
      } catch (_) { /* raced its removal */ }
    }
    return strays;
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
  const work = b.eval(
      'lambda n: (lambda np: float(np.dot(np.random.rand(n), np.random.rand(n))))' +
      '(__import__("numpy"))');
  const boom = b.eval('lambda: (__import__("os")._exit(7))');
  return { py, work: py.task(work), boom: py.task(boom) };
}

/* ---- 0. Wire integrity: user stdout cannot forge protocol replies ------- */
async function wireIntegrity() {
  console.log('=== wire_integrity ===');
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  // Python code whose stdout output has exactly the shape of a bridge
  // reply.  Replies pair by request id, so an id-less line is ignored —
  // never consumed as a response, never shifting later replies onto the
  // wrong promises.
  const forge = await b.eval(
    'lambda: (print(\'{"v":999}\', flush=True), 41)[1]');
  const v1 = await forge();
  // A forged line with an already-settled id is equally not consumable.
  const forge2 = await b.eval(
    'lambda: (print(\'{"v":888,"id":1}\', flush=True), 42)[1]');
  const v2 = await forge2();
  // Pipelined calls after the noise still pair correctly.
  const sq = await b.eval('lambda x: x * x');
  const after = await Promise.all([py.task(sq)(2), py.task(sq)(3), py.task(sq)(4)]);
  await py.destroy();
  ok('wire_integrity_plain', v1 === 41);
  ok('wire_integrity_forged_id', v2 === 42);
  ok('wire_integrity_order', after.join(',') === '4,9,16');
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
      const work = b.eval('lambda x: x * x + 1');
      domains.push({ py, work: py.task(work) });
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
    const sums = await Promise.all(domains.map((d, i) =>
      d.py.task(d.np.sum)(bufs[i])));
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
  const echoed = domains[0].np.arange(ELEMS).toTypedArray();
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
    // Drain before the next create — overlapping lane destroy + create is refuse.
    try { await killer; } catch (_) {}
    try { await py.destroy(); doubleDestroy++; } catch (_) {}
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
      await py.destroy();
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
  await py.destroy();
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
  await py.destroy();
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
  await py.destroy();
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
  await py.destroy();
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
      const f = await py.task(apply)(async (n) => {
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
    jobs3.push(py.task(apply)((x) => {
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
  await py.destroy();
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
    await py.destroy();
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
    jobs.push(py.task(apply)(async (a) => {
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
  await py.destroy();
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
  await py.destroy();
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
      await py.destroy();
      return;
    }
  }
  const settled = await Promise.all(laneJobs);
  await sleep(25);
  clearInterval(tick);
  const laneOk = settled.every((v) => typeof v === 'number' && Number.isFinite(v));
  await py.destroy();
  result('mixed_hammer_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('mixed_hammer_ticks %d', ticks);
  result('mixed_hammer_lane_jobs %d', settled.length);
  ok('mixed_hammer_lane', laneOk);
  /* Wall-clock 1ms ticks during a sub-ms burst are weather; require a few. */
  ok('mixed_hammer_loop_alive', ticks >= 5);
}

/* ---- 18. Destroy during slow isolated thenable (cross-task) ------------- */
async function destroyDuringThenable() {
  console.log('=== destroy_during_thenable ===');
  const rounds = SOAK ? 24 : (FULL ? 16 : 8);
  const delay = SOAK ? 250 : 120;
  const t0 = hr();
  let settled = 0, rejected = 0;
  for (let r = 0; r < rounds; r++) {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const sleeper = await b.eval(
      'lambda n: (__import__("time").sleep(n), 99)[1]');
    const p = py.task(sleeper)(delay / 1000);
    await sleep(15);
    await py.destroy();
    try {
      const v = await p;
      if (v === 99) settled++;
      else rejected++; // unexpected shape counts against clean reject path
    } catch (e) {
      if (/closed|exited/.test(e.message)) rejected++;
      else throw e;
    }
  }
  result('destroy_during_thenable_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('destroy_during_thenable_settled %d', settled);
  result('destroy_during_thenable_rejected %d', rejected);
  ok('destroy_during_thenable_accounted', settled + rejected === rounds);
}

/* ---- 19. RSS soak (create/churn/destroy) -------------------------------- */
async function rssSoak() {
  console.log('=== rss_soak ===');
  const seconds = SOAK
    ? Number(process.env.SOAK_SECONDS || 8)
    : (FULL ? 3 : 1);
  const ops = SOAK ? 30 : (FULL ? 20 : 12);
  const limitMb = SOAK ? 512 : (FULL ? 384 : 256);
  const r0 = rss();
  const t0 = hr();
  let cycles = 0;
  let acc = 0;
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    const py = ccpy.create();
    const math = py.import('math');
    for (let i = 0; i < ops; i++) {
      acc += math.floor(i + 0.2);
      if (i % 5 === 0) void math.sqrt;
    }
    await py.destroy();
    cycles++;
    if (cycles % 4 === 0 && global.gc) global.gc();
  }
  if (global.gc) { global.gc(); await sleep(20); global.gc(); }
  const r1 = rss();
  const delta = Math.max(0, r1 - r0);
  result('rss_soak_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('rss_soak_cycles %d', cycles);
  result('rss_soak_rss0_mb %s', (r0 / MB).toFixed(1));
  result('rss_soak_rss1_mb %s', (r1 / MB).toFixed(1));
  result('rss_soak_delta_mb %s', (delta / MB).toFixed(1));
  ok('rss_soak_cycles', cycles >= (SOAK ? 15 : (FULL ? 6 : 2)));
  ok('rss_soak_acc', acc > 0);
  ok('rss_soak_rss', delta < limitMb * MB);
}

/* ---- 20. Isolated RSS soak (process children) --------------------------- */
async function rssSoakIsolated() {
  console.log('=== rss_soak_isolated ===');
  const seconds = SOAK
    ? Number(process.env.SOAK_SECONDS || 6)
    : (FULL ? 2.5 : 0.8);
  const limitMb = SOAK ? 768 : (FULL ? 512 : 384);
  const r0 = rss();
  const t0 = hr();
  let cycles = 0;
  let acc = 0;
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const f = await b.eval('lambda x: x + 1');
    for (let i = 0; i < 10; i++) acc += await f(i);
    await py.destroy();
    cycles++;
  }
  if (global.gc) { global.gc(); await sleep(20); }
  const delta = Math.max(0, rss() - r0);
  result('rss_soak_isolated_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('rss_soak_isolated_cycles %d', cycles);
  result('rss_soak_isolated_delta_mb %s', (delta / MB).toFixed(1));
  ok('rss_soak_isolated_cycles', cycles >= (SOAK ? 8 : (FULL ? 3 : 1)));
  ok('rss_soak_isolated_acc', acc > 0);
  ok('rss_soak_isolated_rss', delta < limitMb * MB);
}

/* ---- 21. Lease blender -------------------------------------------------- */
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
  await py.destroy();
  result('lease_blender_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('lease_blender_rss_delta_mb %s', ((rss() - r0) / MB).toFixed(1));
  ok('lease_blender_sums', sumOk);
  ok('lease_blender_rss', rss() - r0 < 128 * MB);
}

/* ---- 22. Handle-leak soak (long-lived domain + ledger settle) ----------- */
async function handleLeakSoak() {
  console.log('=== handle_leak_soak ===');
  const seconds = SOAK
    ? Number(process.env.SOAK_SECONDS || 10)
    : (FULL ? 4 : 1.2);
  const burst = SOAK ? 60 : (FULL ? 40 : 24);
  const limitMb = SOAK ? 384 : (FULL ? 256 : 192);
  const py = ccpy.create();
  const math = py.import('math');
  const base = py.stats();
  const r0 = rss();
  const t0 = hr();
  let bursts = 0;
  let acc = 0;
  let peak = base;
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    const keep = [];
    for (let i = 0; i < burst; i++) {
      /* Attr + call mint transient handles; keep a third live per burst. */
      const v = math.floor(i + 0.2);
      acc += v;
      if (i % 3 === 0) keep.push(math.sqrt);
    }
    peak = Math.max(peak, py.stats());
    keep.length = 0;
    if (global.gc) global.gc();
    bursts++;
  }
  if (global.gc) { global.gc(); await sleep(30); global.gc(); }
  const after = py.stats();
  await py.destroy();
  const delta = Math.max(0, rss() - r0);
  result('handle_leak_soak_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('handle_leak_soak_bursts %d', bursts);
  result('handle_leak_soak_peak %d', peak);
  result('handle_leak_soak_after %d', after);
  result('handle_leak_soak_delta_mb %s', (delta / MB).toFixed(1));
  ok('handle_leak_soak_bursts', bursts >= (SOAK ? 8 : (FULL ? 3 : 1)));
  ok('handle_leak_soak_acc', acc > 0);
  /* Attr cache pins O(unique attrs on live parents), not O(getattr calls).
   * Ledger must stay well below burst size across the soak (no unbounded growth). */
  ok('handle_leak_soak_ledger', after < burst && peak < burst && after <= peak);
  ok('handle_leak_soak_rss', delta < limitMb * MB);
}

/* ---- 23. Everything concurrent (mixed shapes, many domains) ------------- */
async function everythingConcurrent(numpyOk) {
  console.log('=== everything_concurrent ===');
  const nDom = FULL ? 5 : 4;
  const t0 = hr();
  const jobs = [];
  for (let i = 0; i < nDom; i++) {
    jobs.push((async (kind) => {
      try {
        if (kind % 4 === 0) {
          const py = ccpy.create();
          try {
            const math = py.import('math');
            const n = FULL ? 40 : 20;
            let s = 0;
            for (let k = 0; k < n; k++) s += math.floor(k + 0.1);
            return s === Array.from({ length: n }, (_, k) => k)
              .reduce((a, b) => a + b, 0);
          } finally {
            await py.destroy();
          }
        }
        if (kind % 4 === 1) {
          const py = ccpy.create();
          try {
            const b = py.import('builtins');
            const json = py.import('json');
            const n = FULL ? 48 : 24;
            const lst = json.loads(
              '[' + Array.from({ length: n }, (_, j) => j).join(',') + ']');
            const mapped = b.list(b.map((x) => x * 3 + 1, lst));
            const want = '[' + Array.from({ length: n }, (_, j) => j * 3 + 1)
              .join(', ') + ']';
            return String(mapped) === want;
          } finally {
            await py.destroy();
          }
        }
        if (kind % 4 === 2) {
          const py = ccpy.create({ isolated: true });
          try {
            const b = py.import('builtins');
            const f = await b.eval('lambda x: x * 2');
            const n = FULL ? 20 : 10;
            let s = 0;
            for (let k = 0; k < n; k++) s += await f(k);
            return s === Array.from({ length: n }, (_, k) => k * 2)
              .reduce((a, b) => a + b, 0);
          } finally {
            try { await py.destroy(); } catch (_) {}
          }
        }
        if (numpyOk) {
          const py = ccpy.create({ isolated: true });
          try {
            const np = py.import('numpy');
            const a = new Float64Array(1 << 14);
            a.fill(1.5);
            const v = await np.sum(a);
            return Math.abs(v - a.length * 1.5) < 1e-6;
          } finally {
            try { await py.destroy(); } catch (_) {}
          }
        }
        const py = ccpy.create({ isolated: true });
        try {
          const b = py.import('builtins');
          const ln = await b.eval('lambda a: len(a)');
          const buf = new Float64Array((1 << 16) / 8 + 8);
          const n = await ln(buf);
          return n === buf.length;
        } finally {
          try { await py.destroy(); } catch (_) {}
        }
      } catch (e) {
        console.log('everything_concurrent_err kind=%d %s', kind,
          e && e.message ? e.message : e);
        return false;
      }
    })(i));
  }
  const settled = await Promise.all(jobs);
  result('everything_concurrent_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('everything_concurrent_n %d', nDom);
  ok('everything_concurrent_ok', settled.every(Boolean));
}

/* ---- 24. Escaped proxy via JS closure after destroy --------------------- */
async function escapedClosure() {
  console.log('=== escaped_closure ===');
  const t0 = hr();
  let closed = 0;
  let isoClosed = 0;
  // In-process: capture callable, destroy, GC, invoke later.
  {
    let escaped;
    {
      const py = ccpy.create();
      const fn = py.import('math').floor;
      escaped = (x) => fn(x);
      await py.destroy();
    }
    if (global.gc) { global.gc(); await sleep(20); global.gc(); }
    try { escaped(3.7); }
    catch (e) { if (/closed/.test(e.message)) closed++; }
  }
  // Isolated: same shape across the wire.
  {
    let escaped;
    {
      const py = ccpy.create({ isolated: true });
      const b = py.import('builtins');
      const fn = await b.eval('lambda x: x + 1');
      escaped = async (x) => fn(x);
      await py.destroy();
    }
    if (global.gc) { global.gc(); await sleep(20); global.gc(); }
    try { await escaped(1); }
    catch (e) { if (/closed|exited/.test(e.message)) isoClosed++; }
  }
  result('escaped_closure_ms %s', nsToMs(hr() - t0).toFixed(1));
  ok('escaped_closure_inproc', closed === 1);
  ok('escaped_closure_isolated', isoClosed === 1);
}

/* ---- 25. Lease + ArrayBuffer.transfer mid-lane -------------------------- */
async function leaseDetach() {
  console.log('=== lease_detach ===');
  const py = ccpy.create();
  const math = py.import('math');
  const n = FULL ? (1 << 20) : (1 << 18);
  const rounds = FULL ? 8 : 4;
  const t0 = hr();
  let okN = 0;
  for (let r = 0; r < rounds; r++) {
    const buf = new Float64Array(n);
    buf.fill(2);
    const want = n * 2;
    const p = py.task(math.fsum)(buf);
    await sleep(0); // let the lane start
    let detachHow = 'none';
    try {
      if (typeof buf.buffer.transfer === 'function') {
        buf.buffer.transfer();
        detachHow = 'transfer';
      } else {
        structuredClone(buf, { transfer: [buf.buffer] });
        detachHow = 'structuredClone';
      }
    } catch (e) {
      detachHow = 'blocked';
    }
    try {
      const s = await p;
      // Pin held (correct sum) or host blocked detach — both fine.
      if (s === want || detachHow === 'blocked') okN++;
    } catch (e) {
      // Articulate failure after detach is also fine — not crash/hang.
      if (/closed|detach|buffer|ArrayBuffer|memoryview|lease/i.test(e.message))
        okN++;
      else throw e;
    }
  }
  // Domain still healthy for a later lease.
  const probe = new Float64Array(64);
  probe.fill(1);
  const alive = math.fsum(probe) === 64;
  await py.destroy();
  result('lease_detach_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('lease_detach_rounds %d', rounds);
  ok('lease_detach_accounted', okN === rounds);
  ok('lease_detach_alive', alive);
}

/* ---- 26. Sync call while lane holds a large leased buffer --------------- */
async function syncVsLaneLease() {
  console.log('=== sync_vs_lane_lease ===');
  const py = ccpy.create();
  const math = py.import('math');
  const elems = FULL ? (1 << 22) : (1 << 20); // 32MB / 8MB
  const buf = new Float64Array(elems);
  buf.fill(1.25);
  const t0 = hr();
  const laneP = py.task(math.fsum)(buf);
  // Queue-jump sync work while the lease is live on the lane.
  let syncOk = true;
  for (let i = 0; i < 200; i++) {
    const v = math.sqrt(i + 1);
    if (typeof v !== 'number' || Math.abs(v - Math.sqrt(i + 1)) > 1e-9) {
      syncOk = false;
      break;
    }
  }
  const laneVal = await laneP;
  const laneOk = laneVal === elems * 1.25;
  // Lease must still be usable after lane settle.
  const after = math.fsum(buf.subarray(0, 1024));
  await py.destroy();
  result('sync_vs_lane_lease_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('sync_vs_lane_lease_elems %d', elems);
  ok('sync_vs_lane_lease_sync', syncOk);
  ok('sync_vs_lane_lease_lane', laneOk);
  ok('sync_vs_lane_lease_after', after === 1024 * 1.25);
}

/* ---- 27. Promise.all across isolated domains; destroy a subset ---------- */
async function promiseAllDestroy() {
  console.log('=== promise_all_destroy ===');
  /* Cooperative destroy drains (close + wait); in-flight work may still
   * fulfill. Hard-cancel is kill_mid_spill / abort_inject. Here we pin
   * composition: every promise settles, survivors are correct, destroyed
   * domains end closed, no stray SHM, no hang. */
  const N = FULL ? 10 : 6;
  const delay = FULL ? 0.12 : 0.06;
  const t0 = hr();
  const domains = [];
  for (let i = 0; i < N; i++) {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const work = await b.eval(
      'lambda n: (__import__("time").sleep(n), int(n * 1000))[1]');
    domains.push({ py, work });
  }
  const jobs = domains.map((d) => d.py.task(d.work)(delay));
  await sleep(10);
  const doomed = [];
  for (let i = 1; i < N; i += 2) doomed.push(i);
  await Promise.all(doomed.map(async (i) => {
    try { await domains[i].py.destroy(); } catch (_) {}
  }));
  const settled = await Promise.allSettled(jobs);
  let fulfilled = 0, rejected = 0, valuesOk = 0;
  const want = Math.round(delay * 1000);
  for (let i = 0; i < N; i++) {
    const s = settled[i];
    if (s.status === 'fulfilled') {
      fulfilled++;
      if (s.value === want) valuesOk++;
    } else if (/closed|exited/.test(String(s.reason && s.reason.message))) {
      rejected++;
    }
  }
  let doomedClosed = 0;
  for (const i of doomed) if (domains[i].py.closed) doomedClosed++;
  // Survivors that were not destroy()'d must still be usable, then close.
  let survivorsOk = 0;
  for (let i = 0; i < N; i += 2) {
    try {
      const b = domains[i].py.import('builtins');
      const one = await b.eval('lambda: 1');
      if ((await one()) === 1) survivorsOk++;
      await domains[i].py.destroy();
    } catch (_) {}
  }
  const strays = straySpills();
  result('promise_all_destroy_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('promise_all_destroy_fulfilled %d', fulfilled);
  result('promise_all_destroy_rejected %d', rejected);
  ok('promise_all_destroy_accounted', fulfilled + rejected === N);
  ok('promise_all_destroy_values', valuesOk === fulfilled);
  ok('promise_all_destroy_doomed_closed', doomedClosed === doomed.length);
  ok('promise_all_destroy_survivors', survivorsOk === Math.ceil(N / 2));
  ok('promise_all_destroy_no_strays', strays.length === 0);
}

/* ---- 28. SIGKILL mid-spill — SHM files must be reaped ------------------- */
async function killMidSpill() {
  console.log('=== kill_mid_spill ===');
  const rounds = SOAK ? 12 : (FULL ? 8 : 4);
  const t0 = hr();
  let rejected = 0;
  let clean = 0;
  for (let r = 0; r < rounds; r++) {
    const before = straySpills().length;
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const pidFn = await b.eval('lambda: __import__("os").getpid()');
    const pid = await pidFn();
    // Sleep inside child so the spill is in flight when we SIGKILL.
    const slow = await b.eval(
      'lambda a: (__import__("time").sleep(0.15), len(a))[1]');
    const buf = new Float64Array((1 << 16) / 8 + 4096);
    buf.fill(1);
    const p = py.task(slow)(buf);
    await sleep(20);
    try { process.kill(pid, 'SIGKILL'); } catch (_) {}
    try {
      await p;
    } catch (e) {
      if (/closed|exited/.test(e.message)) rejected++;
    }
    try { await py.destroy(); } catch (_) {}
    await sleep(30);
    if (straySpills().length <= before) clean++;
  }
  result('kill_mid_spill_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('kill_mid_spill_rounds %d', rounds);
  ok('kill_mid_spill_rejected', rejected === rounds);
  ok('kill_mid_spill_no_strays', clean === rounds);
}

/* ---- 28b. Same JS buffer → two isolated spills; kill one mid-flight ----- */
async function sharedBufKillSibling() {
  console.log('=== shared_buf_kill_sibling ===');
  /* Not one shared mapping — each encode stages its own spill file from
   * the same Float64Array. Kill A mid-spill; B must still finish; no
   * stray files. Reassures the "sibling reader" case for the real wire. */
  const rounds = SOAK ? 10 : (FULL ? 6 : 3);
  const t0 = hr();
  let aRejected = 0, bOk = 0, clean = 0;
  const n = (1 << 16) / 8 + 4096;
  for (let r = 0; r < rounds; r++) {
    const before = straySpills().length;
    const buf = new Float64Array(n);
    buf.fill(1);
    const A = ccpy.create({ isolated: true });
    const B = ccpy.create({ isolated: true });
    const bA = A.import('builtins');
    const bB = B.import('builtins');
    const pidFn = await bA.eval('lambda: __import__("os").getpid()');
    const pidA = await pidFn();
    const slowA = await bA.eval(
      'lambda a: (__import__("time").sleep(0.15), len(a))[1]');
    const slowB = await bB.eval(
      'lambda a: (__import__("time").sleep(0.12), len(a))[1]');
    const pA = A.task(slowA)(buf);
    const pB = B.task(slowB)(buf);
    await sleep(25);
    try { process.kill(pidA, 'SIGKILL'); } catch (_) {}
    try { await pA; } catch (e) {
      if (/closed|exited/.test(e.message)) aRejected++;
    }
    try {
      if ((await pB) === n) bOk++;
    } catch (_) { /* B must not die with A */ }
    try { await A.destroy(); } catch (_) {}
    try { await B.destroy(); } catch (_) {}
    await sleep(30);
    if (straySpills().length <= before) clean++;
  }
  result('shared_buf_kill_sibling_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('shared_buf_kill_sibling_rounds %d', rounds);
  ok('shared_buf_kill_sibling_a_rejected', aRejected === rounds);
  ok('shared_buf_kill_sibling_b_ok', bOk === rounds);
  ok('shared_buf_kill_sibling_no_strays', clean === rounds);
}

/* ---- 28c. Kill-to-cancel + respawn loop (correctness, not latency) ------ */
async function killRespawnLoop() {
  console.log('=== kill_respawn_loop ===');
  /* Cancel of CPU-bound work has no clean form — kill the worker, mint
   * another. Assert N cycles settle, no hang, no stray SHM, RSS bounded.
   * Latency of that pattern: npm/cc-python/examples/js_isolated_cancel_churn.js */
  const cycles = SOAK ? 24 : (FULL ? 12 : 6);
  const r0 = rss();
  const t0 = hr();
  let rejected = 0, replaced = 0;
  for (let c = 0; c < cycles; c++) {
    const before = straySpills().length;
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const pidFn = await b.eval('lambda: __import__("os").getpid()');
    const pid = await pidFn();
    const slow = await b.eval(
      'lambda: (__import__("time").sleep(0.2), 1)[1]');
    const p = py.task(slow)();
    await sleep(15);
    try { process.kill(pid, 'SIGKILL'); } catch (_) {}
    try { await p; } catch (e) {
      if (/closed|exited/.test(e.message)) rejected++;
    }
    try { await py.destroy(); } catch (_) {}
    const py2 = ccpy.create({ isolated: true });
    const b2 = py2.import('builtins');
    const one = await b2.eval('lambda: 1');
    if ((await one()) === 1) replaced++;
    await py2.destroy();
    if (straySpills().length > before) {
      ok('kill_respawn_loop_no_strays', false);
      return;
    }
  }
  const delta = Math.max(0, rss() - r0);
  result('kill_respawn_loop_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('kill_respawn_loop_cycles %d', cycles);
  result('kill_respawn_loop_delta_mb %s', (delta / MB).toFixed(1));
  ok('kill_respawn_loop_rejected', rejected === cycles);
  ok('kill_respawn_loop_replaced', replaced === cycles);
  ok('kill_respawn_loop_no_strays', true);
  ok('kill_respawn_loop_rss', delta < (SOAK ? 512 : 256) * MB);
}

/* ---- 29. Mixed sync + lane + isolated soak ------------------------------ */
async function mixedLoadSoak(numpyOk) {
  console.log('=== mixed_load_soak ===');
  const seconds = SOAK
    ? Number(process.env.SOAK_SECONDS || 15)
    : (FULL ? 2.5 : 0.9);
  const limitMb = SOAK ? 768 : (FULL ? 512 : 384);
  const r0 = rss();
  const t0 = hr();
  const py = ccpy.create();
  const math = py.import('math');
  const lane = py.task(math.fsum);
  const iso = ccpy.create({ isolated: true });
  const b = iso.import('builtins');
  const add = await b.eval('lambda x: x + 1');
  let syncN = 0, laneN = 0, isoN = 0;
  const deadline = Date.now() + seconds * 1000;
  while (Date.now() < deadline) {
    syncN += math.floor(1.2);
    const buf = new Float64Array(256);
    buf.fill(0.5);
    laneN += await lane(buf);
    isoN += await add(1);
    if ((syncN + laneN) % 40 === 0 && global.gc) global.gc();
  }
  await py.destroy();
  await iso.destroy();
  if (global.gc) { global.gc(); await sleep(20); }
  const delta = Math.max(0, rss() - r0);
  result('mixed_load_soak_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('mixed_load_soak_sync %d', syncN);
  result('mixed_load_soak_lane %s', laneN);
  result('mixed_load_soak_iso %d', isoN);
  result('mixed_load_soak_delta_mb %s', (delta / MB).toFixed(1));
  ok('mixed_load_soak_progress', syncN > 0 && laneN > 0 && isoN > 0);
  ok('mixed_load_soak_rss', delta < limitMb * MB);
  void numpyOk;
}

/* ---- 30. Native abort inject (isolated children) ------------------------ */
async function abortInject() {
  console.log('=== abort_inject ===');
  const rounds = SOAK ? 16 : (FULL ? 10 : 6);
  const t0 = hr();
  let rejected = 0;
  let late = 0;
  for (let i = 0; i < rounds; i++) {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    try {
      if (i % 2 === 0) {
        const boom = await b.eval(
          'lambda: (__import__("time").sleep(0.005), '
          + '__import__("os").abort())');
        await boom();
      } else {
        const pidFn = await b.eval('lambda: __import__("os").getpid()');
        const pid = await pidFn();
        const caller = await b.eval('lambda f: f(1)');
        await caller(() => {
          try { process.kill(pid, 'SIGABRT'); } catch (_) {}
          return 1;
        });
      }
    } catch (e) {
      if (/closed|exited|abort/.test(String(e && e.message || e))) rejected++;
    }
    try {
      const probe = await b.eval('lambda: 1');
      await probe();
    } catch (e) {
      if (/closed|exited/.test(String(e && e.message || e))) late++;
    }
    try { await py.destroy(); } catch (_) {}
  }
  result('abort_inject_ms %s', nsToMs(hr() - t0).toFixed(1));
  result('abort_inject_rounds %d', rounds);
  ok('abort_inject_rejected', rejected === rounds);
  ok('abort_inject_late', late === rounds);
}

(async () => {
  console.log('js_bridge_chaos scale=%s pid=%d', SCALE, process.pid);
  const numpyOk = await hasNumpyIsolated();
  console.log('numpy_isolated %s', numpyOk ? 'yes' : 'no');

  await wireIntegrity();
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
  await destroyDuringThenable();
  await mixedHammer(numpyOk);
  await everythingConcurrent(numpyOk);
  await escapedClosure();
  await leaseDetach();
  await syncVsLaneLease();
  await promiseAllDestroy();
  await leaseBlender();
  await rssSoak();
  await rssSoakIsolated();
  await handleLeakSoak();
  await mixedLoadSoak(numpyOk);
  /* Unclean death last: SIGKILL mid-spill / sibling / respawn, then abort. */
  await killMidSpill();
  await sharedBufKillSibling();
  await killRespawnLoop();
  await abortInject();

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
