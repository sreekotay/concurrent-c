/* Isolated domains: a FULL CPython per child process, speaking the
 * cc-node wire discipline mirrored.  numpy works in EVERY domain (the
 * subinterpreter refusal is gone), N domains are N GILs in N processes,
 * a child crash is a rejected promise (parent survives), and per-domain
 * python/venv selection is honest.  Cross-process is natively async:
 * chains extend lazily with zero round trips, calls are Promises.
 * Deterministic booleans; the paired smoke pins them. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const { execSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const ccpy = require(process.cwd() + '/npm/cc-python');

(async () => {
  const py = ccpy.create({ isolated: true });
  const np = py.import('numpy');
  const b = py.import('builtins');

  // 1. numpy IN A CHILD — no subinterpreter refusal, full C extensions.
  {
    const buf = new Float64Array([1, 2, 3, 4]);
    out('numpy_in_child', (await np.sum(buf)) === 10);
  }

  // 2. Values cross by the wire rules: small arrays inline as typed
  //    arrays, big ones stay handles, scalars are scalars, non-finite
  //    floats survive.
  {
    const small = await np.arange(5);
    out('small_array_inlines', small.constructor === BigInt64Array &&
        small.length === 5 && small[4] === 4n);
    const big = await np.zeros(1 << 17);
    out('big_array_is_handle', typeof big === 'function');
    out('handle_chains', (await big.mean()) === 0);
    out('nonfinite_crosses', (await b.float('-inf')) === -Infinity);
    // Big buffers spill through shared memory, both directions, and the
    // spill files are consumed — none stray.
    const wide = new Float64Array(1 << 18).map((_, i) => (i % 89) * 0.5);
    let want = 0;
    for (const v of wide) want += v;
    out('shm_arg_integrity', Math.abs((await np.sum(wide)) - want) < 1e-6);
    const echoed = await (await np.arange(1 << 18)).toTypedArray();
    out('shm_result_integrity', echoed.length === (1 << 18) &&
        echoed[(1 << 18) - 1] === BigInt((1 << 18) - 1));
    {
      const dir = fs.existsSync('/dev/shm') ? '/dev/shm' : os.tmpdir();
      const stray = fs.readdirSync(dir).filter(
          (f) => f.startsWith('ccpy-' + process.pid + '-'));
      out('shm_no_strays', stray.length === 0);
    }
  }

  // 3. Lazy chains: np.linalg.norm is ZERO round trips until the call.
  {
    const norm = np.linalg.norm; // no awaits anywhere on the path
    out('lazy_chain_call', (await norm(new Float64Array([3, 4]))) === 5);
  }

  // 4. JS callbacks cross the wire — and may be async (the child blocks
  //    on its line; the parent awaits whatever it likes first).
  {
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const mapped = await b.list(await b.map(async (x) => {
      await sleep(5);
      return x * 10;
    }, [1, 2, 3]));
    out('async_callback_over_wire', JSON.stringify(mapped) === '[10,20,30]');
  }

  // 5. Exceptions keep "Type: message".
  {
    let msg = '';
    try { await np.linalg.inv(new Float64Array([1, 2, 3])); }
    catch (e) { msg = e.message; }
    out('exc_type_and_message', /LinAlgError|ValueError/.test(msg));
  }

  // 6. THE headline: two domains are two PROCESSES that execute
  //    concurrently.  Proven by mechanism, not wall clock — each child
  //    reports its own [start, end] monotonic interval, and the
  //    intervals must overlap.  That holds even on a fully loaded box
  //    (where timeslicing erases speedup but not concurrency); the
  //    SCALING numbers live in examples/js_multiprocess_numpy.js where
  //    the box is quiet.  A wire that accidentally serialized domains
  //    would produce disjoint intervals and fail here.
  {
    const py2 = ccpy.create({ isolated: true });
    const np2 = py2.import('numpy');
    const spin = 'lambda n: (lambda t: [t.monotonic(), ' +
                 'sum(i * i for i in range(n)) % 7, t.monotonic()])' +
                 '(__import__("time"))';
    const f1 = await (py.import('builtins')).eval(spin);
    const f2 = await (py2.import('builtins')).eval(spin);
    const N = 400000;
    await f1(1); await f2(1); // warm
    const [r1, r2] = await Promise.all([f1(N), f2(N)]);
    const [a0, , a1] = r1;
    const [b0, , b1] = r2;
    out('two_pythons_concurrent', a0 < b1 && b0 < a1);
    out('numpy_in_second_child',
        (await np2.sum(new Float64Array([5, 6]))) === 11);
    await py2.destroy();
  }

  // 7. Per-domain venv: each child picks its own python — honest now.
  {
    const venv = fs.mkdtempSync(path.join(os.tmpdir(), 'ccpy-proc-')) + '/v';
    execSync(`python3 -m venv --without-pip '${venv}'`);
    const pv = ccpy.create({ isolated: true, python: venv });
    const sys = pv.import('sys');
    out('per_domain_venv', String(await sys.prefix) === venv);
    await pv.destroy();
  }

  // 8. Crash isolation: the child dies mid-flight; in-flight and later
  //    calls reject articulately, the PARENT keeps running.
  {
    const pc = ccpy.create({ isolated: true });
    const osmod = pc.import('os');
    let died = '';
    try { await osmod._exit(9); } catch (e) { died = e.message; }
    out('crash_rejects_inflight', /closed|exited/.test(died));
    let late = '';
    try { await pc.import('math').pi; } catch (e) { late = e.message; }
    out('crash_rejects_later', /closed/.test(late));
    out('crash_marks_closed', pc.closed);
    out('parent_survives', (await np.sum(new Float64Array([1, 1]))) === 2);
  }

  // 9. Handle hygiene and teardown.
  {
    const before = await py.stats();
    const held = await np.zeros(1 << 17);
    const mid = await py.stats();
    const after = await py.release(held);
    out('release_drops', mid === before + 1 && after === before);
  }
  await py.destroy();
  out('destroy_lands', py.closed);
  let closedMsg = '';
  try { await np.sum(new Float64Array([1])); } catch (e) { closedMsg = e.message; }
  out('after_close_rejects', /closed/.test(closedMsg));

  console.log('proc suite done');
})().catch((e) => { console.error('PROC SUITE ERROR:', e.message); process.exit(1); });
