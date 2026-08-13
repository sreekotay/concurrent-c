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

    out('neg_inproc_isproxy_mod', ccpy.isProxy(math) && py.isProxy(math));
    out('neg_inproc_isproxy_scalar',
        !ccpy.isProxy(math.sqrt(16)) && !ccpy.isProxy(null));
    out('neg_inproc_is_same',
        py.is(math, py.import('math')) && py.is(math, math));
    out('neg_inproc_is_distinct', !py.is(b.list(), b.list()));
    out('neg_inproc_is_scalar',
        raisesSync(() => py.is(1, 1), /two proxies/));

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

    // Proxy traps: reflection cheap; unrepresentable toJSON refuses by path.
    out('neg_inproc_proxy_keys', Array.isArray(Object.keys(math.sqrt)));
    out('neg_inproc_proxy_json',
        raisesSync(() => JSON.stringify(math.sqrt),
                   /cannot materialize/));
    out('neg_inproc_tojs',
        raisesSync(() => math.sqrt.toJS(), /cannot materialize/));
    {
      const util = require('util');
      out('neg_inproc_inspect',
          /built-in|function|sqrt/i.test(util.inspect(math.sqrt)));
    }
    out('neg_inproc_proxy_alive', math.floor(3.2) === 3);

    // In-process lists / dicts / None / kwargs (ergonomic maturity).
    {
      const { kwargs } = ccpy;
      const builtins = py.import('builtins');
      out('neg_inproc_list_arg',
          builtins.list([1, 2, 3]).toJS().join(',') === '1,2,3');
      out('neg_inproc_dict_arg',
          builtins.dict({ a: 1, b: 2 }).toJS().a === 1 &&
          builtins.dict({ a: 1, b: 2 }).toJS().b === 2);
      out('neg_inproc_undefined_none',
          builtins.list([undefined, null]).toJS()[0] === null &&
          builtins.list([undefined, null]).toJS()[1] === null);
      const fmt = builtins.eval(
          "lambda a, sep=' ': sep.join(str(x) for x in a)");
      out('neg_inproc_kwargs',
          fmt([1, 2], kwargs({ sep: '+' })) === '1+2');
      out('neg_inproc_positional_dict',
          builtins.type(builtins.dict({ x: 1 })).__name__ === 'dict' ||
          builtins.dict({ x: 1 }).toJS().x === 1);
    }

    // Dead-domain import rejects (no silent proxy).
    {
      const d = ccpy.create();
      d.destroy();
      out('neg_inproc_import_after_close',
          raisesSync(() => d.import('math'), /closed/));
    }

    // Strict toJS/toJSON: one materializer, path-bearing refuse, host JSON.
    {
      const builtins = py.import('builtins');
      const g = builtins.dict();
      const d = builtins.eval(
          '{"a": float("nan"), "n": None, "k": {1: "x"}, "t": (1, 2)}', g);
      const js = d.toJS();
      out('neg_inproc_tojs_nan_null',
          Number.isNaN(js.a) && JSON.stringify(js) ===
          JSON.stringify({ a: null, n: null, k: { '1': 'x' }, t: [1, 2] }));
      out('neg_inproc_tojs_none_null', js.n === null);
      out('neg_inproc_tojs_int_keys', js.k && js.k['1'] === 'x');
      out('neg_inproc_tojs_tuple',
          Array.isArray(js.t) && js.t[0] === 1 && js.t[1] === 2);
      out('neg_inproc_tojs_one_materializer',
          JSON.stringify(d) === JSON.stringify(d.toJS()));

      const big = builtins.eval('{"x": 2 ** 100}', g);
      out('neg_inproc_tojs_bigint_throw',
          raisesSync(() => JSON.stringify(big.toJS()),
                     /BigInt|serialize/i));

      const withSet = builtins.eval('{"tags": {1, 2, 3}}', g);
      out('neg_inproc_tojs_set_path',
          raisesSync(() => withSet.toJS(),
                     /cannot materialize set at \$\.tags/));

      builtins.exec('cyc={}; cyc["self"]=cyc', g);
      const cyc = builtins.eval('cyc', g);
      out('neg_inproc_tojs_cycle',
          raisesSync(() => cyc.toJS(), /cycle detected/));

      // JS circular args must refuse before blowing the call stack.
      {
        const o = {};
        o.self = o;
        out('neg_inproc_js_cycle_arg',
            raisesSync(() => builtins.type(o), /circular reference/));
      }

      // Multi-hop callback errors must not telescope
      // RuntimeError: Error: python: invoke: RuntimeError: …
      {
        builtins.exec('def boom(f):\n  return f()\n', g);
        const boom = builtins.eval('boom', g);
        let n = 0;
        function bad() {
          n++;
          if (n < 5) return boom(bad);
          throw new Error('leaf');
        }
        let msg = '';
        try { boom(bad); } catch (e) { msg = String(e.message); }
        out('neg_inproc_err_flatten',
            /leaf/.test(msg) &&
            (msg.match(/RuntimeError/g) || []).length <= 1 &&
            (msg.match(/python:\s*invoke:/g) || []).length <= 1 &&
            !/RuntimeError:\s*Error:\s*python:/.test(msg));
      }

      const keyed = builtins.eval('{"keys": 1, "get": 2}', g);
      out('neg_inproc_keys_win',
          keyed.keys === 1 && keyed.get === 2);
      {
        const desc = Object.getOwnPropertyDescriptor(keyed, 'keys');
        out('neg_inproc_gopd_accessor',
            !!desc && typeof desc.get === 'function' &&
            !Object.prototype.hasOwnProperty.call(desc, 'value'));
      }
    }

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
      // Empty typed arrays must stay memoryview — not flip to list at n=0.
      {
        const ty = builtins.type;
        const ln = builtins.len;
        out('neg_inproc_empty_f64_memoryview',
            ty(new Float64Array(0)).__name__ === 'memoryview' &&
            ty(new Float64Array(1)).__name__ === 'memoryview' &&
            ln(new Float64Array(0)) === 0);
        out('neg_inproc_empty_u8_memoryview',
            ty(new Uint8Array(0)).__name__ === 'memoryview' &&
            ln(new Uint8Array(0)) === 0);
      }

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

    // Worker threads: in-process is owned by one OS thread. Concurrent
    // create() otherwise aborts Node (_PyImport_Init); late create() on
    // another thread must refuse articulately — not a third-party ImportError.
    {
      const { Worker, isMainThread } = require('worker_threads');
      if (!isMainThread) throw new Error('neg suite expects main thread');
      const main = ccpy.create();
      const workerSrc = `
        const { parentPort } = require('worker_threads');
        const ccpy = require(${JSON.stringify(process.cwd() + '/npm/cc-python')});
        try {
          ccpy.create();
          parentPort.postMessage({ ok: false, msg: 'no-throw' });
        } catch (e) {
          parentPort.postMessage({
            ok: /owned by another thread|isolated:\\s*true/i.test(String(e && e.message)),
            msg: String(e && e.message),
          });
        }
      `;
      const msg = await new Promise((resolve, reject) => {
        const w = new Worker(workerSrc, { eval: true });
        w.on('message', resolve);
        w.on('error', reject);
        w.on('exit', (code) => {
          if (code !== 0) reject(new Error('worker exit ' + code));
        });
      });
      out('neg_inproc_worker_create_refuses', !!msg.ok);
      main.destroy();
    }
  }

  // ---- isolated ---------------------------------------------------------
  {
    const py = ccpy.create({ isolated: true });
    const b = py.import('builtins');
    const id = b.eval('lambda x: x');

    out('neg_iso_dollar_key',
        raisesSync(() => id({ $x: 1 }), /reserved/));
    {
      const huge = 10n ** 40n;
      out('neg_iso_bigint_roundtrip', id(huge) === huge);
      const u64 = b.eval('(1 << 64) - 1');
      out('neg_iso_u64_bigint',
          typeof u64 === 'bigint' && u64 === (1n << 64n) - 1n);
    }
    {
      /* Non-plain values stay handles (set inside) so toJS is available. */
      const d = b.eval('{"tags": {1, 2}, "x": float("nan")}');
      out('neg_iso_tojs_set_path',
          raisesSync(() => d.toJS(),
                            /cannot materialize set at \$\.tags/));
      const ok = b.eval('{"a": 1, "b": [2, 3], "c": {4: 5}}');
      const js = ok.toJS();
      const js2 = ok.toJSON();
      out('neg_iso_tojs_ok',
          js.a === 1 && Array.isArray(js.b) && js.b[1] === 3 &&
          js.c && js.c['4'] === 5 &&
          JSON.stringify(js) === JSON.stringify(js2));
    }
    // Missing import fails at import (same as in-process).
    out('neg_iso_import_fail',
        raisesSync(() => py.import('definitely_not_a_pkg_neg_xyz'),
                   /ModuleNotFoundError|No module/));

    // Unawaited task result is a Promise — say so, not "unsupported".
    {
      const pending = py.task(b.dict)();
      out('neg_iso_promise_arg',
          raisesSync(() => id(pending), /got a Promise|py\.task/));
      await pending;
    }

    // Blocking call while py.task is in flight — fail at the blocking
    // call, not inside a sibling callback.
    {
      const apply = b.eval('lambda f: f()');
      let unlock;
      const gate = new Promise((r) => { unlock = r; });
      const p = py.task(apply)(async () => { await gate; return 7; });
      out('neg_iso_block_during_task',
          raisesSync(() => apply(() => 1),
                     /in flight|overlap with py\.task/));
      unlock();
      out('neg_iso_block_after_task',
          (await p) === 7 && apply(() => 1) === 1);
    }

    // Empty dict stays a live handle (exec namespace), not JS {}.
    {
      const ns = b.dict();
      out('neg_iso_empty_dict_handle', typeof ns === 'function');
      b.exec('def add1(x):\n  return x + 1\n', ns);
      const add1 = ns.get('add1');
      out('neg_iso_exec_namespace', add1(40) === 41);
    }

    out('neg_iso_isproxy_mod', ccpy.isProxy(b) && py.isProxy(b));
    out('neg_iso_isproxy_scalar', !ccpy.isProxy(id(1)));
    out('neg_iso_is_same', py.is(b, py.import('builtins')) && py.is(b, b));
    out('neg_iso_is_distinct', !py.is(b.list(), b.list()));
    out('neg_iso_is_scalar',
        raisesSync(() => py.is(1, 1), /two proxies/));

    // Foreign handle as call argument — must not silently re-home.
    const other = ccpy.create({ isolated: true });
    const foreign = (other.import('builtins')).eval('lambda x: x + 1');
    out('neg_iso_foreign_arg',
        raisesSync(() => id(foreign), /another bridge/));
    await other.destroy();

    // Sibling domain still works after abuse.
    await py.destroy();
    out('neg_iso_after_close',
        raisesSync(() => id(1), /closed/));

    const ok = ccpy.create({ isolated: true });
    const sum = (ok.import('builtins')).eval('lambda a, b: a + b');
    out('neg_iso_sibling_alive', sum(2, 3) === 5);
    await ok.destroy();
  }

  console.log('cc-python neg suite done');
})().catch((e) => {
  console.error('NEG SUITE ERROR:', e && e.stack ? e.stack : e);
  process.exit(1);
});
