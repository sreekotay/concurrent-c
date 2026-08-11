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
const readline = require('readline');

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
  if (a !== null && a !== undefined && a[KWARGS] !== undefined) {
    throw new Error(
        'concurrent-c-python: kwargs(...) crosses only the isolated wire ' +
        'today — in-process calls are positional (bind keywords in Python, ' +
        'e.g. functools.partial)');
  }
  return (a !== null && a !== undefined && a[HANDLE]) ? a[HANDLE] : a;
}

function rethrowImportHint(err, isolated) {
  const msg = String(err && err.message || err);
  if (!/No module named|ModuleNotFoundError/i.test(msg)) throw err;
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
  throw e;
}

// Scalars arrive as JS scalars; a held reference arrives as an External
// (typeof 'object') and gets a proxy of its own — the same
// materialization rule at every boundary crossing.
function materialize(bridge, r) {
  return (r !== null && typeof r === 'object') ? wrap(bridge, r) : r;
}

function wrap(bridge, handle) {
  // A function target so `apply` traps; the closure over `bridge` is the
  // strong reference that keeps the domain collectable only when every
  // proxy from it is unreachable too.
  const target = function () {};
  target[HANDLE] = handle;
  return new Proxy(target, {
    get(t, prop) {
      if (prop === HANDLE) return handle;
      if (prop === 'then') return undefined; // not a thenable
      if (prop === Symbol.toPrimitive || prop === 'toString' ||
          prop === 'toJS') {
        return () => native.invoke(bridge._dom, bridge._str(), [handle]);
      }
      if (typeof prop !== 'string') return undefined;
      return materialize(bridge, native.getattr(bridge._dom, handle, prop));
    },
    apply(t, thisArg, args) {
      return materialize(bridge,
                         native.invoke(bridge._dom, handle,
                                       args.map(unwrapArg)));
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
        return native.invoke_async(bridge._dom, h, args.map(unwrapArg))
          .then((r) => materialize(bridge, r));
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
    try {
      return wrap(this, native.pyimport(this._dom, name));
    } catch (e) {
      rethrowImportHint(e, false);
    }
  }
  release(proxy) {
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
  [Symbol.dispose]() {
    this.destroy();
  }
  [Symbol.asyncDispose]() {
    return this.destroy();
  }
}

/* ---- isolated domains: a FULL CPython per child process ------------- */

// Cross-process is natively async (that was the point of building the
// async surface first): attribute chains extend LAZILY with no round
// trips, a call is a Promise, `await proxy` materializes an attribute
// value, and revocation is a rejected promise — a child crash included.

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

function rwrap(bridge, h, chain) {
  const target = function () {};
  target[RHANDLE] = { h, chain, bridge };
  return new Proxy(target, {
    get(t, prop) {
      if (prop === RHANDLE) return t[RHANDLE];
      if (prop === 'then') {
        // `await proxy` materializes the attribute value.
        if (chain.length === 0) return undefined; // module roots stay put
        return (resolve, reject) =>
          bridge._req({ op: 'getp', h, path: chain })
            .then((r) => resolve(bridge._materialize(r)), reject);
      }
      if (prop === 'str') {
        return () => bridge._req({ op: 'str', h, path: chain })
          .then((r) => r.v);
      }
      if (prop === 'toTypedArray') {
        // Materialize a buffer-shaped value: small inline, big through
        // the shm spill — one memcpy per side either way.
        return () => bridge._req({ op: 'ta', h, path: chain })
          .then((r) => bridge._materialize(r));
      }
      if (prop === Symbol.toPrimitive || prop === 'toString')
        return () => '[cc-python remote ' + h +
                     (chain.length ? '.' + chain.join('.') : '') + ']';
      if (typeof prop !== 'string') return undefined;
      return rwrap(bridge, h, chain.concat(prop)); // no round trip
    },
    apply(t, thisArg, args) {
      return bridge
        ._req(Object.assign({ op: 'callp', h, path: chain },
                            bridge._callPayload(args)))
        .then((r) => bridge._materialize(r));
    },
  });
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
    const rl = readline.createInterface({ input: this._child.stdio[4] });
    rl.on('line', (line) => this._online(line));
    rl.on('error', wireDie('rep'));
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

  _online(line) {
    // The reply fd is the broker's alone — nothing else can write here,
    // so an unparseable line is a wire bug, not user output.
    let obj;
    try { obj = JSON.parse(line); }
    catch (e) { return this._die('protocol violation (unparseable reply)'); }
    if (obj && obj.cb) {
      // A callback request from the child, arriving mid-call: run the JS
      // function (awaiting whatever it awaits) and reply on its line.
      // destroy() from inside the cb defers stdin teardown until cbr
      // is sent — otherwise the broker hangs on its sync read.
      const fn = this._cbs.get(obj.cbid);
      this._cbInflight++;
      Promise.resolve()
        .then(() => {
          if (!fn) throw new Error('unknown callback ' + obj.cbid);
          return fn(...(obj.args || []).map((a) => this._decode(a)));
        })
        .then((r) => {
          // Child consumes cbr spills; do not stage them on _shmOut or a
          // sibling _req settle may unlink before the broker reads.
          const prev = this._trackShm;
          this._trackShm = false;
          try { this._send({ cbr: this._encode(r) }); }
          finally { this._trackShm = prev; }
        },
              (e) => this._send({ e: String(e && e.message || e) }))
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
      if (Number.isFinite(v)) return v;
      return { $nf: v === Infinity ? 'inf' : v === -Infinity ? '-inf'
                                           : 'nan' };
    }
    if (t === 'string' || t === 'boolean') return v;
    if (t === 'bigint') {
      if (v >= -(2n ** 53n) && v <= 2n ** 53n) return Number(v);
      throw new Error('concurrent-c-python: bigint beyond 2^53 cannot cross the ' +
                      'wire yet');
    }
    if (t === 'function') {
      if (v[RHANDLE]) {
        const r = v[RHANDLE];
        if (r.bridge && r.bridge !== this)
          throw new Error('concurrent-c-python: handle belongs to another bridge');
        if (r.chain.length)
          throw new Error('concurrent-c-python: pass the awaited value, not an ' +
                          'attribute path');
        if (r.h === null)
          throw new Error('concurrent-c-python: this module has not landed yet — ' +
                          'await any use of it before passing it as an ' +
                          'argument');
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
    // Unawaited isolated call results are Promises — say so before the
    // generic "unsupported" line (the usual dict()/exec footgun).
    if (t === 'object' && v !== null && typeof v.then === 'function' &&
        !v[RHANDLE]) {
      throw new Error(
          'concurrent-c-python: got a Promise — await isolated call results ' +
          'before passing them as arguments (e.g. await builtins.dict())');
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
        'functions, or this domain\'s handles — same-domain handles chain; ' +
        'await call results, and keep exec namespaces as live handles via ' +
        'await builtins.dict())');
  }

  _decode(v) {
    if (v === null || typeof v !== 'object') return v;
    if (v.$h !== undefined) return rwrap(this, v.$h, []);
    if (v.$nf !== undefined)
      return v.$nf === 'inf' ? Infinity : v.$nf === '-inf' ? -Infinity
                                                           : NaN;
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

  // The surface, mirrored: import returns a proxy with NO round trip
  // (the request pipelines under the first use), calls are Promises.
  import(name) {
    const p = this._req({ op: 'import', name });
    // The import's handle arrives with the first operation that needs
    // it; a failed import surfaces there.  We pre-resolve eagerly so
    // chains stay synchronous:
    const bridge = this;
    const target = function () {};
    target[RHANDLE] = { h: null, chain: [], pending: p, bridge };
    p.then((r) => { target[RHANDLE].h = r.h; }, () => {});
    return new Proxy(target, {
      get(t, prop) {
        if (prop === RHANDLE) return t[RHANDLE];
        if (prop === 'then') return undefined;
        if (prop === 'str')
          return () => p.then((r) =>
            bridge._req({ op: 'str', h: r.h, path: [] }).then((x) => x.v));
        if (typeof prop !== 'string') return undefined;
        return rlazy(bridge, p, [prop]);
      },
      apply() {
        throw new Error('concurrent-c-python: a module is not callable');
      },
    });
  }
  task(fn) {
    // API uniformity with in-process domains: every isolated call is
    // already a task-shaped Promise.
    return (...args) => Promise.resolve(fn(...args));
  }
  release(proxy) {
    const r = proxy && proxy[RHANDLE];
    if (!r) return Promise.reject(new Error('concurrent-c-python: not a handle'));
    if (r.chain && r.chain.length)
      return Promise.reject(new Error(
        'concurrent-c-python: attribute paths are not held handles — await the ' +
        'value, then release what it returns'));
    const bridge = this;
    const settle = (h) => bridge._req({ op: 'release', h }).then((x) => x.v);
    return r.pending ? r.pending.then((x) => settle(x.h)) : settle(r.h);
  }
  stats() {
    return this._req({ op: 'stats' }).then((r) => r.v);
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
  [Symbol.dispose]() { this.destroy(); }
  [Symbol.asyncDispose]() { return this.destroy(); }
}

// A chain rooted on a not-yet-resolved import: still zero round trips
// to extend, one to use.
function rlazy(bridge, pending, chain) {
  const target = function () {};
  target[RHANDLE] = { h: null, chain, pending, bridge };
  return new Proxy(target, {
    get(t, prop) {
      if (prop === RHANDLE) return t[RHANDLE];
      if (prop === 'then') {
        return (resolve, reject) =>
          pending.then((r) =>
            bridge._req({ op: 'getp', h: r.h, path: chain })
              .then((x) => resolve(bridge._materialize(x)), reject),
            reject);
      }
      if (prop === 'str') {
        return () => pending.then((r) =>
          bridge._req({ op: 'str', h: r.h, path: chain }).then((x) => x.v));
      }
      if (prop === 'toTypedArray') {
        return () => pending.then((r) =>
          bridge._req({ op: 'ta', h: r.h, path: chain })
            .then((x) => bridge._materialize(x)));
      }
      if (prop === Symbol.toPrimitive || prop === 'toString')
        return () => '[cc-python remote .' + chain.join('.') + ']';
      if (typeof prop !== 'string') return undefined;
      return rlazy(bridge, pending, chain.concat(prop));
    },
    apply(t, thisArg, args) {
      const payload = bridge._callPayload(args); // validate before the hop
      return pending.then((r) =>
        bridge._req(Object.assign({ op: 'callp', h: r.h, path: chain },
                                  payload))
          .then((x) => bridge._materialize(x)));
    },
  });
}

module.exports = {
  version: require('./package.json').version,

  // Keyword arguments for a Python call, explicitly marked and last:
  //   fmt(1, kwargs({ sep: '+' }))     →  fmt(1, sep='+')
  // A trailing plain object stays a positional dict — only this marker
  // means keywords.  Isolated wire only (in-process refuses by name).
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
