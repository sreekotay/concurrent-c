/* cc-python: Python from Node over the Concurrent-C bridge.
 *
 *     const py = require('cc-python').create();   // an Isolation Domain
 *     const np = py.import('numpy');
 *     const s  = np.linalg.norm(new Float64Array([3, 4]));   // 5
 *     py.destroy();   // one sweep — every handle, arena, interpreter ref
 *
 * Proxies wrap opaque handles: attribute access is getattr, a call is
 * invoke, scalars come back as JS scalars and everything else as another
 * proxy.  Every proxy strong-refs its bridge, so the GC collects the
 * domain (and runs the same sweep destroy() runs) only when the whole
 * graph is unreachable.  `using py = ...` disposes at end of scope. */
'use strict';

const path = require('path');
const fs = require('fs');

function locateAddon() {
  const candidates = [];
  if (process.env.CC_PYTHON_ADDON) candidates.push(process.env.CC_PYTHON_ADDON);
  candidates.push(path.join(__dirname, 'bin', 'cc_python.node'));
  candidates.push(path.join(__dirname, '..', '..', 'bin', 'cc_python.node'));
  for (const c of candidates) if (fs.existsSync(c)) return c;
  throw new Error(
    'cc-python: cc_python.node not found (build it with ' +
    '`ccc build npm/cc-python/src/cc_python.ccs`, or set CC_PYTHON_ADDON); ' +
    'looked at: ' + candidates.join(', '));
}

const native = require(locateAddon());

const HANDLE = Symbol('cc-python-handle');

function unwrapArg(a) {
  return (a !== null && a !== undefined && a[HANDLE]) ? a[HANDLE] : a;
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
  _str() {
    if (!this._strHandle) {
      const b = native.pyimport(this._dom, 'builtins');
      this._strHandle = native.getattr(this._dom, b, 'str');
    }
    return this._strHandle;
  }
  import(name) {
    return wrap(this, native.pyimport(this._dom, name));
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
  destroy() {
    this._strHandle = null;
    native.close(this._dom);
  }
  close() {
    this.destroy();
  }
  [Symbol.dispose]() {
    this.destroy();
  }
}

module.exports = {
  create() { return new Bridge(); },
};
