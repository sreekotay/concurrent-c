/* Stage-2, the asyncio lane: async def through py.task becomes a real
 * asyncio task — tasks interleave, `await cb(x)` treats a JS Promise as
 * an awaitable (only that task suspends), and exception text survives
 * every crossing.
 *
 *   node npm/cc-python/examples/asyncio_lane.js          (from repo root)
 */
'use strict';

const ccpy = require(process.cwd() + '/npm/cc-python');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const py = ccpy.create();
  const b = py.import('builtins');
  const ns = b.dict();
  b.exec('import asyncio\n' +
         'async def crawl(fetch, urls):\n' +
         '    return " ".join(await asyncio.gather(\n' +
         '        *(fetch(u) for u in urls.split(","))))\n' +
         'async def work(tag, n):\n' +
         '    await asyncio.sleep(n)\n' +
         '    return tag\n' +
         'async def guarded(cb):\n' +
         '    try:\n' +
         '        return await cb()\n' +
         '    except Exception as e:\n' +
         '        return "caught " + type(e).__name__ + ": " + str(e)\n', ns);

  // Tasks interleave: two 150ms sleeps run in ~max, not sum.
  {
    const t0 = Date.now();
    const [a, c] = await Promise.all([
      py.task(ns.get('work'))('a', 0.15),
      py.task(ns.get('work'))('b', 0.15),
    ]);
    console.log('interleaved:', String(a), String(c),
                'in', Date.now() - t0, 'ms');
  }

  // asyncio.gather over JS fetches: each await suspends only its task.
  {
    const r = await py.task(ns.get('crawl'))(async (u) => {
      await sleep(20);
      return u.toUpperCase();
    }, 'alpha,beta,gamma');
    console.log('crawl      :', String(r));
  }

  // A JS rejection raises at the Python await — catchable there.
  {
    const g = await py.task(ns.get('guarded'))(async () => {
      throw new Error('service unavailable');
    });
    console.log('caught     :', String(g));
  }

  await py.destroy();
})().catch((e) => { console.error('ERR', e.message); process.exit(1); });
