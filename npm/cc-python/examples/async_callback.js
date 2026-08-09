/* Stage-1 promise suspension: ordinary synchronous Python calls a JS
 * async function and blocks — GIL released, event loop live — until the
 * promise settles.  The callback may even lean on tasks of the same
 * domain (nested servicing).
 *
 *   node npm/cc-python/examples/async_callback.js        (from repo root)
 */
'use strict';

const ccpy = require(process.cwd() + '/npm/cc-python');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const py = ccpy.create();
  const b = py.import('builtins');
  const np = py.import('numpy');

  // A Python function that CALLS a JS callback and post-processes the
  // result — plain synchronous Python, no asyncio, no annotations.
  const ns = b.dict();
  b.exec('def score(fetch, user):\n' +
         '    row = fetch(user)   # blocks HERE until the JS promise settles\n' +
         '    return f"{user}: {row * 2}"\n', ns);
  const score = ns.get('score');

  // The JS side hands it an ASYNC function.  Python suspends at
  // fetch(user); a 1ms ticker proves the loop never blocks.
  let ticks = 0;
  const meter = setInterval(() => ticks++, 1);

  const result = await py.task(score)(async (user) => {
    await sleep(50); // fake network latency
    // ...and the "fetch" may use Python again, same domain:
    return await py.task(np.mean)(new Float64Array([3, 4, 5]));
  }, 'ada');

  clearInterval(meter);
  console.log('result     :', String(result)); // ada: 8.0 * 2
  console.log('loop_alive :', ticks, 'ticks during the suspension');

  await py.destroy();
})().catch((e) => { console.error('ERR', e.message); process.exit(1); });
