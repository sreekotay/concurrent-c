/* Async-mode rungs for the cc-python bridge — run with --expose-gc (the
 * paired smoke does).  Deterministic booleans; timing bounds are loose
 * enough for a loaded CI box. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  // 1. The awaited surface: scalars, pipelining, exceptions as
  //    rejections, sync attribute reads interleaved between awaits.
  {
    const py = ccpy.create({ mode: 'async' });
    const math = py.import('math');
    out('sync_attr', math.pi === Math.PI);
    out('await_scalar', (await math.sqrt(16)) === 4);
    const [a, b, c] =
      await Promise.all([math.sqrt(4), math.sqrt(9), math.sqrt(25)]);
    out('pipelined', a === 2 && b === 3 && c === 5);
    let rejected = false;
    try { await math.sqrt(-1); }
    catch (e) { rejected = /math domain error/.test(e.message); }
    out('python_error_rejects', rejected);
    await py.destroy();
    out('destroy_resolves', py.closed);
    let closedErr = false;
    try { await math.sqrt(4); }
    catch (e) { closedErr = /closed/.test(e.message); }
    out('after_close_rejects', closedErr);
    let again = true;
    await py.destroy(); /* second drain resolves immediately */
    out('double_destroy_resolves', again);
  }

  // 2. The event loop stays live while Python runs off-thread, two
  //    domains parallelize under per-interpreter GILs, and one domain
  //    completes FIFO.
  {
    const A = ccpy.create({ mode: 'async' });
    const B = ccpy.create({ mode: 'async' });
    const ta = A.import('time');
    const tb = B.import('time');
    let ticked = false;
    setTimeout(() => { ticked = true; }, 100);
    await ta.sleep(0.3);
    out('loop_stays_live', ticked);
    const t1 = Date.now();
    await Promise.all([ta.sleep(0.3), tb.sleep(0.3)]);
    out('parallel_domains', Date.now() - t1 < 480);
    const order = [];
    await Promise.all([
      ta.sleep(0.05).then(() => order.push(1)),
      ta.sleep(0.01).then(() => order.push(2)),
    ]);
    out('fifo_in_domain', order.join(',') === '1,2');
    await Promise.all([A.destroy(), B.destroy()]);
    out('both_closed', A.closed && B.closed);
  }

  // 3. The lease spans the job: a typed array submitted then dropped is
  //    pinned by the job's napi_ref through GC pressure.
  {
    const py = ccpy.create({ mode: 'async' });
    const math = py.import('math');
    let p;
    {
      let buf = new Float64Array(1 << 18);
      buf.fill(0.25);
      p = math.fsum(buf);
      buf = null;
    }
    global.gc();
    await sleep(5);
    global.gc();
    out('inflight_lease_pinned', (await p) === (1 << 18) * 0.25);
    await py.destroy();
  }

  // 4. Revoke-then-drain: destroy mid-queue lets the in-flight call
  //    finish and rejects everything queued behind it.
  {
    const py = ccpy.create({ mode: 'async' });
    const tm = py.import('time');
    const inflight = tm.sleep(0.2);
    // Outcome handlers attach at creation: the rejections settle while
    // the in-flight await is still pending, and a rejection that crosses
    // a turn unhandled is fatal to the process.
    const queued = [tm.sleep(0.1), tm.sleep(0.1), tm.sleep(0.1)].map((p) =>
      p.then(() => 'resolved',
             (e) => (/closed/.test(e.message) ? 'rejected' : 'other')));
    const done = py.destroy();
    out('inflight_completes', (await inflight) === undefined);
    const outcomes = await Promise.all(queued);
    out('queued_reject', outcomes.join(',') === 'rejected,rejected,rejected');
    await done;
    out('drain_resolves', py.closed);
  }

  // 5. A dropped async bridge (never destroyed) drains via its GC
  //    finalizer without wedging the process — proven by clean exit.
  {
    (() => { void ccpy.create({ mode: 'async' }); })();
    global.gc();
    await sleep(30);
    out('gc_drain_survives', true);
  }

  console.log('async suite done');
})();
