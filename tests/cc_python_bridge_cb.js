/* Callback rungs: JS functions crossing INTO Python calls, both flavors.
 * Python's map() is lazy — callbacks fire where the iterator is CONSUMED
 * — so the lane rungs consume via py.task(list)(...).  Deterministic
 * booleans; the paired smoke pins them. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  const py = ccpy.create();
  const builtins = py.import('builtins');
  const json = py.import('json');
  const math = py.import('math');
  const toList = py.task(builtins.list); // lane-side consumption
  const lst = json.loads('[3, 1, 2]');

  // 1. Both flavors, scalar and string returns.
  out('sync_map',
      String(builtins.list(builtins.map((x) => x * 2, lst))) === '[6, 2, 4]');
  {
    const r = await toList(builtins.map((x) => x + 10, lst));
    out('lane_map', String(r) === '[13, 11, 12]');
  }
  out('string_returns',
      String(builtins.list(builtins.map((x) => 'n' + x, lst))) ===
      "['n3', 'n1', 'n2']");

  // 2. JS throws become Python exceptions and surface with their message
  //    — as a sync throw and as a task rejection.
  {
    let sync = false;
    try { builtins.list(builtins.map(() => { throw new Error('boom'); }, lst)); }
    catch (e) { sync = /boom/.test(e.message); }
    out('throw_surfaces_sync', sync);
    let lane = false;
    try { await toList(builtins.map(() => { throw new Error('pow'); }, lst)); }
    catch (e) { lane = /pow/.test(e.message); }
    out('throw_surfaces_lane', lane);
  }

  // 3. A Promise return is refused articulately (callbacks are sync in v1).
  {
    let refused = false;
    try { builtins.list(builtins.map(async (x) => x, lst)); }
    catch (e) { refused = /synchronous in v1/.test(e.message); }
    out('promise_return_refused', refused);
  }

  // 4. A lane-side callback makes nested SYNC bridge calls — the lane
  //    released the GIL, so main can take it mid-callback.
  {
    const r = await toList(builtins.map((x) => math.floor(x + 0.5), lst));
    out('nested_sync_in_callback', String(r) === '[3, 1, 2]');
  }

  // 5. A lane-side callback SUBMITS (never awaits) a task on its own
  //    domain — fire-and-forget composes; awaiting would be the cycle.
  {
    let submitted = null;
    const sq = py.task(math.sqrt);
    const r = await toList(builtins.map((x) => {
      if (submitted === null) submitted = sq(16);
      return x;
    }, lst));
    out('callback_submits_task',
        String(r) === '[3, 1, 2]' && (await submitted) === 4);
  }

  // 6. A retained callable: Python holds the JS function past the call
  //    that minted it (functools.partial), and it fires on a later call
  //    — on either thread.
  {
    const partial = py.import('functools').partial((x) => x * 3, 7);
    out('retained_fires_sync', partial() === 21);
    out('retained_fires_lane', (await py.task(partial)()) === 21);
  }

  // 7. Reentrant destroy: a lane-side callback revokes its own bridge
  //    mid-task.  The in-flight job finishes, the drain follows, nothing
  //    deadlocks, and the revocation answers at the next door.
  {
    const py2 = ccpy.create();
    const b2 = py2.import('builtins');
    const l2 = py2.import('json').loads('[1, 2, 3]');
    const r = await py2.task(b2.list)(b2.map((x) => {
      if (x === 2) py2.destroy(); // not awaited — revocation from inside
      return x * 10;
    }, l2));
    out('reentrant_destroy_completes', r !== undefined);
    let revoked = false;
    try { String(r); } catch (e) { revoked = /closed/.test(e.message); }
    out('reentrant_destroy_revokes', revoked);
    await sleep(30); /* the drain sentinel lands */
    out('reentrant_destroy_closes', py2.closed);
  }

  await py.destroy();
  out('main_closed', py.closed);
  console.log('cb suite done');
})().catch((e) => { console.error('CB SUITE ERROR:', e.message); process.exit(1); });
