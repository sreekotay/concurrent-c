/* Adversarial lifetime/memory suite for the cc-python bridge — run with
 * --expose-gc (the paired smoke does).  Every rung attacks the Isolation
 * Domain teardown or the lease boundary; output is deterministic booleans
 * so the smoke can pin it. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const gcNow = async () => {
  global.gc();
  await sleep(10);
  global.gc();
  await sleep(10);
};
const rss = () => process.memoryUsage().rss;
const MB = 1024 * 1024;

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  // 1. Explicit handle churn: mint + release 20k handles; the domain's
  //    ledger and the process both end where they started.
  {
    const py = ccpy.create();
    const math = py.import('math');
    py.release(math.sqrt); // warm every path once
    const base = py.stats();
    const r0 = rss();
    for (let i = 0; i < 20000; i++) py.release(math.sqrt);
    out('churn_stats_flat', py.stats() === base);
    out('churn_rss_flat', rss() - r0 < 48 * MB);
    py.destroy();
  }

  // 1b. Float scalar materialize must not leave durable probe diagnostics
  //     in the domain arena.  Previously as_i64→TypeError→cc__py_err
  //     arena-copied ~100 B per successful float return.  Acceptance:
  //     1e6 float returns, handle ledger delta 0, RSS plateaus.
  {
    const py = ccpy.create();
    const b = py.import('builtins');
    b.exec('def rf(x):\n  return x + 0.5\n');
    const rf = b.eval('rf');
    const math = py.import('math');
    /* Warm paths; pin the few long-lived proxies we keep. */
    for (let i = 0; i < 1000; i++) {
      rf(i);
      math.sqrt(i + 1);
    }
    await gcNow();
    const h0 = py.stats();
    const samples = [];
    samples.push(rss());
    const N = 1000000;
    const step = 250000;
    for (let i = 0; i < N; i++) {
      rf(i);
      if ((i + 1) % step === 0) samples.push(rss());
    }
    const h1 = py.stats();
    out('float_return_handles_flat', h1 === h0);
    const grow = samples[samples.length - 1] - samples[0];
    /* Absolute bound (was ~100 MB / 1e6 before the fix). */
    out('float_return_rss_bounded', grow < 32 * MB);
    /* Plateau: second half must not keep climbing like the old leak. */
    const mid = samples[Math.floor(samples.length / 2)];
    const end = samples[samples.length - 1];
    out('float_return_rss_plateau', end - mid < 16 * MB);

    const h2 = py.stats();
    const rSqrt0 = rss();
    for (let i = 0; i < N; i++) math.sqrt(i + 1);
    out('float_sqrt_handles_flat', py.stats() === h2);
    out('float_sqrt_rss_bounded', rss() - rSqrt0 < 32 * MB);
    py.destroy();
  }

  // 2. GC-only churn: mint thousands of proxies, drop them, and let the
  //    finalizers do the releasing.
  {
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
    py.destroy();
  }

  // 3. Domain churn: 100 create/compute/destroy cycles; nothing
  //    accumulates across domains.
  {
    const r0 = rss();
    let compute = true;
    for (let i = 0; i < 100; i++) {
      const py = ccpy.create();
      const m = py.import('math');
      compute = compute && m.floor(9.9) === 9;
      py.destroy();
    }
    await gcNow();
    out('domain_churn_compute', compute);
    out('domain_churn_rss', rss() - r0 < 64 * MB);
  }

  // 4. Finalizer storm after close: destroy a domain out from under 2000
  //    live proxies, then let the GC run their (now orphaned) finalizers.
  {
    const py = ccpy.create();
    const math = py.import('math');
    let keep = [];
    for (let i = 0; i < 2000; i++) keep.push(math.sqrt);
    py.destroy();
    let closedErr = false;
    try {
      keep[0](4);
    } catch (e) {
      closedErr = /closed/.test(e.message);
    }
    out('storm_use_after_close', closedErr);
    keep = null;
    await gcNow();
    await gcNow();
    out('storm_survived_gc', true);
  }

  // 5. The proxy graph pins the domain: a surviving proxy keeps its
  //    bridge alive across GC and still computes; dropping the last
  //    proxy lets the whole graph collect.
  {
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
  }

  // 5b. Whole-graph death with 500 live handles: the domain and object
  //     finalizers run in whatever order the GC picks, and the arena —
  //     which owns every box — must outlive the last of them.
  {
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
  }

  // 6. Repeated big leases: a 8MB Float64Array crosses as a zero-copy
  //    view ten times; the sum is exact and the process does not grow by
  //    ten copies.
  {
    const py = ccpy.create();
    const math = py.import('math');
    const buf = new Float64Array(1 << 20);
    buf.fill(0.5);
    const r0 = rss();
    let s = 0;
    for (let i = 0; i < 10; i++) s = math.fsum(buf);
    out('lease_sum', s === buf.length * 0.5);
    out('lease_rss_flat', rss() - r0 < 48 * MB);
    py.destroy();
  }

  // 6b. Keep-past-return: a callee that stashes the memoryview is caught
  //     at the lease boundary; the domain stays healthy for later leases.
  {
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
    const keepT = py.task(keep);
    let laneCaught = false;
    try { await keepT(buf); } catch (e) {
      laneCaught = /retained by the callee|borrow ends/.test(e.message);
    }
    out('keep_past_return_lane', laneCaught);
    await py.destroy();
  }

  // 6c. Create while a lane drain is in flight must refuse loudly — not
  //     SEGV.  destroy() was kicked without await; create answers; then
  //     the outstanding destroy settles and a later create works.
  {
    const py = ccpy.create();
    const math = py.import('math');
    await py.task(math.floor)(1.2);
    const draining = py.destroy();
    let refused = false;
    try { ccpy.create(); }
    catch (e) { refused = /destroy is still in progress|await the outstanding destroy/.test(e.message); }
    out('create_during_destroy', refused);
    await draining;
    const ok = ccpy.create();
    out('create_after_destroy_ok', ok.import('math').floor(2.8) === 2);
    await ok.destroy();
  }

  // 7. Released-handle abuse: as receiver, as argument, and re-released
  //    — three articulate errors, no UB.
  {
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
  }

  // 8–9 need a second in-process interpreter (CPython 3.12+).  On 3.11
  // create() refuses articulately — skip rather than fail the suite.
  let multiInterp = false;
  {
    const probe = ccpy.create();
    try {
      const b = ccpy.create();
      multiInterp = true;
      await b.destroy();
    } catch (e) {
      if (!/second interpreter|Py_NewInterpreterFromConfig/.test(e.message))
        throw e;
    }
    await probe.destroy();
  }

  // 8. Cross-domain argument: domain B rejects a handle minted by A at
  //    the argument door, not just the receiver door.
  if (multiInterp) {
    const A = ccpy.create();
    const B = ccpy.create();
    const bm = B.import('math');
    let rejected = false;
    try {
      bm.hypot(A.import('math').floor, 3);
    } catch (e) {
      rejected = /another bridge/.test(e.message);
    }
    out('cross_domain_arg', rejected);
    await A.destroy();
    await B.destroy();
  } else {
    console.log('cross_domain_arg skipped (needs Py_NewInterpreterFromConfig / CPython 3.12+)');
  }

  // 9. Twenty interleaved domains; destroying half must not disturb the
  //    other half.
  if (multiInterp) {
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
  } else {
    console.log('interleaved_survivors skipped (needs Py_NewInterpreterFromConfig / CPython 3.12+)');
    console.log('interleaved_closed skipped (needs Py_NewInterpreterFromConfig / CPython 3.12+)');
  }

  // 10. Deep-chain intermediates: every step of os.path.join mints a
  //     handle; two thousand rounds must not accrete.
  {
    const py = ccpy.create();
    const os = py.import('os');
    out('chain_result', os.path.join('a', 'b') === 'a/b');
    const base = py.stats();
    for (let i = 0; i < 2000; i++) void os.path.join('a', 'b');
    await gcNow();
    await gcNow();
    out('chain_stats_bounded', py.stats() < base + 100);
    py.destroy();
  }

  // 11. Escaped proxy via JS closure: destroy domain, GC, invoke later.
  {
    let escaped;
    {
      const py = ccpy.create();
      const fn = py.import('math').floor;
      escaped = (x) => fn(x);
      await py.destroy();
    }
    await gcNow();
    let closed = false;
    try { escaped(3.7); } catch (e) { closed = /closed/.test(e.message); }
    out('escaped_closure_closed', closed);
  }

  // 12. Lease + ArrayBuffer.transfer while a lane call holds the buffer.
  {
    const py = ccpy.create();
    const math = py.import('math');
    const buf = new Float64Array(1 << 18);
    buf.fill(2);
    const want = buf.length * 2;
    const p = py.task(math.fsum)(buf);
    await sleep(0);
    let detachHow = 'none';
    try {
      if (typeof buf.buffer.transfer === 'function') {
        buf.buffer.transfer();
        detachHow = 'transfer';
      } else {
        structuredClone(buf, { transfer: [buf.buffer] });
        detachHow = 'structuredClone';
      }
    } catch (_) {
      detachHow = 'blocked';
    }
    let accounted = false;
    try {
      const s = await p;
      accounted = s === want || detachHow === 'blocked';
    } catch (e) {
      accounted = /closed|detach|buffer|ArrayBuffer|memoryview|lease/i
        .test(e.message);
    }
    out('lease_detach_mid_lane', accounted);
    const probe = new Float64Array(32);
    probe.fill(1);
    out('lease_detach_alive', math.fsum(probe) === 32);
    await py.destroy();
  }

  console.log('mem suite done');
})();
