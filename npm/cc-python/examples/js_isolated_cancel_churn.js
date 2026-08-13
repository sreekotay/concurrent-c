/* Cancel-via-kill cost for isolated domains.
 *
 * There is no clean cancel of CPU-bound native work (BLAS, long C).
 * Product choices: wait for cooperative destroy (in-flight may fulfill),
 * or SIGKILL the child and mint a replacement. This file measures the
 * second path — not a correctness suite (see stress/bridge/).
 *
 *   node npm/cc-python/examples/js_isolated_cancel_churn.js
 *
 * RESULT lines are machine-comparable (spawn / kill-reject / respawn).
 */
'use strict';

const ccpy = require(__dirname + '/..');

const CYCLES = Number(process.env.CANCEL_CHURN_CYCLES || 20);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hr = () => process.hrtime.bigint();
const nsToUs = (n) => Number(n) / 1e3;

(async () => {
  // Warm one domain so cold import noise is outside the loop.
  {
    const warm = ccpy.create({ isolated: true });
    const b = warm.import('builtins');
    b.eval('lambda: 1')();
    await warm.destroy();
  }

  let spawnUs = 0, killRejectUs = 0, respawnUs = 0;
  for (let i = 0; i < CYCLES; i++) {
    const tSpawn = hr();
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const pidFn = b.eval('lambda: __import__("os").getpid()');
    const pid = pidFn();
    const slow = b.eval(
      'lambda: (__import__("time").sleep(0.25), 1)[1]');
    spawnUs += nsToUs(hr() - tSpawn);

    const p = py.task(slow)();
    await sleep(20);
    const tKill = hr();
    try { process.kill(pid, 'SIGKILL'); } catch (_) {}
    try { await p; } catch (_) { /* expected closed/exited */ }
    try { await py.destroy(); } catch (_) {}
    killRejectUs += nsToUs(hr() - tKill);

    const tRe = hr();
    const py2 = ccpy.create({ isolated: true });
    const one = py2.import('builtins').eval('lambda: 1');
    if (one() !== 1) throw new Error('respawn probe failed');
    await py2.destroy();
    respawnUs += nsToUs(hr() - tRe);
  }

  console.log('RESULT cancel_churn_cycles %d', CYCLES);
  console.log('RESULT cancel_churn_spawn_us %d', Math.round(spawnUs / CYCLES));
  console.log('RESULT cancel_churn_kill_reject_us %d',
              Math.round(killRejectUs / CYCLES));
  console.log('RESULT cancel_churn_respawn_us %d',
              Math.round(respawnUs / CYCLES));
  console.log('RESULT cancel_churn_cycle_us %d',
              Math.round((spawnUs + killRejectUs + respawnUs) / CYCLES));
})().catch((e) => {
  console.error(e && e.stack ? e.stack : e);
  process.exit(1);
});
