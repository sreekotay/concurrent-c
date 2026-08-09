/* Stage-2, the asyncio lane: a call that returns a coroutine becomes a
 * loop task (engaged lazily — the sync FIFO path is untouched until the
 * first coroutine); tasks interleave; a JS callback awaited from inside
 * a task returns an awaitable future, and only that task suspends.
 * Exceptions keep "Type: message" in BOTH directions and across double
 * crossings; revocation cancels pending tasks articulately.
 * Deterministic booleans; the paired smoke pins them. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  const py = ccpy.create();
  const b = py.import('builtins');
  const math = py.import('math');
  const ns = b.dict();
  b.exec(
      'import asyncio\n' +
      'async def work(tag, n):\n' +
      '    await asyncio.sleep(n)\n' +
      '    return tag\n' +
      'async def fetch2(cb, x):\n' +
      '    v = await cb(x)\n' +
      '    return v * 2\n' +
      'async def ticker(n):\n' +
      '    for _ in range(n):\n' +
      '        await asyncio.sleep(0.005)\n' +
      '    return "ticked"\n' +
      'async def boom():\n' +
      '    raise ValueError("bad input 42")\n' +
      'async def guarded(cb):\n' +
      '    try:\n' +
      '        return await cb()\n' +
      '    except Exception as e:\n' +
      '        return "caught " + type(e).__name__ + ": " + str(e)\n' +
      'async def unguarded(cb):\n' +
      '    return await cb()\n' +
      'async def forever():\n' +
      '    await asyncio.sleep(3600)\n', ns);
  const work = ns.get('work');

  // 1. A coroutine result completes the task's promise.
  out('coro_resolves', String(await py.task(work)('one', 0.001)) === 'one');

  // 2. Tasks INTERLEAVE: two staggered sleeps run in max, not sum —
  //    measured relatively (serial first) so suite load cannot flake it.
  {
    const t0 = Date.now();
    await py.task(work)('s1', 0.08);
    await py.task(work)('s2', 0.08);
    const serial = Date.now() - t0;
    const t1 = Date.now();
    await Promise.all([py.task(work)('p1', 0.08), py.task(work)('p2', 0.08)]);
    out('tasks_interleave', Date.now() - t1 < serial * 0.8);
  }

  // 3. Completion order follows readiness, not submission: the shorter
  //    sleep submitted SECOND finishes first.
  {
    const order = [];
    await Promise.all([
      py.task(work)('slow', 0.08).then((v) => order.push(String(v))),
      py.task(work)('fast', 0.01).then((v) => order.push(String(v))),
    ]);
    out('readiness_order', order.join(',') === 'fast,slow');
  }

  // 4. `await cb(x)`: a JS async function is an awaitable from Python —
  //    only the awaiting task suspends; a sibling task keeps running.
  {
    const tick = py.task(ns.get('ticker'))(6); // alive DURING the await
    const r = await py.task(ns.get('fetch2'))(async (x) => {
      await sleep(30);
      return x + 1;
    }, 20);
    out('awaited_js_callback', r === 42);
    out('loop_live_during_await', String(await tick) === 'ticked');
  }

  // 5. Same-domain composition: the awaited callback leans on ANOTHER
  //    task of this domain — it pumps on the same loop, no deadlock.
  {
    const r = await py.task(ns.get('fetch2'))(async (x) => {
      return await py.task(math.sqrt)(x);
    }, 16);
    out('same_domain_compose', r === 8);
  }

  // 6. Python exception -> JS rejection keeps "Type: message".
  {
    let msg = '';
    try { await py.task(ns.get('boom'))(); }
    catch (e) { msg = e.message; }
    out('exc_type_and_message', /ValueError: bad input 42/.test(msg));
  }

  // 7. JS rejection -> Python exception AT THE AWAIT, catchable, with
  //    the reason's text.
  {
    const g = await py.task(ns.get('guarded'))(async () => {
      throw new Error('nope503');
    });
    out('rejection_caught_in_python',
        /caught RuntimeError: .*nope503/.test(String(g)));
  }

  // 8. Uncaught, it crosses BACK: JS reject -> Python raise -> JS
  //    rejection, text intact through both crossings.
  {
    let msg = '';
    try {
      await py.task(ns.get('unguarded'))(async () => {
        throw new Error('lost twice 7');
      });
    } catch (e) { msg = e.message; }
    out('exc_double_crossing', /lost twice 7/.test(msg));
  }

  // 9. Triple hop: coroutine awaits a callback whose promise is another
  //    task that ITSELF raises in Python — ValueError text survives
  //    Python -> JS -> Python -> JS.
  {
    let msg = '';
    try {
      await py.task(ns.get('unguarded'))(() => py.task(ns.get('boom'))());
    } catch (e) { msg = e.message; }
    out('exc_triple_hop', /ValueError: bad input 42/.test(msg));
  }

  // 10. Loop mode did not eat the sync shapes: plain sync callables via
  //     task, and a sync callable whose callback returns a Promise
  //     (stage-1 blocking suspension) both still work.
  {
    out('sync_task_in_loop_mode', (await py.task(math.floor)(7.9)) === 7);
    const p5 = b.eval('lambda cb: cb() + 5', b.dict());
    const r = await py.task(p5)(async () => {
      await sleep(10);
      return 37;
    });
    out('sync_suspension_in_loop_mode', r === 42);
  }

  // 11. Revocation with a pending task: destroy cancels it, the task's
  //     promise answers closed, destroy resolves, the door closes.
  {
    const py2 = ccpy.create();
    const b2 = py2.import('builtins');
    const ns2 = b2.dict();
    b2.exec('import asyncio\n' +
            'async def forever():\n' +
            '    await asyncio.sleep(3600)\n', ns2);
    const hung = py2.task(ns2.get('forever'))();
    const got = hung.then(() => 'resolved', (e) =>
        (/closed/.test(e.message) ? 'rejected' : 'other'));
    await sleep(50); // the task is genuinely suspended in asyncio.sleep
    await py2.destroy();
    out('pending_task_cancelled', (await got) === 'rejected');
    out('destroy_lands', py2.closed);
  }

  await py.destroy();
  out('main_closed', py.closed);
  console.log('aio suite done');
})().catch((e) => { console.error('AIO SUITE ERROR:', e.message); process.exit(1); });
