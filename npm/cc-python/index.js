/* concurrent-c-python: Python from Node over the Concurrent-C bridge.
 *
 *     const py = require('concurrent-c-python').create();  // Isolation Domain
 *     const np = py.import('numpy');
 *     const s  = np.linalg.norm(new Float64Array([3, 4]));   // 5
 *     await py.destroy();   // one sweep — await before the next create()
 *
 * Proxies wrap opaque handles: attribute access is getattr, a call is
 * invoke, scalars come back as JS scalars and everything else as another
 * proxy.  Every proxy strong-refs its bridge, so the GC collects the
 * domain (and runs the same sweep destroy() runs) only when the whole
 * graph is unreachable.  `using py = ...` disposes at end of scope. */
'use strict';

const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

function locateAddon() {
  const plat = process.platform + '-' + process.arch;
  const repoAddon = path.join(__dirname, '..', '..', 'bin', 'cc_python.node');
  const inRepo = fs.existsSync(path.join(__dirname, '..', '..', 'cc', 'bin'));
  const candidates = [];
  if (process.env.CC_PYTHON_ADDON) candidates.push(process.env.CC_PYTHON_ADDON);
  // In the repo, the freshly built artifact beats any stale package
  // prebuilt left by prepare-publish; installed packages never have the
  // repo marker and use their own bin/.
  if (inRepo) candidates.push(repoAddon);
  candidates.push(path.join(__dirname, 'bin', 'cc_python-' + plat + '.node'));
  candidates.push(path.join(__dirname, 'bin', 'cc_python.node'));
  if (!inRepo) candidates.push(repoAddon);
  for (const c of candidates) if (fs.existsSync(c)) return c;
  throw new Error(
    'concurrent-c-python: no addon for ' + plat + ' (reinstall to run the source ' +
    'build, build it with `ccc build npm/cc-python/src/cc_python.ccs`, or ' +
    'set CC_PYTHON_ADDON); looked at: ' + candidates.join(', '));
}

const native = require(locateAddon());

const HANDLE = Symbol('cc-python-handle');
const KWARGS = Symbol('cc-python-kwargs');

function unwrapArg(a) {
  return (a !== null && a !== undefined && a[HANDLE]) ? a[HANDLE] : a;
}

/* Split positionals from a trailing kwargs(...) marker.  A trailing plain
 * object stays positional (Python dict) — only the marker means keywords.
 * Shared by in-process and isolated call sites. */
function splitCallArgs(args) {
  const list = args ? Array.prototype.slice.call(args) : [];
  for (let i = 0; i < list.length; i++) {
    const a = list[i];
    if (a !== null && a !== undefined && a[KWARGS] !== undefined) {
      if (i !== list.length - 1)
        throw new Error(
            'concurrent-c-python: kwargs(...) must be the last argument');
      return { pos: list.slice(0, -1), kw: a[KWARGS] };
    }
  }
  return { pos: list, kw: null };
}

function prepareCallArgs(bridge, rawArgs) {
  const split = splitCallArgs(rawArgs);
  return {
    pos: split.pos.map((a) => prepareArg(bridge, a)),
    kw: split.kw
      ? Object.fromEntries(
            Object.keys(split.kw).map((k) => [k, prepareArg(bridge, split.kw[k])]))
      : null,
  };
}

function inprocInvoke(bridge, fnHandle, rawArgs) {
  const { pos, kw } = prepareCallArgs(bridge, rawArgs);
  if (kw)
    return native.invoke_kw(bridge._dom, fnHandle, pos, kw);
  return native.invoke(bridge._dom, fnHandle, pos);
}

function maybeWarnHandles(bridge) {
  bridge._mintCount = (bridge._mintCount || 0) + 1;
  if ((bridge._mintCount & 0xff) !== 0 || bridge._handleWarn) return;
  try {
    const n = native.stats(bridge._dom);
    if (n >= 5000) {
      bridge._handleWarn = true;
      console.warn(
          'concurrent-c-python: ' + n +
          ' live handles — FinalizationRegistry only runs after event-loop ' +
          'turns. In tight sync loops call py.release(h) or use `using` / ' +
          'destroy(); see README "Handles and GC".');
    }
  } catch (e) { /* closed */ }
}

/* Host JS functions passed into Python receive bare napi Externals for
 * non-scalar args.  Without a Proxy those are inert (`x[0]` → undefined)
 * and a scipy objective can "converge" on a constant.  Wrap once so every
 * callback arg materializes the same way call results do — missing attrs
 * then throw via getattr, matching String(proxy). */
const HOST_CB = Symbol('cc-python-host-cb');
function wrapHostCallback(bridge, fn) {
  if (typeof fn !== 'function' || fn[HANDLE]) return fn;
  if (fn[HOST_CB]) return fn[HOST_CB]; /* cached wrap, or self if already wrap */
  const wrapped = function (...args) {
    return fn.apply(this, args.map((x) => materialize(bridge, x)));
  };
  wrapped[HOST_CB] = wrapped;
  fn[HOST_CB] = wrapped;
  return wrapped;
}
/* Plain arrays/objects cross as list/dict; Set/Map/Date/class instances
 * refuse (no silent empty-dict / wrong-shape conversion).  Cycle + depth
 * guards must run in JS — a circular object otherwise blows the raw call
 * stack before the native nesting check can fire. */
function assertBridgeValue(a, stack) {
  if (a === null || a === undefined) return;
  const t = typeof a;
  if (t !== 'object') return;
  if (a[HANDLE]) return;
  if (ArrayBuffer.isView(a)) return;
  if (!stack) stack = [];
  if (stack.length > 32)
    throw new Error('cc-python: argument nesting exceeds 32 levels');
  for (let i = 0; i < stack.length; i++) {
    if (stack[i] === a)
      throw new Error(
          'cc-python: unsupported argument (circular reference)');
  }
  stack.push(a);
  try {
    if (Array.isArray(a)) {
      for (let i = 0; i < a.length; i++) assertBridgeValue(a[i], stack);
      return;
    }
    const proto = Object.getPrototypeOf(a);
    if (proto !== Object.prototype && proto !== null) {
      throw new Error(
          'cc-python: unsupported argument (pass numbers, BigInt, strings, ' +
          'booleans, null/undefined, plain arrays/objects, typed arrays, ' +
          'or bridge handles)');
    }
    for (const k of Object.keys(a)) assertBridgeValue(a[k], stack);
  } finally {
    stack.pop();
  }
}

function prepareArg(bridge, a) {
  assertBridgeValue(a);
  if (typeof a === 'function' && !(HANDLE in a))
    return wrapHostCallback(bridge, a);
  return unwrapArg(a);
}

function rethrowImportHint(err, isolated) {
  const msg = String(err && err.message || err);
  if (!/No module named|ModuleNotFoundError/i.test(msg)) throw attachPyType(err);
  // Broker already attaches the isolated hint; do not double-append.
  if (isolated && /child python|venvPath/i.test(msg)) throw err;
  if (!isolated && /isolated: true|usePython/i.test(msg)) throw err;
  const hint = isolated
    ? ' — install into this child python, or pass create({ isolated: true, ' +
      'python: venvPath })'
    : ' — in-process libpython may not see system site-packages; try ' +
      'create({ isolated: true }) for ambient python3, or ' +
      'usePython("/path/to/venv") before create()';
  const e = new Error(msg.replace(/\s*$/, '') + hint);
  if (err && err.stack) e.stack = err.stack;
  throw attachPyType(e);
}

function flattenCrossErrMessage(msg) {
  let s = String(msg || '');
  for (;;) {
    const m = /^(?:RuntimeError:\s*|Error:\s*|python:\s*invoke:\s*)+/i
        .exec(s);
    if (!m) break;
    s = s.slice(m[0].length);
  }
  return s;
}

/* Surface Python exception class on the Error: `.pyType` and, when the
 * napi `code` was still a CC_ERR_* badge, replace it with the class name. */
function attachPyType(err) {
  if (!err || typeof err !== 'object') return err;
  if (err.message) {
    const flat = flattenCrossErrMessage(err.message);
    if (flat && flat !== err.message) {
      try { err.message = flat; } catch (e) { /* ignore */ }
    }
  }
  if (err.pyType) return err;
  let ty = null;
  if (typeof err.code === 'string' && err.code && !/^CC_ERR_/.test(err.code) &&
      err.code !== 'PythonError')
    ty = err.code;
  if (!ty) {
    const m = /python:\s*[^:]+:\s*([A-Za-z_][A-Za-z0-9_]*)\b/
        .exec(String(err.message || ''));
    if (m) ty = m[1];
  }
  if (!ty) {
    const m = /^([A-Za-z_][A-Za-z0-9_]*)\b/.exec(String(err.message || ''));
    if (m && /Error$|Exception$|Exit$/.test(m[1])) ty = m[1];
  }
  if (!ty) return err;
  try { err.pyType = ty; } catch (e) { /* ignore */ }
  if (typeof err.code === 'string' && /^CC_ERR_/.test(err.code)) {
    try { err.code = ty; } catch (e) { /* ignore */ }
  }
  return err;
}
function pyCall(fn) {
  try { return fn(); }
  catch (e) { throw attachPyType(e); }
}

// Scalars arrive as JS scalars; a held reference arrives as an External
// (typeof 'object') and gets a proxy of its own — the same
// materialization rule at every boundary crossing.  TypedArrays/Buffers
// from callback buffer-copy must NOT be wrapped (they are real JS values).
function materialize(bridge, r) {
  if (r === null || typeof r !== 'object') return r;
  if (r[HANDLE]) return r;
  if (ArrayBuffer.isView(r) || Array.isArray(r)) return r;
  if (typeof Buffer !== 'undefined' && Buffer.isBuffer(r)) return r;
  return wrap(bridge, r);
}

function wrap(bridge, handle) {
  maybeWarnHandles(bridge);
  // A function target so `apply` traps; the closure over `bridge` is the
  // strong reference that keeps the domain collectable only when every
  // proxy from it is unreachable too.
  const target = function () {};
  target[HANDLE] = handle;
  /* Cache getattr results so `mod.fn(x)` in a hot loop does not mint a
   * fresh method handle per call (reclaimed only under GC pressure).
   * Bumped on any release() so a cached proxy cannot outlive its box. */
  const attrCache = new Map();
  let cacheGen = bridge._attrCacheGen || 0;
  /* ownKeys snapshot for the current reflection walk (GOPD answers from
   * this without re-fetching; cleared on set / next ownKeys). */
  let keysSnap = null;
  let mappingFlag = undefined; /* undefined | true | false */
  const inspectCustom = Symbol.for('nodejs.util.inspect.custom');
  /* util.inspect reads the symbol off the target (not only the get trap)
   * for callable proxies — install both. Display = str(), not toJS. */
  target[inspectCustom] = () => pyCall(() =>
    materialize(bridge, native.invoke(bridge._dom, bridge._str(), [handle])));

  function pyGetAttr(prop) {
    return pyCall(() => materialize(
        bridge, native.getattr(bridge._dom, handle, prop)));
  }
  function pyInvoke(fnHandle, args) {
    return pyCall(() => materialize(
        bridge, inprocInvoke(bridge, fnHandle, args)));
  }
  function pyGetItem(prop) {
    return pyInvoke(unwrapArg(pyGetAttr('__getitem__')), [prop]);
  }
  function isMapping() {
    if (mappingFlag !== undefined) return mappingFlag;
    try {
      pyGetAttr('keys');
      pyGetAttr('__getitem__');
      mappingFlag = true;
    } catch (e) {
      mappingFlag = false;
    }
    return mappingFlag;
  }
  /* Mapping membership — __contains__; do not fall back to getattr (that
   * would make methods look like keys). */
  function pyContains(prop) {
    if (!isMapping()) return false;
    try {
      return !!pyInvoke(unwrapArg(pyGetAttr('__contains__')), [prop]);
    } catch (e) {
      return false;
    }
  }
  function pyOwnKeys() {
    if (!isMapping()) return [];
    try {
      const keysFn = pyGetAttr('keys');
      const view = pyInvoke(unwrapArg(keysFn), []);
      const out = [];
      for (const k of view) {
        if (typeof k === 'string') out.push(k);
        else if (typeof k === 'number' || typeof k === 'bigint')
          out.push(String(k));
      }
      return out;
    } catch (e) {
      return [];
    }
  }
  function readProp(prop) {
    /* Keys win on mappings: {'get': 1} → 1, not the bound method.
     * Methods: builtins.getattr (documented escape hatch). */
    if (isMapping() && pyContains(prop)) return pyGetItem(prop);
    return pyGetAttr(prop);
  }
  function materializeStrict() {
    return pyCall(() => native.to_js(bridge._dom, handle));
  }

  return new Proxy(target, {
    get(t, prop) {
      if (prop === HANDLE) return handle;
      if (prop === 'then') return undefined; // not a thenable
      /* One materializer: toJSON === toJS (strict JSON-safe deep copy). */
      if (prop === 'toJS' || prop === 'toJSON') {
        return materializeStrict;
      }
      if (prop === Symbol.toPrimitive || prop === 'toString' ||
          prop === inspectCustom) {
        return () => pyInvoke(bridge._str(), [handle]);
      }
      if (prop === 'toTypedArray') {
        return () => pyCall(
            () => native.to_typed_array(bridge._dom, handle));
      }
      if (prop === Symbol.iterator) {
        return function () {
          const it = pyInvoke(
              unwrapArg(pyGetAttr('__iter__')), []);
          return {
            next() {
              try {
                /* CPython 3: tp_iternext is `__next__`, not `next`. */
                return { value: it.__next__(), done: false };
              } catch (e) {
                attachPyType(e);
                if (/StopIteration/i.test(String(e && e.message)))
                  return { value: undefined, done: true };
                throw e;
              }
            },
            [Symbol.iterator]() { return this; },
          };
        };
      }
      if (typeof prop !== 'string') return undefined;
      if ((bridge._attrCacheGen || 0) !== cacheGen) {
        attrCache.clear();
        cacheGen = bridge._attrCacheGen || 0;
      }
      if (attrCache.has(prop)) return attrCache.get(prop);
      const v = readProp(prop);
      /* Proxies are function-targets → typeof 'function', not 'object'. */
      if (v != null && v[HANDLE] &&
          (typeof v === 'object' || typeof v === 'function'))
        attrCache.set(prop, v);
      return v;
    },
    has(t, prop) {
      if (prop === HANDLE) return true;
      if (typeof prop !== 'string') return false;
      return pyContains(prop);
    },
    ownKeys(t) {
      /* Function targets require length/name/prototype in ownKeys. */
      const base = Reflect.ownKeys(t);
      keysSnap = pyOwnKeys();
      for (let i = 0; i < keysSnap.length; i++) {
        if (base.indexOf(keysSnap[i]) < 0) base.push(keysSnap[i]);
      }
      return base;
    },
    getOwnPropertyDescriptor(t, prop) {
      if (prop === 'length' || prop === 'name' || prop === 'prototype' ||
          prop === inspectCustom)
        return Reflect.getOwnPropertyDescriptor(t, prop);
      if (typeof prop !== 'string')
        return Reflect.getOwnPropertyDescriptor(t, prop);
      const keys = keysSnap || pyOwnKeys();
      if (keys.indexOf(prop) < 0 && !pyContains(prop)) return undefined;
      /* Accessor: values come from [[Get]] (spread/assign); no eager
       * materialize on Object.keys. */
      return {
        configurable: true,
        enumerable: true,
        get() { return readProp(prop); },
        set(v) {
          keysSnap = null;
          attrCache.delete(prop);
          try {
            pyCall(() => native.setattr(
                bridge._dom, handle, prop, prepareArg(bridge, v)));
          } catch (e) {
            const si = pyGetAttr('__setitem__');
            pyInvoke(unwrapArg(si), [prop, prepareArg(bridge, v)]);
          }
        },
      };
    },
    set(t, prop, value) {
      if (typeof prop !== 'string') return false;
      keysSnap = null;
      try {
        pyCall(() => native.setattr(
            bridge._dom, handle, prop, prepareArg(bridge, value)));
        attrCache.delete(prop);
        return true;
      } catch (e) {
        /* Mappings: d.k = v → __setitem__ when setattr refuses. */
        try {
          const si = pyGetAttr('__setitem__');
          pyInvoke(unwrapArg(si), [prop, prepareArg(bridge, value)]);
          attrCache.delete(prop);
          return true;
        } catch (e2) {
          throw attachPyType(e);
        }
      }
    },
    apply(t, thisArg, args) {
      return pyInvoke(handle, args);
    },
  });
}

class Bridge {
  constructor() {
    this._dom = native.create();
    this._strHandle = null;
  }
  // THE async primitive: task(bridgeCallable) binds it to the domain's
  // executor lane (latent — first task call starts it) and returns an
  // async function.  Every call through it is a Promise; everything
  // else on the bridge stays synchronous.  FIFO within a domain,
  // parallel across domains (per-interpreter GILs).  A plain JS closure
  // is the reserved recorded-batch form (parameterized graphs) — not
  // implemented yet, and says so.
  task(fn) {
    if (typeof fn === 'function' && !(HANDLE in fn)) {
      throw new Error(
        'concurrent-c-python: batch thunks (recorded graphs) are not implemented ' +
        'yet — pass a bridge callable like py.task(np.linalg.norm)');
    }
    const h = unwrapArg(fn);
    if (h === null || h === undefined || typeof h !== 'object') {
      throw new Error('concurrent-c-python: task wants a bridge callable');
    }
    const bridge = this;
    return (...args) => {
      try {
        const { pos, kw } = prepareCallArgs(bridge, args);
        const p = kw
          ? native.invoke_async_kw(bridge._dom, h, pos, kw)
          : native.invoke_async(bridge._dom, h, pos);
        return p.then((r) => materialize(bridge, r));
      } catch (e) {
        return Promise.reject(e); // a closed bridge rejects, never throws
      }
    };
  }
  _str() {
    if (!this._strHandle) {
      const b = native.pyimport(this._dom, 'builtins');
      this._strHandle = native.getattr(this._dom, b, 'str');
    }
    return this._strHandle;
  }
  import(name) {
    if (native.closed(this._dom))
      throw new Error('concurrent-c-python: bridge is closed');
    try {
      return wrap(this, native.pyimport(this._dom, name));
    } catch (e) {
      rethrowImportHint(e, false);
    }
  }
  release(proxy) {
    this._attrCacheGen = (this._attrCacheGen || 0) + 1;
    return native.release(this._dom, unwrapArg(proxy));
  }
  stats() {
    return native.stats(this._dom);
  }
  get closed() {
    return native.closed(this._dom);
  }
  // Always a Promise: resolved immediately when no lane ever started,
  // after the revoke-then-drain when one did.
  destroy() {
    this._strHandle = null;
    return native.close_async(this._dom);
  }
  close() {
    return this.destroy();
  }
  /* Sync `using` requires dispose to finish revocation before the next
   * statement — fire-and-forget destroy() left the domain usable. */
  [Symbol.dispose]() {
    this._strHandle = null;
    native.close(this._dom);
  }
  [Symbol.asyncDispose]() {
    return this.destroy();
  }
}

/* ---- isolated domains: a FULL CPython per child process ------------- */

// Same calling convention as in-process: a default call blocks this
// thread until the child answers; py.task(fn) is the Promise door.
// Isolated is crash isolation and a different Python, not a different
// await discipline. Mixing a blocking call with in-flight py.task on
// this domain is refused (the blocking pump owns the pipe). A child
// crash still rejects in-flight task() calls.

const RHANDLE = Symbol('cc-python-remote');
const TA_KIND = new Map([
  [Float64Array, 'f64'], [Float32Array, 'f32'],
  [Int32Array, 'i32'], [BigInt64Array, 'i64'], [Uint8Array, 'u8'],
]);
const TA_CTOR = {
  f64: Float64Array, f32: Float32Array,
  i32: Int32Array, i64: BigInt64Array, u8: Uint8Array,
};

// Big buffers spill through shared memory (tmpfs where available):
// one memcpy per side instead of base64's inflate-encode-parse-decode.
// Each bridge gets a PRIVATE 0700 directory (predictable names in a
// shared /dev/shm invite pre-creation races and umask-dependent
// exposure); files are 0600 and exclusive-create.  The receiver
// consumes-and-unlinks; the sender sweeps its own after the request
// settles, and the whole directory goes with the bridge.
const SHM_SPILL = 1 << 16;
const SHM_BASE = process.env.CC_PY_SHM_DIR ||
                 (fs.existsSync('/dev/shm') ? '/dev/shm'
                                            : require('os').tmpdir());
let shmSeq = 0;

function resolvePythonExe(spec) {
  const asVenv = (dir) => {
    for (const b of ['python', 'python3']) {
      const p = path.join(dir, 'bin', b);
      if (fs.existsSync(p)) return p;
    }
    return null;
  };
  if (spec) {
    const abs = path.resolve(spec);
    if (fs.existsSync(path.join(abs, 'pyvenv.cfg'))) {
      const exe = asVenv(abs);
      if (exe) return exe;
      throw new Error('concurrent-c-python: venv has no bin/python: ' + abs);
    }
    if (fs.existsSync(abs)) return abs;
    throw new Error('concurrent-c-python: python does not exist: ' + abs);
  }
  // Ambient, mirroring the in-process order: VIRTUAL_ENV, ./.venv, PATH.
  if (process.env.VIRTUAL_ENV) {
    const exe = asVenv(process.env.VIRTUAL_ENV);
    if (exe) return exe;
    throw new Error('concurrent-c-python: VIRTUAL_ENV has no bin/python: ' +
                    process.env.VIRTUAL_ENV);
  }
  if (fs.existsSync(path.join('.venv', 'pyvenv.cfg'))) {
    const exe = asVenv(path.resolve('.venv'));
    if (exe) return exe;
    throw new Error('concurrent-c-python: ./.venv has no bin/python');
  }
  return 'python3';
}

/* Isolated dropped proxies never auto-released — register so GC can
 * release child handles (best-effort; explicit release() remains sharp). */
const isoFinalizers = (typeof FinalizationRegistry !== 'undefined')
  ? new FinalizationRegistry((entry) => {
      try {
        if (entry.bridge && !entry.bridge._closed && entry.h != null)
          entry.bridge._req({ op: 'release', h: entry.h }).catch(() => {});
      } catch (e) { /* bridge already gone */ }
    })
  : null;

function rwrap(bridge, h, chain) {
  const target = function () {};
  target[RHANDLE] = { h, chain, bridge };
  const inspectCustom = Symbol.for('nodejs.util.inspect.custom');
  const attrCache = new Map();
  let cacheGen = bridge._attrCacheGen || 0;

  function materializeReply(r) {
    try { return bridge._materialize(r); }
    catch (e) { throw attachPyType(e); }
  }
  function callp(path, args) {
    try {
      return materializeReply(bridge._reqSync(Object.assign(
          { op: 'callp', h, path }, bridge._callPayload(args || []))));
    } catch (e) { throw attachPyType(e); }
  }
  function getp(path) {
    try {
      return materializeReply(bridge._reqSync({ op: 'getp', h, path }));
    } catch (e) { throw attachPyType(e); }
  }
  const strOf = () => {
    try {
      return bridge._reqSync({ op: 'str', h, path: chain }).v;
    } catch (e) { throw attachPyType(e); }
  };
  target[inspectCustom] = strOf;

  const proxy = new Proxy(target, {
    get(t, prop) {
      if (prop === RHANDLE) return t[RHANDLE];
      if (prop === 'then') return undefined;
      if (prop === 'str') return strOf;
      if (prop === 'toTypedArray') {
        return () => {
          try {
            return materializeReply(
                bridge._reqSync({ op: 'ta', h, path: chain }));
          } catch (e) { throw attachPyType(e); }
        };
      }
      if (prop === Symbol.toPrimitive || prop === 'toString' ||
          prop === inspectCustom)
        return strOf;
      if (prop === 'toJS' || prop === 'toJSON') {
        return () => {
          try {
            const r = bridge._reqSync({ op: 'tojs', h, path: chain });
            if (r && r.v !== undefined) return bridge._decode(r.v);
            return bridge._materialize(r);
          } catch (e) { throw attachPyType(e); }
        };
      }
      if (prop === Symbol.iterator) {
        return function () {
          const it = callp(chain.concat('__iter__'), []);
          return {
            next() {
              try {
                return { value: it.__next__(), done: false };
              } catch (e) {
                attachPyType(e);
                if (/StopIteration/i.test(String(e && e.message)))
                  return { value: undefined, done: true };
                throw e;
              }
            },
            [Symbol.iterator]() { return this; },
          };
        };
      }
      if (typeof prop !== 'string') return undefined;
      if ((bridge._attrCacheGen || 0) !== cacheGen) {
        attrCache.clear();
        cacheGen = bridge._attrCacheGen || 0;
      }
      if (attrCache.has(prop)) return attrCache.get(prop);
      const v = getp(chain.concat(prop));
      if (v != null && v[RHANDLE] &&
          (typeof v === 'object' || typeof v === 'function'))
        attrCache.set(prop, v);
      return v;
    },
    set(t, prop, value) {
      if (typeof prop !== 'string') return false;
      try {
        callp(chain.concat('__setattr__'), [prop, value]);
        attrCache.delete(prop);
        return true;
      } catch (e) {
        try {
          callp(chain.concat('__setitem__'), [prop, value]);
          attrCache.delete(prop);
          return true;
        } catch (e2) {
          throw attachPyType(e);
        }
      }
    },
    apply(t, thisArg, args) {
      return callp(chain, args);
    },
  });
  /* Only root handles (no lazy chain) — releasing a path is meaningless. */
  if (isoFinalizers && chain.length === 0 && h != null)
    isoFinalizers.register(proxy, { bridge, h });
  return proxy;
}

class ProcBridge {
  constructor(opts) {
    const exe = resolvePythonExe(opts && opts.python);
    this._pythonExe = exe;
    this._pending = new Map(); // request id -> {settle, reject}
    this._nextReq = 1;
    this._cbs = new Map();
    this._nextCb = 1;
    this._closed = false;
    this._dead = false;
    this._closePending = false;
    this._cbInflight = 0;
    this._closeWaiters = [];
    this._destroyPromise = null;
    this._shmOut = [];
    this._trackShm = true;
    this._shmDir = fs.mkdtempSync(
        path.join(SHM_BASE, 'ccpy-' + process.pid + '-'));
    this._syncDepth = 0;
    this._taskInflight = 0;
    this._repBuf = Buffer.alloc(0);
    // The wire lives on fds 3 (requests) / 4 (replies); stdio is
    // inherited, so user print() reaches the real stdout and can never
    // collide with a protocol reply.
    this._child = spawn(exe, [path.join(__dirname, 'broker.py')], {
      stdio: ['inherit', 'inherit', 'inherit', 'pipe', 'pipe'],
      env: Object.assign({}, process.env, { CC_PY_SHM_DIR: this._shmDir }),
    });
    this._child.on('error', (e) => this._die('cannot spawn ' + exe +
                                             ': ' + e.message));
    this._child.on('exit', () => this._die('bridge is closed (the ' +
                                           'python child exited)'));
    // Abrupt peer death (SIGABRT / hard kill) surfaces as stream 'error'
    // (often ECONNRESET on Linux) before a clean EOF.  Without listeners
    // Node throws and takes the host down; route into idempotent _die.
    const wireDie = (side) => (e) => {
      const code = e && e.code ? e.code : 'error';
      this._die('bridge is closed (the python child exited; wire ' +
                side + ' ' + code + ')');
    };
    if (this._child.stdio[3])
      this._child.stdio[3].on('error', wireDie('req'));
    if (this._child.stdio[4])
      this._child.stdio[4].on('error', wireDie('rep'));
    const rep = this._child.stdio[4];
    this._repFd = (rep && rep._handle && typeof rep._handle.fd === 'number')
      ? rep._handle.fd : -1;
    rep.on('data', (chunk) => this._pushChunk(chunk));
    rep.on('error', wireDie('rep'));
  }

  _die(why) {
    if (this._dead) return;
    this._dead = true;
    this._closed = true;
    this._closePending = false;
    const pending = [...this._pending.values()];
    this._pending.clear();
    for (const p of pending) p.reject(new Error('concurrent-c-python: ' + why));
    for (const w of this._closeWaiters.splice(0)) w();
    this._cbs.clear();
    // The child is gone; anything unconsumed in the private spill dir
    // is garbage.
    try { fs.rmSync(this._shmDir, { recursive: true, force: true }); }
    catch (e) { /* already swept */ }
  }

  _pushChunk(chunk) {
    this._repBuf = Buffer.concat([this._repBuf, Buffer.from(chunk)]);
    for (;;) {
      const nl = this._repBuf.indexOf(10);
      if (nl < 0) break;
      const line = this._repBuf.subarray(0, nl).toString('utf8');
      this._repBuf = Buffer.from(this._repBuf.subarray(nl + 1));
      this._online(line);
    }
  }

  _pumpSyncUntil(pred) {
    const stream = this._child && this._child.stdio[4];
    if (!stream)
      throw new Error('concurrent-c-python: bridge is closed');
    this._syncDepth++;
    if (this._syncDepth === 1 && typeof stream.pause === 'function')
      stream.pause();
    try {
      while (!pred()) {
        let buf;
        while (typeof stream.read === 'function' &&
               (buf = stream.read()) !== null)
          this._pushChunk(buf);
        if (pred()) break;
        if (this._dead) break;
        const fd = this._repFd;
        if (typeof fd !== 'number' || fd < 0)
          throw new Error('concurrent-c-python: isolated reply fd is missing');
        try {
          this._pushChunk(native.read_pipe_sync(fd));
        } catch (e) {
          this._die('bridge is closed (the python child exited)');
          break;
        }
      }
    } finally {
      this._syncDepth--;
      if (this._syncDepth === 0 && typeof stream.resume === 'function')
        stream.resume();
    }
  }

  _online(line) {
    // The reply fd is the broker's alone — nothing else can write here,
    // so an unparseable line is a wire bug, not user output.
    let obj;
    try { obj = JSON.parse(line); }
    catch (e) { return this._die('protocol violation (unparseable reply)'); }
    if (obj && obj.cb) {
      // A callback request from the child, arriving mid-call. Sync
      // default calls run the JS function on this thread (thenables
      // refuse — use py.task). task() keeps the Promise path so async
      // callbacks can await. destroy() from inside the cb defers stdin
      // teardown until cbr is sent — otherwise the broker hangs on its
      // sync read.
      const fn = this._cbs.get(obj.cbid);
      const run = () => {
        if (!fn) throw new Error('unknown callback ' + obj.cbid);
        return fn(...(obj.args || []).map((a) => this._decode(a)));
      };
      const sendCbr = (r) => {
        const prev = this._trackShm;
        this._trackShm = false;
        try { this._send({ cbr: this._encode(r) }); }
        finally { this._trackShm = prev; }
      };
      const sendErr = (e) => this._send({ e: String(e && e.message || e) });
      this._cbInflight++;
      if (this._syncDepth > 0) {
        try {
          const ret = run();
          if (ret && typeof ret.then === 'function') {
            throw new Error(
              'concurrent-c-python: a synchronous bridge call cannot wait on a ' +
              'Promise — call through py.task(fn) and the callback may return one');
          }
          sendCbr(ret);
        } catch (e) {
          sendErr(e);
        } finally {
          this._cbInflight--;
          if (this._closePending && this._cbInflight === 0)
            this._tearDownChild();
        }
        return;
      }
      Promise.resolve()
        .then(run)
        .then(sendCbr, sendErr)
        .finally(() => {
          this._cbInflight--;
          if (this._closePending && this._cbInflight === 0)
            this._tearDownChild();
        });
      return;
    }
    // Replies pair by request id.  An unpaired line is a wire bug —
    // except during teardown, when the id-less farewell echo is fine.
    const p = obj && obj.id !== undefined ? this._pending.get(obj.id)
                                          : undefined;
    if (!p) {
      if (this._closed || this._dead) return;
      return this._die('protocol violation (unpaired reply)');
    }
    this._pending.delete(obj.id);
    p.settle(obj);
  }

  _send(obj) {
    // Allow cbr after destroy-from-callback (closePending); block else.
    if (this._dead || (this._closed && !this._closePending)) return;
    const wire = this._child && this._child.stdio[3];
    if (!wire || wire.destroyed || wire.writableEnded) return;
    try {
      wire.write(JSON.stringify(obj) + '\n');
    } catch (e) { /* torn down mid-flight */ }
  }

  _req(obj) {
    if (this._closed)
      return Promise.reject(new Error('concurrent-c-python: bridge is closed'));
    // Args were _encode'd before this call; take ownership of whatever
    // spill paths they staged so a pipelined sibling settle cannot
    // unlink them (Promise.all + multi-MB args).
    const owned = this._shmOut.splice(0);
    return new Promise((resolve, reject) => {
      const sweep = () => {
        // The child unlinks spill files as it decodes them; this sweep
        // only matters when it died first (ENOENT is the normal case).
        for (const p of owned) {
          try { fs.unlinkSync(p); } catch (e) { /* consumed */ }
        }
      };
      obj.id = this._nextReq++;
      this._pending.set(obj.id, {
        settle: (r) => {
          sweep();
          if (r && r.e !== undefined) {
            try { rethrowImportHint(new Error(r.e), true); }
            catch (e) { reject(e); }
          } else resolve(r);
        },
        reject: (e) => { sweep(); reject(e); },
      });
      this._send(obj);
    });
  }

  _reqSync(obj) {
    if (this._closed)
      throw new Error('concurrent-c-python: bridge is closed');
    // Blocking pump owns the reply pipe. A sibling py.task callback that
    // returns a Promise cannot be honored while we are inside it — refuse
    // here, at the blocking call, not inside that callback.
    if (this._taskInflight > 0) {
      throw new Error(
          'concurrent-c-python: cannot block on this isolated domain while ' +
          'py.task is in flight — overlap with py.task only');
    }
    const owned = this._shmOut.splice(0);
    const sweep = () => {
      for (const p of owned) {
        try { fs.unlinkSync(p); } catch (e) { /* consumed */ }
      }
    };
    let done = false, result, error;
    obj.id = this._nextReq++;
    this._pending.set(obj.id, {
      settle: (r) => {
        sweep();
        done = true;
        if (r && r.e !== undefined) {
          try { rethrowImportHint(new Error(r.e), true); }
          catch (e) { error = e; }
        } else result = r;
      },
      reject: (e) => { sweep(); done = true; error = e; },
    });
    this._send(obj);
    this._pumpSyncUntil(() => done || this._dead);
    if (error) throw error;
    if (!done)
      throw new Error('concurrent-c-python: bridge is closed');
    return result;
  }

  // Split a JS argument list into wire args + keyword args.  kwargs(...)
  // is explicit and last — a trailing plain object stays a positional
  // dict, never silently reinterpreted.
  _callPayload(args) {
    const n = args.length;
    let kw = null;
    for (let i = 0; i < n; i++) {
      const a = args[i];
      if (a === null || a === undefined || a[KWARGS] === undefined) continue;
      if (i !== n - 1) {
        throw new Error(
            'concurrent-c-python: kwargs(...) must be the last argument');
      }
      kw = {};
      for (const k of Object.keys(a[KWARGS])) {
        kw[k] = this._encode(a[KWARGS][k]);
      }
    }
    const pos = (kw ? args.slice(0, -1) : args)
      .map((a) => this._encode(a));
    return kw ? { args: pos, kw } : { args: pos };
  }

  _encode(v) {
    if (v === null || v === undefined) return null;
    if (v[KWARGS] !== undefined) {
      throw new Error(
          'concurrent-c-python: kwargs(...) is a call-site marker, not a ' +
          'value — it cannot nest inside another argument');
    }
    const t = typeof v;
    if (t === 'number') {
      if (Object.is(v, -0)) return { $nf: '-0' };
      if (Number.isFinite(v)) return v;
      return { $nf: v === Infinity ? 'inf' : v === -Infinity ? '-inf'
                                           : 'nan' };
    }
    if (t === 'string' || t === 'boolean') return v;
    if (t === 'bigint') {
      if (v >= -(2n ** 53n) && v <= 2n ** 53n) return Number(v);
      return { $bi: v.toString() };
    }
    if (t === 'function') {
      if (v[RHANDLE]) {
        const r = v[RHANDLE];
        if (r.bridge && r.bridge !== this)
          throw new Error('concurrent-c-python: handle belongs to another bridge');
        if (r.chain.length)
          throw new Error('concurrent-c-python: pass the handle, not an ' +
                          'unresolved attribute path');
        if (r.h === null)
          throw new Error('concurrent-c-python: this module has not landed yet');
        return { $h: r.h };
      }
      const id = this._nextCb++;
      this._cbs.set(id, v);
      return { $f: id };
    }
    // Buffers are Uint8Array subclasses with their own constructor —
    // they cross as u8, same as on the cc-node wire.
    const kind = Buffer.isBuffer(v) ? 'u8' : TA_KIND.get(v.constructor);
    if (kind) {
      const buf = Buffer.from(v.buffer, v.byteOffset, v.byteLength);
      if (v.byteLength > SHM_SPILL) {
        const p = path.join(this._shmDir, 's' + (++shmSeq));
        fs.writeFileSync(p, buf, { flag: 'wx', mode: 0o600 });
        if (this._trackShm) this._shmOut.push(p);
        return { $shm: p, t: kind };
      }
      return { $ta: kind, b64: buf.toString('base64') };
    }
    if (Array.isArray(v)) return v.map((x) => this._encode(x));
    // Unawaited py.task() results are Promises — say so before the
    // generic "unsupported" line (the usual dict()/exec footgun).
    if (t === 'object' && v !== null && typeof v.then === 'function' &&
        !v[RHANDLE]) {
      throw new Error(
          'concurrent-c-python: got a Promise — await py.task(...) results ' +
          'before passing them as arguments');
    }
    if (t === 'object' && (v.constructor === Object || !v.constructor)) {
      const o = {};
      for (const k of Object.keys(v)) {
        if (k.startsWith('$'))
          throw new Error('concurrent-c-python: object keys starting with $ are ' +
                          'reserved on the wire');
        o[k] = this._encode(v[k]);
      }
      return o;
    }
    throw new Error(
        'concurrent-c-python: unsupported argument for an isolated domain ' +
        '(numbers, strings, booleans, typed arrays, plain objects/arrays, ' +
        'functions, or this domain\'s handles — same-domain handles chain)');
  }

  _decode(v) {
    if (v === null || typeof v !== 'object') return v;
    if (v.$h !== undefined) return rwrap(this, v.$h, []);
    if (v.$nf !== undefined) {
      if (v.$nf === '-0') return -0;
      return v.$nf === 'inf' ? Infinity : v.$nf === '-inf' ? -Infinity
                                                           : NaN;
    }
    if (v.$bi !== undefined) return BigInt(v.$bi);
    if (v.$shm !== undefined || v.shm !== undefined) {
      const path = v.$shm !== undefined ? v.$shm : v.shm;
      const kind = v.t || v.$t;
      const buf = fs.readFileSync(path);
      try { fs.unlinkSync(path); } catch (e) { /* consumed */ }
      const C = TA_CTOR[kind];
      return new C(buf.buffer, buf.byteOffset,
                   buf.byteLength / C.BYTES_PER_ELEMENT);
    }
    if (v.$ta !== undefined || v.ta !== undefined) {
      const kind = v.$ta !== undefined ? v.$ta : v.ta;
      const buf = Buffer.from(v.b64, 'base64');
      const C = TA_CTOR[kind];
      return new C(buf.buffer, buf.byteOffset,
                   buf.byteLength / C.BYTES_PER_ELEMENT);
    }
    if (Array.isArray(v)) return v.map((x) => this._decode(x));
    const o = {};
    for (const k of Object.keys(v)) o[k] = this._decode(v[k]);
    return o;
  }

  _materialize(r) {
    if (r.h !== undefined) return rwrap(this, r.h, []);
    if (r.shm !== undefined) {
      const buf = fs.readFileSync(r.shm);
      try { fs.unlinkSync(r.shm); } catch (e) { /* consumed */ }
      const C = TA_CTOR[r.t];
      return new C(buf.buffer, buf.byteOffset,
                   buf.byteLength / C.BYTES_PER_ELEMENT);
    }
    if (r.ta !== undefined) return this._decode(r);
    return this._decode(r.v);
  }

  // import waits for the child (same as in-process). Attribute hops
  // are eager getattr so np.linalg.norm is a real handle, not a path.
  import(name) {
    if (this._dead || this._closed)
      throw new Error('concurrent-c-python: bridge is closed');
    try {
      const r = this._reqSync({ op: 'import', name });
      return rwrap(this, r.h, []);
    } catch (e) {
      rethrowImportHint(e, true);
    }
  }
  task(fn) {
    const r = fn && fn[RHANDLE];
    if (!r) {
      throw new Error('concurrent-c-python: task wants a bridge callable');
    }
    const bridge = this;
    return (...args) => {
      try {
        const payload = bridge._callPayload(args);
        const call = (h) => bridge._req(Object.assign(
            { op: 'callp', h, path: r.chain || [] }, payload))
          .then((x) => bridge._materialize(x),
                (e) => { throw attachPyType(e); });
        if (r.h == null)
          return Promise.reject(new Error(
            'concurrent-c-python: this module has not landed yet'));
        bridge._taskInflight++;
        return call(r.h).finally(() => { bridge._taskInflight--; });
      } catch (e) {
        return Promise.reject(e);
      }
    };
  }
  release(proxy) {
    const r = proxy && proxy[RHANDLE];
    if (!r) throw new Error('concurrent-c-python: not a handle');
    if (r.chain && r.chain.length)
      throw new Error(
        'concurrent-c-python: attribute paths are not held handles');
    this._attrCacheGen = (this._attrCacheGen || 0) + 1;
    return this._reqSync({ op: 'release', h: r.h }).v;
  }
  stats() {
    return this._reqSync({ op: 'stats' }).v;
  }
  get closed() {
    return this._closed;
  }
  // Named to avoid colliding with the module-level python() introspection
  // (which describes the IN-PROCESS runtime, a different thing).
  get pythonExe() {
    return this._pythonExe;
  }
  destroy() {
    if (this._dead) return Promise.resolve();
    if (this._destroyPromise) return this._destroyPromise;
    this._closed = true;
    this._destroyPromise = new Promise((resolve) => {
      this._closeWaiters.push(resolve);
    });
    if (this._cbInflight > 0) {
      // Farewell must not land in the broker's cbr slot.
      this._closePending = true;
      return this._destroyPromise;
    }
    this._tearDownChild();
    return this._destroyPromise;
  }

  _tearDownChild() {
    this._closePending = false;
    this._closed = true;
    try {
      const wire = this._child.stdio[3];
      if (wire && !wire.destroyed && !wire.writableEnded) {
        try { wire.write(JSON.stringify({ op: 'close' }) + '\n'); }
        catch (e) { /* ignore */ }
        try { wire.end(); } catch (e) { /* ignore */ }
      }
    } catch (e) { /* ignore */ }
    const child = this._child;
    const t = setTimeout(() => { try { child.kill('SIGKILL'); } catch (e) {} },
                         2000);
    t.unref();
    // _die (via exit) resolves closeWaiters; also resolve if already dead.
    if (this._dead) {
      clearTimeout(t);
      for (const w of this._closeWaiters.splice(0)) w();
    } else {
      this._child.once('exit', () => clearTimeout(t));
    }
  }
  close() { return this.destroy(); }
  /* Sync using: mark closed and tear down immediately (SIGKILL backup). */
  [Symbol.dispose]() {
    if (this._dead) return;
    this._closed = true;
    try {
      const child = this._child;
      if (child && !child.killed) child.kill('SIGKILL');
    } catch (e) { /* ignore */ }
    this._tearDownChild();
    this._dead = true;
    for (const w of this._closeWaiters.splice(0)) w();
    for (const [, p] of this._pending) {
      try { p.reject(new Error('concurrent-c-python: bridge is closed')); }
      catch (e) { /* ignore */ }
    }
    this._pending.clear();
  }
  [Symbol.asyncDispose]() { return this.destroy(); }
}

module.exports = {
  version: require('./package.json').version,

  // Keyword arguments for a Python call, explicitly marked and last:
  //   fmt(1, kwargs({ sep: '+' }))     →  fmt(1, sep='+')
  // A trailing plain object stays a positional dict — only this marker
  // means keywords.  Works in-process and on the isolated wire.
  kwargs(obj) {
    if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) {
      throw new TypeError(
          'concurrent-c-python: kwargs wants a plain object of keyword ' +
          'arguments');
    }
    return { [KWARGS]: obj };
  },

  create(opts) {
    if (opts && opts.isolated) return new ProcBridge(opts);
    if (opts && opts.python)
      throw new Error(
        'concurrent-c-python: the in-process runtime is process-wide — choose it ' +
        'with usePython(...); per-domain python needs { isolated: true }');
    return new Bridge();
  },

  // Choose the process Python from code — a venv dir, an interpreter
  // executable, or a libpython path.  Valid until the first create()
  // loads a runtime; after that a matching choice is a no-op and a
  // different one throws (one runtime per process — per-domain runtimes
  // arrive with process-isolated domains).  Beats CC_LIBPYTHON and the
  // ambient VIRTUAL_ENV / ./.venv forms.
  usePython(spec) {
    if (typeof spec !== 'string' || !spec)
      throw new TypeError('concurrent-c-python: usePython wants a path (venv dir, ' +
                          'python executable, or libpython)');
    native.use_python(path.resolve(spec));
  },

  // { loaded, version, lib, how } — how the runtime was (or will be) chosen.
  python() {
    const [version, lib, how] = String(native.runtime_desc()).split('|');
    return { loaded: !!version, version, lib, how };
  },
};
