/* Async rungs for the cc-python bridge's ONE async primitive:
 * py.task(callable) binds it to the domain's latent executor lane and
 * returns an async function.  Run with --expose-gc (the paired smoke
 * does); deterministic booleans, load-tolerant timing bounds. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  // 1. One primitive, three uses: held task, inline one-shot, pipelining
  //    — while the same domain keeps answering synchronously.
  {
    const py = ccpy.create();
    const math = py.import('math');
    out('sync_still_sync', math.sqrt(16) === 4);
    const sqrt = py.task(math.sqrt);
    out('await_scalar', (await sqrt(25)) === 5);
    out('one_shot', (await py.task(math.hypot)(3, 4)) === 5);
    const [a, b, c] = await Promise.all([sqrt(4), sqrt(9), sqrt(36)]);
    out('pipelined', a === 2 && b === 3 && c === 6);
    out('mixed_interleave', math.floor(7.9) === 7); // sync between awaits
    let rejected = false;
    try { await sqrt(-1); }
    catch (e) { rejected = /math domain error/.test(e.message); }
    out('python_error_rejects', rejected);
    // A handle minted sync crosses into an async call on the same domain.
    const builtins = py.import('builtins');
    const attrs = builtins.dir(math); // held list handle, sync
    out('handle_crosses_flavors', (await py.task(builtins.len)(attrs)) > 10);
    // The reserved recorded-batch form names itself.
    let reserved = false;
    try { py.task(() => math.sqrt(4)); }
    catch (e) { reserved = /not implemented yet/.test(e.message); }
    out('closure_form_reserved', reserved);
    await py.destroy();
    out('destroy_resolves', py.closed);
    let closedErr = false;
    try { await sqrt(4); }
    catch (e) { closedErr = /closed/.test(e.message); }
    out('after_close_rejects', closedErr);
    await py.destroy();
    out('double_destroy_resolves', true);
  }

  // 2. destroy() is a Promise even for a domain whose lane never started.
  {
    const py = ccpy.create();
    out('laneless_math', py.import('math').floor(2.5) === 2);
    const p = py.destroy();
    out('laneless_destroy_promise', typeof p.then === 'function');
    await p;
    out('laneless_destroy_resolves', py.closed);
  }

  // 3. The loop stays live while Python runs off-thread; two domains
  //    parallelize under per-interpreter GILs; one lane completes FIFO.
  {
    const A = ccpy.create();
    const B = ccpy.create();
    const sa = A.task(A.import('time').sleep);
    const sb = B.task(B.import('time').sleep);
    let ticked = false;
    setTimeout(() => { ticked = true; }, 100);
    await sa(0.3);
    out('loop_stays_live', ticked);
    const t1 = Date.now();
    await Promise.all([sa(0.3), sb(0.3)]);
    out('parallel_domains', Date.now() - t1 < 480);
    const order = [];
    await Promise.all([
      sa(0.05).then(() => order.push(1)),
      sa(0.01).then(() => order.push(2)),
    ]);
    out('fifo_in_domain', order.join(',') === '1,2');
    await Promise.all([A.destroy(), B.destroy()]);
    out('both_closed', A.closed && B.closed);
  }

  // 4. The lease spans the job: a typed array submitted then dropped is
  //    pinned by the job's napi_ref through GC pressure.
  {
    const py = ccpy.create();
    const fsum = py.task(py.import('math').fsum);
    let p;
    {
      let buf = new Float64Array(1 << 18);
      buf.fill(0.25);
      p = fsum(buf);
      buf = null;
    }
    global.gc();
    await sleep(5);
    global.gc();
    out('inflight_lease_pinned', (await p) === (1 << 18) * 0.25);
    await py.destroy();
  }

  // 5. Revoke-then-drain: destroy mid-queue lets the in-flight call
  //    finish and rejects everything queued behind it.
  {
    const py = ccpy.create();
    const zzz = py.task(py.import('time').sleep);
    const inflight = zzz(0.2);
    // Outcome handlers attach at creation: the rejections settle while
    // the in-flight await is still pending, and a rejection that crosses
    // a turn unhandled is fatal to the process.
    const queued = [zzz(0.1), zzz(0.1), zzz(0.1)].map((p) =>
      p.then(() => 'resolved',
             (e) => (/closed/.test(e.message) ? 'rejected' : 'other')));
    const done = py.destroy();
    out('inflight_completes', (await inflight) === undefined);
    const outcomes = await Promise.all(queued);
    out('queued_reject', outcomes.join(',') === 'rejected,rejected,rejected');
    await done;
    out('drain_resolves', py.closed);
  }

  // 6. A dropped domain with a started lane (never destroyed) drains via
  //    its GC finalizer without wedging the process — proven by exit.
  {
    await (async () => {
      const py = ccpy.create();
      await py.task(py.import('math').sqrt)(4);
    })();
    global.gc();
    await sleep(30);
    out('gc_drain_survives', true);
  }

  console.log('async suite done');
})();
