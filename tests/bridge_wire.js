/* Wire-integrity rungs for the npm/cc-python isolated wire: the
 * protocol lives on dedicated fds, so user print() reaches the real
 * stdout (the forged lines below appear in the pinned output as
 * ordinary prints), and replies pair by request id, so output shaped
 * exactly like a bridge reply can never be consumed as one — the
 * genuine result comes back and pipelined calls stay correctly
 * paired.  Python's print(flush=True) is a synchronous write, so its
 * lines land before the reply and the interleaving pins exactly. */
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const ccpy = require(path.join(__dirname, '..', 'npm', 'cc-python'));
const out = (n, c) => console.log(n, c ? 'true' : 'false');

const shmBase = fs.existsSync('/dev/shm') ? '/dev/shm' : os.tmpdir();
const myShmDirs = () => fs.readdirSync(shmBase)
  .filter((f) => f.startsWith('ccpy-' + process.pid + '-'));

(async () => {
  const py = ccpy.create({ isolated: true });
  const b = py.import('builtins');
  const forge = await b.eval(
    'lambda: (print(\'{"v":999}\', flush=True), 41)[1]');
  out('py_forge_ignored', (await forge()) === 41);
  const forge2 = await b.eval(
    'lambda: (print(\'{"v":888,"id":1}\', flush=True), 42)[1]');
  out('py_forged_id_ignored', (await forge2()) === 42);
  const sq = await b.eval('lambda x: x * x');
  const trio = await Promise.all([sq(2), sq(3), sq(4)]);
  out('py_pairing_order', trio.join(',') === '4,9,16');
  let errOk = false;
  try { await b.eval('1/0'); }
  catch (e) { errOk = /ZeroDivision/.test(e.message); }
  out('py_error_paired', errOk);
  const shout = await b.eval(
    'lambda: (print("py-print-visible", flush=True), 7)[1]');
  out('py_print_visible', (await shout()) === 7);

  // kwargs cross the wire explicitly marked and last; keyword-only
  // signatures work, misuse is loud at the call site.
  const fmt = await b.eval('lambda a, *, sep="-": f"{a}{sep}end"');
  out('py_kwargs_call',
      (await fmt(1, ccpy.kwargs({ sep: '+' }))) === '1+end');
  out('py_kwargs_default', (await fmt(2)) === '2-end');
  let kwPos = false;
  try { await fmt(1, 2); }
  catch (e) { kwPos = /positional/.test(e.message); }
  out('py_kwargs_typeerror_crosses', kwPos);
  let kwPlace = false;
  try { await fmt(ccpy.kwargs({ sep: '+' }), 1); }
  catch (e) { kwPlace = /last argument/.test(e.message); }
  out('py_kwargs_must_be_last', kwPlace);
  await py.destroy();

  // Spill hardening: each bridge owns a private 0700 dir; a >64KB
  // buffer round-trips through it, and the dir goes with the bridge.
  const py2 = ccpy.create({ isolated: true });
  const echo = await py2.import('builtins').eval('lambda a: a');
  const big = new Float64Array(1 << 15).fill(0.5);
  const back = await (await echo(big)).toTypedArray();
  const dirs = myShmDirs();
  const mode = dirs.length === 1
    ? (fs.statSync(path.join(shmBase, dirs[0])).mode & 0o777) : -1;
  out('py_shm_private_dir', mode === 0o700);
  out('py_shm_roundtrip', back.length === big.length && back[7] === 0.5);
  await py2.destroy();
  out('py_shm_dir_swept', myShmDirs().length === 0);
})().catch((e) => { console.log('FAIL', e.message); process.exit(1); });
