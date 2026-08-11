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

    // Bare eval/exec use the domain's __main__ (no frame to default from).
    out('neg_inproc_bare_eval', b.eval('lambda x: x+1')(41) === 42);
    b.exec('ccpy_bare_x = 7');
    out('neg_inproc_bare_exec', b.eval('ccpy_bare_x') === 7);

    // Dict reflection: keys/in match iteration (not the empty function target).
    {
      const d = b.dict();
      d.a = 1;
      d.bb = 2;
      const keys = Object.keys(d).sort();
      out('neg_inproc_dict_keys',
          keys.length === 2 && keys[0] === 'a' && keys[1] === 'bb');
      out('neg_inproc_dict_in', 'a' in d && !('zz' in d) && d.a === 1);
    }

    // setattr reaches Python (SimpleNamespace).
    {
      const ns = py.import('types').SimpleNamespace();
      ns.zz = 41;
      out('neg_inproc_setattr', ns.zz === 41);
    }

    // Symbol.dispose sync-closes (using-safe).
    {
      const d = ccpy.create();
      const m = d.import('math');
      d[Symbol.dispose]();
      out('neg_inproc_dispose',
          d.closed &&
          raisesSync(() => m.sqrt(9), /closed/));
    }

    // Python exceptions: .pyType / .code are the class, not CC_ERR_INTERNAL.
    {
      let e = null;
      try { b.eval('lambda: (_ for _ in ()).throw(KeyError())')(); }
      catch (err) { e = err; }
      out('neg_inproc_pytype',
          !!e && e.pyType === 'KeyError' && e.code === 'KeyError');
    }

    // Proxy traps: stringify must fail articulately; domain stays usable.
    out('neg_inproc_proxy_keys', Array.isArray(Object.keys(math.sqrt)));
    out('neg_inproc_proxy_json',
        raisesSync(() => JSON.stringify(math.sqrt),
                   /toJSON|no attribute|AttributeError|unsupported/));
    out('neg_inproc_proxy_alive', math.floor(3.2) === 3);

    // Scalar edges through a Python identity.
    {
      const builtins = py.import('builtins');
      const g = builtins.dict();
      const id = builtins.eval('lambda x: x', g);
      out('neg_inproc_scalar_nan', Number.isNaN(id(NaN)));
      out('neg_inproc_scalar_inf', id(Infinity) === Infinity);
      out('neg_inproc_scalar_ninf', id(-Infinity) === -Infinity);
      // Signed -0 must round-trip (not collapse via int-valued double).
      out('neg_inproc_scalar_neg0', Object.is(id(-0), -0));

      // Exact BigInt: beyond signed i64 (u64 max) and round-trip.
      const u64max = builtins.eval('lambda: (1 << 64) - 1', g)();
      out('neg_inproc_u64_bigint',
          typeof u64max === 'bigint' && u64max === (1n << 64n) - 1n);
      out('neg_inproc_bigint_roundtrip', id(u64max) === u64max);
      const huge = 10n ** 40n;
      out('neg_inproc_bigint_huge', id(huge) === huge);

      // Uint8Array / Buffer zero-copy memoryviews.
      const mv_sum = builtins.eval('lambda mv: sum(mv)', g);
      out('neg_inproc_u8_sum',
          mv_sum(new Uint8Array([1, 2, 3, 4])) === 10);
      out('neg_inproc_buffer_sum',
          mv_sum(Buffer.from([9, 1])) === 10);

      // Lone surrogates must refuse — not become U+FFFD.
      out('neg_inproc_lone_surrogate',
          raisesSync(() => id('\uD800'), /lone UTF-16 surrogate/));

      // R2: callback args are Proxies — missing attr throws, not undefined.
      {
        const call = builtins.eval('lambda f, x: f(x)', g);
        const lst = builtins.eval('[1, 2, 3]', g);
        let saw;
        try {
          call((x) => x.no_such_attr_zzz, lst);
          saw = 'no-throw';
        } catch (e) {
          saw = /no_such_attr_zzz|AttributeError|no attribute/i.test(e.message)
              ? 'threw' : ('other:' + e.message);
        }
        out('neg_inproc_cb_missing_attr', saw === 'threw');
      }

      // R1: toTypedArray door on in-process handles.
      {
        const arr = builtins.eval(
            'lambda: __import__("array").array("d", [1.5, 2.5])', g)();
        const ta = arr.toTypedArray();
        out('neg_inproc_to_typed_array',
            ta instanceof Float64Array && ta.length === 2 && ta[0] === 1.5);
      }

      // R3: leased buffer may cross into a JS callback (copied, not retained).
      {
        const call = builtins.eval('lambda f, mv: f(mv)', g);
        const got = call((x) => {
          if (!(x instanceof Float64Array)) return -1;
          return x[0] + x[1];
        }, new Float64Array([3, 4]));
        out('neg_inproc_lease_to_cb', got === 7);
      }

      // Symbol.iterator — for..of over a Python list.
      {
        const lst = builtins.eval('[10, 20, 30]', g);
        const got = [];
        for (const x of lst) got.push(x);
        out('neg_inproc_for_of',
            got.length === 3 && got[0] === 10 && got[2] === 30);
      }

      // Empty str(exc) still names the type (KeyError()).
      {
        builtins.exec('def _boom_ke():\n  raise KeyError()\n', g);
        const boom = builtins.eval('_boom_ke', g);
        out('neg_inproc_empty_exc_type',
            raisesSync(() => boom(), /KeyError/));
      }

      // Attr cache: repeated mod.fn does not grow the handle ledger.
      {
        const math = py.import('math');
        void math.floor; // warm
        const base = py.stats();
        for (let i = 0; i < 200; i++) math.floor(1.2);
        out('neg_inproc_attr_cache', py.stats() <= base + 2);
      }
    }

    // sys.exit must become an articulate error — not take down Node.
    {
      const builtins = py.import('builtins');
      const g = builtins.dict();
      const boom = builtins.eval('lambda: (__import__("sys").exit(3))', g);
      out('neg_inproc_system_exit',
          raisesSync(() => boom(), /invoke:\s*3|SystemExit|exit/));
      out('neg_inproc_after_system_exit', math.floor(1.8) === 1);
    }

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
    {
      const huge = 10n ** 40n;
      out('neg_iso_bigint_roundtrip', (await id(huge)) === huge);
      const u64 = await (b.eval('(1 << 64) - 1'));
      out('neg_iso_u64_bigint',
          typeof u64 === 'bigint' && u64 === (1n << 64n) - 1n);
    }
    // Lazy import fail surfaces on first use.
    const missing = py.import('definitely_not_a_pkg_neg_xyz');
    out('neg_iso_lazy_import_fail',
        await raisesAsync(() => missing.foo(), /ModuleNotFoundError|No module/));

    // Pass unresolved module root as an argument.
    const pending = py.import('math');
    out('neg_iso_pass_unresolved',
        await raisesAsync(() => id(pending), /has not landed yet|await any use/));

    // Unawaited call result is a Promise — say so, not "unsupported".
    out('neg_iso_promise_arg',
        await raisesAsync(() => id(b.dict()), /got a Promise|await isolated/));

    // Empty dict stays a live handle (exec namespace), not JS {}.
    {
      const ns = await b.dict();
      out('neg_iso_empty_dict_handle', typeof ns === 'function');
      await b.exec('def add1(x):\n  return x + 1\n', ns);
      const add1 = await ns.get('add1');
      out('neg_iso_exec_namespace', (await add1(40)) === 41);
    }

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
