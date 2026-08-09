/* Stage-1 promise suspension: a lane-side callback may return a Promise.
 * Python blocks at cb() — GIL released — until it settles; while the
 * request is suspended the executor services its own queue, so a promise
 * that depends on a task of the SAME domain completes instead of
 * deadlocking.  Rejections become Python exceptions at the cb() call
 * site.  Sync bridge calls still refuse thenables, articulately.
 * Deterministic booleans; the paired smoke pins them. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  const py = ccpy.create();
  const b = py.import('builtins');
  const math = py.import('math');
  // Python-side helpers that CALL a JS callback and use its result — the
  // round trip the suspension has to survive.
  const call2 = b.eval('lambda cb, x: cb(x) * 2', b.dict());
  const plus5 = b.eval('lambda cb: cb() + 5', b.dict());

  // 1. The fetch shape: the callback awaits real async work (a timer),
  //    the settled value flows back into the Python expression.
  {
    const r = await py.task(call2)(async (x) => {
      await sleep(5);
      return x + 1;
    }, 20);
    out('awaited_callback', r === 42);
  }

  // 2. Resolution kinds cross like plain returns: string, bool, and
  //    undefined (None to Python).
  {
    const s = await py.task(b.str)(); // warm the lane
    void s;
    const up = b.eval('lambda cb: cb().upper()', b.dict());
    out('string_resolution',
        (await py.task(up)(async () => 'abc')) === 'ABC');
    const notf = b.eval('lambda cb: not cb()', b.dict());
    out('bool_resolution', (await py.task(notf)(async () => false)) === true);
    const isnone = b.eval('lambda cb: cb() is None', b.dict());
    out('undefined_resolution',
        (await py.task(isnone)(async () => undefined)) === true);
  }

  // 3. THE deadlock-dissolver: the callback's promise needs a task on
  //    its own domain.  The outer job is suspended; the inner job runs
  //    nested on the same executor thread, its completion settles the
  //    promise on main, main resumes the executor.
  {
    const r = await py.task(call2)(async (x) => {
      return await py.task(math.sqrt)(x); // same domain, same lane
    }, 16);
    out('nested_own_domain', r === 8); // sqrt(16) * 2
  }

  // 4. Two suspensions deep: the nested task's OWN callback also returns
  //    a promise — LIFO unwind.
  {
    const r = await py.task(call2)(async (x) => {
      return await py.task(plus5)(async () => {
        await sleep(2);
        return x;
      });
    }, 10);
    out('nested_two_deep', r === 30); // (10 + 5) * 2
  }

  // 5. Rejection becomes a Python exception at cb(), and surfaces as the
  //    task's rejection with the reason's text.
  {
    let msg = '';
    try {
      await py.task(call2)(async () => { throw new Error('nope42'); }, 1);
    } catch (e) { msg = e.message; }
    out('rejection_to_python', /nope42/.test(msg));
  }

  // 6. Python can CATCH the rejection — it is an ordinary exception.
  {
    const ns = b.dict();
    b.exec('def guarded(cb):\n' +
           '    try:\n' +
           '        return cb()\n' +
           '    except Exception as e:\n' +
           '        return "caught:" + type(e).__name__\n', ns);
    const g = ns.get('guarded');
    const r = await py.task(g)(async () => { throw new Error('boom'); });
    out('python_catches_rejection', /^caught:/.test(String(r)));
  }

  // 7. Pipelined tasks that each suspend: FIFO submits, each serviced
  //    (possibly nested under the previous suspension), all settle.
  {
    const mk = (v, ms) => py.task(plus5)(async () => {
      await sleep(ms);
      return v;
    });
    const rs = await Promise.all([mk(10, 8), mk(20, 3), mk(30, 1)]);
    out('pipelined_suspensions',
        rs[0] === 15 && rs[1] === 25 && rs[2] === 35);
  }

  // 8. The sync door still refuses thenables — with the task pointer.
  {
    let refused = false;
    try { call2(async (x) => x, 3); }
    catch (e) { refused = /py\.task/.test(e.message); }
    out('sync_refusal_names_task', refused);
  }

  // 9. Suspend-then-destroy: revocation lands mid-suspension.  The
  //    in-flight job finishes when its promise settles; a task submitted
  //    after destroy answers closed; the close promise resolves.
  {
    const py2 = ccpy.create();
    const b2 = py2.import('builtins');
    const p5 = b2.eval('lambda cb: cb() + 5', b2.dict());
    let settle;
    const gate = new Promise((r) => { settle = r; });
    const inflight = py2.task(p5)(() => gate); // suspends on the gate
    await sleep(30); // the callback has posted and suspended
    const closing = py2.destroy(); // revoke while suspended
    let lateClosed = false;
    try { await py2.task(p5)(() => 1); }
    catch (e) { lateClosed = /closed/.test(e.message); }
    settle(37);
    const r = await inflight;
    await closing;
    out('suspended_job_completes', r === 42);
    out('late_task_refused', lateClosed);
    out('destroy_lands', py2.closed);
  }

  await py.destroy();
  out('main_closed', py.closed);
  console.log('await suite done');
})().catch((e) => { console.error('AWAIT SUITE ERROR:', e.message); process.exit(1); });
