/* concurrent-c-python negative / articulate-error pack.
 *
 * Misuse must fail loudly with a stable message shape; domains stay
 * usable afterward (except after_close). Covers in-process and isolated
 * (no numpy). Deterministic booleans for the paired smoke.
 */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const ccpy = require(process.cwd() + '/npm/cc-python');

function raisesSync(fn, re) {
  try { fn(); return false; }
  catch (e) { return re.test(e.message); }
}
async function raisesAsync(fn, re) {
  try { await fn(); return false; }
  catch (e) { return re.test(e.message); }
}

(async () => {
  // ---- in-process -------------------------------------------------------
  {
    const py = ccpy.create();
    const math = py.import('math');

    out('neg_inproc_dollar_key',
        raisesSync(() => math.floor({ $x: 1 }), /unsupported argument/));
    out('neg_inproc_unsupported_set',
        raisesSync(() => math.floor(new Set([1])), /unsupported argument/));

    const A = ccpy.create();
    const B = ccpy.create();
    out('neg_inproc_foreign_arg',
        raisesSync(() => B.import('math').hypot(A.import('math').floor, 3),
                   /another bridge|released/));
    A.destroy();
    B.destroy();

    const h = math.sqrt;
    py.release(h);
    out('neg_inproc_use_after_release',
        raisesSync(() => h(4), /released|another bridge/));
    out('neg_inproc_release_again',
        raisesSync(() => py.release(h), /released|another bridge/));

    // keep-past-return (lease boundary)
    const b = py.import('builtins');
    const g = b.dict();
    b.exec('G=[]\ndef keep(mv):\n  G.append(mv)\n  return 1\n', g);
    out('neg_inproc_keep_past_return',
        raisesSync(() => b.eval('keep', g)(new Float64Array(8)),
                   /retained by the callee|borrow ends/));

    py.destroy();
    out('neg_inproc_after_close',
        raisesSync(() => math.floor(1.2), /closed/));
    out('neg_inproc_destroy_idempotent', py.closed);
  }

  // ---- isolated ---------------------------------------------------------
  {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const id = await b.eval('lambda x: x');

    out('neg_iso_dollar_key',
        await raisesAsync(() => id({ $x: 1 }), /reserved/));
    out('neg_iso_bigint',
        await raisesAsync(() => id(10n ** 20n), /bigint beyond 2\^53/));

    // Lazy import fail surfaces on first use.
    const missing = py.import('definitely_not_a_pkg_neg_xyz');
    out('neg_iso_lazy_import_fail',
        await raisesAsync(() => missing.foo(), /ModuleNotFoundError|No module/));

    // Pass unresolved module root as an argument.
    const pending = py.import('math');
    out('neg_iso_pass_unresolved',
        await raisesAsync(() => id(pending), /has not landed yet|await any use/));

    // Foreign handle as call argument — must not silently re-home.
    const other = ccpy.create({ isolated: true });
    const foreign = await (other.import('builtins')).eval('lambda x: x + 1');
    out('neg_iso_foreign_arg',
        await raisesAsync(() => id(foreign), /another bridge/));
    await other.destroy();

    // Sibling domain still works after abuse.
    await py.destroy();
    out('neg_iso_after_close',
        await raisesAsync(() => id(1), /closed/));

    const ok = ccpy.create({ isolated: true });
    const sum = await (ok.import('builtins')).eval('lambda a, b: a + b');
    out('neg_iso_sibling_alive', (await sum(2, 3)) === 5);
    await ok.destroy();
  }

  console.log('cc-python neg suite done');
})().catch((e) => {
  console.error('NEG SUITE ERROR:', e && e.stack ? e.stack : e);
  process.exit(1);
});
