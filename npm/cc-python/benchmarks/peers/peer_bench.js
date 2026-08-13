#!/usr/bin/env node
/**
 * concurrent-c-python vs pymport / node-calls-python / pythonia.
 *
 * Each in-process embed gets its own Node child — two libpythons in one
 * process will fight. Isolated peers (cc-iso, pythonia) are children too.
 *
 *   cd npm/cc-python/benchmarks/peers && npm install
 *   VIRTUAL_ENV=/path/to/venv node peer_bench.js
 *
 * DX notes + snippets: README.md in this directory.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { createRequire } = require('module');
const { pathToFileURL } = require('url');

const HERE = __dirname;
const REPO = path.resolve(HERE, '..', '..', '..');
const HELPERS = path.join(HERE, 'helpers.py');
const reqPeers = createRequire(path.join(HERE, 'peer_bench.js'));

const PEERS = ['cc-inproc', 'cc-iso', 'pymport', 'ncp', 'pythonia', 'js'];

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const v = process.argv[i + 1];
  if (!v || v.startsWith('--')) return true;
  return v;
}

function pickVenv() {
  const cand = [
    process.env.CC_PYTHON_VENV,
    process.env.VIRTUAL_ENV,
    path.join(REPO, '.venv'),
  ].filter(Boolean);
  for (const d of cand) {
    if (fs.existsSync(path.join(d, 'pyvenv.cfg'))) return d;
  }
  return null;
}

function venvSitePackages(venv) {
  if (!venv) return null;
  const lib = path.join(venv, 'lib');
  if (!fs.existsSync(lib)) return null;
  for (const d of fs.readdirSync(lib)) {
    const sp = path.join(lib, d, 'site-packages');
    if (fs.existsSync(sp)) return sp;
  }
  return null;
}

function fillVec(n) {
  const a = new Float64Array(n);
  const b = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    a[i] = (i % 97) * 0.01;
    b[i] = (i % 89) * 0.01;
  }
  return [a, b];
}

function fillMat(n) {
  const A = new Float64Array(n * n);
  const B = new Float64Array(n * n);
  for (let i = 0; i < A.length; i++) {
    A[i] = ((i * 13) % 97) * 0.01;
    B[i] = ((i * 17) % 89) * 0.01;
  }
  return [A, B];
}

function jsDot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

function jsMatmulSum(A, B, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) {
      let s = 0;
      for (let k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j];
      sum += s;
    }
  }
  return sum;
}

function jsNorm(a) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * a[i];
  return Math.sqrt(s);
}

async function timeNs(iters, fn) {
  const first = fn();
  if (first && typeof first.then === 'function') await first;
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < iters; i++) {
    const r = fn();
    if (r && typeof r.then === 'function') await r;
  }
  return Number(process.hrtime.bigint() - t0) / iters;
}

function tryReq(name) {
  try { return reqPeers(name); }
  catch (e) { return null; }
}

function unwrapMaybe(x) {
  if (x == null) return x;
  if (typeof x === 'number' || typeof x === 'string' || typeof x === 'boolean' ||
      typeof x === 'bigint')
    return x;
  if (Array.isArray(x)) return x;
  /* pymport proxify keeps scalars as PyObject; toJS is on the hidden slot.
     Do not read unknown attrs on pythonia proxies — that is a round trip. */
  let desc;
  try { desc = Object.getOwnPropertyDescriptor(x, '__PyObject__'); }
  catch (e) { desc = null; }
  if (desc && desc.value && typeof desc.value.toJS === 'function') {
    try { return desc.value.toJS(); } catch (e) { /* keep */ }
  }
  return x;
}

async function awaitAll(x) {
  if (x && typeof x.then === 'function') x = await x;
  return unwrapMaybe(x);
}

/* ------------------------------------------------------------------ */
/* Adapters                                                           */
/* ------------------------------------------------------------------ */

async function setupCc(isolated) {
  const ccpy = require(path.join(HERE, '..', '..'));
  const venv = pickVenv();
  if (!isolated && venv) ccpy.usePython(venv);
  const t0 = process.hrtime.bigint();
  const py = isolated
    ? ccpy.create({ isolated: true, python: venv || undefined })
    : ccpy.create();
  const spawnNs = Number(process.hrtime.bigint() - t0);
  const sys = py.import('sys');
  sys.path.insert(0, HERE);
  const t1 = process.hrtime.bigint();
  const h = py.import('helpers');
  const importNs = Number(process.hrtime.bigint() - t1);
  const hasNp = !!h.has_numpy();
  let describeTa = null;
  try { describeTa = String(h.describe(new Float64Array([1, 2, 3]))); }
  catch (e) { describeTa = 'err:' + (e.message || e).toString().slice(0, 60); }
  return {
    kind: isolated ? 'isolated' : 'inproc',
    sync: true,
    proxy: true,
    kwargs: 'kwargs({k:v}) last',
    spawn_ms: spawnNs / 1e6,
    import_helpers_ms: importNs / 1e6,
    numpy: hasNp,
    describe_ta: describeTa,
    async sqrt(x) { return h.sqrt(x); },
    async dot(a, b) { return h.dot(a, b); },
    async matmulSum(A, B, n) { return h.matmul_sum(A, B, n); },
    async norm(a) { return h.norm(a); },
    async raiseKey() { return h.raise_key(); },
    async doSorted(xs) {
      return h.do_sorted(xs, ccpy.kwargs({ reverse: true }));
    },
    async apply(fn, x) { return h.apply(fn, x); },
    async destroy() { await py.destroy(); },
  };
}

async function setupPymport() {
  const mod = tryReq('pymport');
  if (!mod) throw new Error('not installed (npm i in peers/)');
  const { pymport, proxify } = mod;
  const t0 = process.hrtime.bigint();
  const sys = proxify(pymport('sys'));
  const spawnNs = Number(process.hrtime.bigint() - t0);
  const sp = venvSitePackages(pickVenv());
  if (sp) sys.path.insert(1, sp);
  sys.path.insert(1, HERE);
  const t1 = process.hrtime.bigint();
  const h = proxify(pymport('helpers'));
  const importNs = Number(process.hrtime.bigint() - t1);
  const hasNp = !!unwrapMaybe(h.has_numpy());
  let describeTa = null;
  try { describeTa = String(unwrapMaybe(h.describe(new Float64Array([1, 2, 3])))); }
  catch (e) { describeTa = 'err:' + (e.message || e).toString().slice(0, 60); }
  /* Float64Array becomes a bytearray; lists are the numeric path. */
  const asList = (ta) => Array.from(ta);
  return {
    kind: 'inproc',
    sync: true,
    proxy: true,
    kwargs: 'trailing {k:v}',
    spawn_ms: spawnNs / 1e6,
    import_helpers_ms: importNs / 1e6,
    numpy: hasNp,
    describe_ta: describeTa,
    buf: 'list-copy',
    async sqrt(x) { return unwrapMaybe(h.sqrt(x)); },
    async dot(a, b) { return unwrapMaybe(h.dot(asList(a), asList(b))); },
    async matmulSum(A, B, n) { return unwrapMaybe(h.matmul_sum(asList(A), asList(B), n)); },
    async norm(a) { return unwrapMaybe(h.norm(asList(a))); },
    async raiseKey() { return h.raise_key(); },
    async doSorted(xs) { return unwrapMaybe(h.do_sorted(xs, { reverse: true })); },
    async apply(fn, x) { return unwrapMaybe(h.apply(fn, x)); },
    async destroy() { /* GC; no domain door */ },
  };
}

async function setupNcp() {
  const mod = tryReq('node-calls-python');
  if (!mod) throw new Error('not installed (npm i in peers/)');
  const py = mod.interpreter;
  const venv = pickVenv();
  const sp = venvSitePackages(venv);
  if (sp && py.addImportPath) py.addImportPath(sp);
  const t0 = process.hrtime.bigint();
  const h = py.importSync(HELPERS);
  const spawnNs = Number(process.hrtime.bigint() - t0);
  const hasNp = !!py.callSync(h, 'has_numpy');
  let describeTa = null;
  try {
    describeTa = String(py.callSync(h, 'describe', new Float64Array([1, 2, 3])));
  } catch (e) {
    describeTa = 'err:' + (e.message || e).toString().slice(0, 60);
  }
  /* TypedArray → bytes here; lists are the honest numeric path. */
  const asList = (ta) => Array.from(ta);
  return {
    kind: 'inproc',
    sync: true,
    proxy: false,
    kwargs: '{k:v, __kwargs:true}',
    spawn_ms: spawnNs / 1e6,
    import_helpers_ms: spawnNs / 1e6,
    numpy: hasNp,
    describe_ta: describeTa,
    buf: 'list-copy',
    async sqrt(x) { return py.callSync(h, 'sqrt', x); },
    async dot(a, b) { return py.callSync(h, 'dot', asList(a), asList(b)); },
    async matmulSum(A, B, n) {
      return py.callSync(h, 'matmul_sum', asList(A), asList(B), n);
    },
    async norm(a) { return py.callSync(h, 'norm', asList(a)); },
    async raiseKey() { return py.callSync(h, 'raise_key'); },
    async doSorted(xs) {
      return py.callSync(h, 'do_sorted', xs, { reverse: true, __kwargs: true });
    },
    async apply(fn, x) { return py.callSync(h, 'apply', fn, x); },
    async destroy() { /* singleton interpreter */ },
  };
}

async function setupPythonia() {
  let pythoniaPath;
  try { pythoniaPath = reqPeers.resolve('pythonia'); }
  catch (e) { throw new Error('not installed (npm i in peers/)'); }
  const venv = pickVenv();
  if (venv) {
    process.env.VIRTUAL_ENV = venv;
    const exe = path.join(venv, 'bin', 'python');
    if (fs.existsSync(exe)) process.env.PYTHON_BIN = exe;
  }
  const t0 = process.hrtime.bigint();
  const { python } = await import(pathToFileURL(pythoniaPath).href);
  const spawnNs = Number(process.hrtime.bigint() - t0);
  const t1 = process.hrtime.bigint();
  const h = await python(HELPERS);
  const importNs = Number(process.hrtime.bigint() - t1);
  const hasNp = !!(await h.has_numpy());
  let describeTa = null;
  try {
    describeTa = String(await h.describe(new Float64Array([1, 2, 3])));
  } catch (e) {
    describeTa = 'err:' + (e.message || e).toString().slice(0, 60);
  }
  const asList = (ta) => Array.from(ta);
  async function pyia(x) {
    x = await x;
    if (x && typeof x === 'object' && typeof x.valueOf === 'function') {
      try {
        const v = await x.valueOf();
        if (v !== x) return v;
      } catch (e) { /* keep proxy */ }
    }
    return x;
  }
  return {
    kind: 'isolated',
    sync: false,
    proxy: true,
    kwargs: 'fn$(..., {k:v})',
    spawn_ms: spawnNs / 1e6,
    import_helpers_ms: importNs / 1e6,
    numpy: hasNp,
    describe_ta: describeTa,
    buf: 'list-copy',
    async sqrt(x) { return Number(await pyia(h.sqrt(x))); },
    async dot(a, b) { return Number(await pyia(h.dot(asList(a), asList(b)))); },
    async matmulSum(A, B, n) {
      return Number(await pyia(h.matmul_sum(asList(A), asList(B), n)));
    },
    async norm(a) { return Number(await pyia(h.norm(asList(a)))); },
    async raiseKey() { return h.raise_key(); },
    async doSorted(xs) { return pyia(h.do_sorted$(xs, { reverse: true })); },
    async apply(fn, x) { return pyia(h.apply(fn, x)); },
    async destroy() {
      try { python.exit(); } catch (e) { /* ignore */ }
    },
  };
}

async function setupJs() {
  return {
    kind: 'js',
    sync: true,
    proxy: false,
    kwargs: 'n/a',
    spawn_ms: 0,
    import_helpers_ms: 0,
    numpy: false,
    describe_ta: 'Float64Array',
    async sqrt(x) { return Math.sqrt(x); },
    async dot(a, b) { return jsDot(a, b); },
    async matmulSum(A, B, n) { return jsMatmulSum(A, B, n); },
    async norm(a) { return jsNorm(a); },
    async raiseKey() { const o = {}; return o.missing.x; },
    async doSorted(xs) { return xs.slice().sort((a, b) => b - a); },
    async apply(fn, x) { return fn(x); },
    async destroy() {},
  };
}

const SETUPS = {
  'cc-inproc': () => setupCc(false),
  'cc-iso': () => setupCc(true),
  pymport: setupPymport,
  ncp: setupNcp,
  pythonia: setupPythonia,
  js: setupJs,
};

function exceptName(e) {
  if (!e) return 'none';
  if (e.pyType) return String(e.pyType);
  const m = String(e.message || e);
  if (/KeyError/.test(m)) return 'KeyError';
  return e.name || e.constructor && e.constructor.name || 'Error';
}

async function runPeer(name) {
  const json = !!arg('json', false);
  const log = json ? (...a) => console.error(...a) : (...a) => console.log(...a);
  const out = {
    peer: name,
    ok: false,
    caps: {},
    times: {},
    skip: [],
    error: null,
  };

  let ad;
  try {
    ad = await SETUPS[name]();
  } catch (e) {
    out.error = (e && e.message) ? e.message : String(e);
    log('RESULT', name, 'setup', 'FAIL', out.error.split('\n')[0].slice(0, 120));
    if (json) console.log(JSON.stringify(out));
    return out;
  }

  out.caps = {
    kind: ad.kind,
    sync: ad.sync,
    proxy: ad.proxy,
    kwargs: ad.kwargs,
    numpy: ad.numpy,
    describe_ta: ad.describe_ta,
    buf: ad.buf || (ad.kind === 'js' ? 'native' : 'typedarray'),
    spawn_ms: +ad.spawn_ms.toFixed(2),
    import_helpers_ms: +ad.import_helpers_ms.toFixed(2),
  };
  log('RESULT', name, 'setup_ok', 'kind', ad.kind,
      'numpy', ad.numpy ? 'yes' : 'no',
      'describe_ta', ad.describe_ta,
      'spawn_ms', ad.spawn_ms.toFixed(2));

  const iso = ad.kind === 'isolated';
  const Nsqrt = iso ? 200 : 1000;
  const Nsmall = iso ? 100 : 1000;
  const Nbulk = 8;
  const Nmat = 5;

  async function row(key, iters, fn, check) {
    try {
      const got = await fn();
      if (check) check(got);
      const ns = await timeNs(iters, fn);
      out.times[key] = ns;
      const label = ns >= 1e6
        ? (ns / 1e6).toFixed(3) + ' ms'
        : (ns / 1e3).toFixed(2) + ' us';
      log('RESULT', name, key, label, 'ns', Math.round(ns));
    } catch (e) {
      out.skip.push(key);
      out.times[key] = null;
      log('RESULT', name, key, 'SKIP',
          (e.message || e).toString().split('\n')[0].slice(0, 100));
    }
  }

  await row('sqrt', Nsqrt, () => ad.sqrt(16), (v) => {
    if (Math.abs(Number(v) - 4) > 1e-9) throw new Error('sqrt got ' + v);
  });

  await row('kwargs_sorted', Nsmall, async () => {
    const v = await ad.doSorted([3, 1, 2]);
    return v;
  }, (v) => {
    const xs = Array.isArray(v) ? v : Array.from(v);
    if (Number(xs[0]) !== 3 || Number(xs[1]) !== 2 || Number(xs[2]) !== 1)
      throw new Error('sorted got ' + JSON.stringify(xs));
  });

  await row('except', Nsmall, async () => {
    try {
      await ad.raiseKey();
      throw new Error('expected throw');
    } catch (e) {
      if (/expected throw/.test(e.message)) throw e;
      return exceptName(e);
    }
  }, (v) => {
    out.caps.except_type = v;
  });

  await row('callback', Math.min(Nsmall, 200), () => ad.apply((x) => x + 1, 41),
            (v) => {
              if (Number(v) !== 42) throw new Error('callback got ' + v);
            });

  if (ad.numpy || name === 'js') {
    const [a16, b16] = fillVec(16);
    await row('dot16', Nsmall, () => ad.dot(a16, b16), (v) => {
      const want = jsDot(a16, b16);
      if (Math.abs(Number(v) - want) > 1e-6)
        throw new Error('dot16 ' + v + ' vs ' + want);
    });

    const [a1m, b1m] = fillVec(1e6);
    await row('dot1m', Nbulk, () => ad.dot(a1m, b1m), (v) => {
      const want = jsDot(a1m, b1m);
      if (Math.abs(Number(v) - want) > Math.abs(want) * 1e-6)
        throw new Error('dot1m mismatch');
    });

    const n = 128;
    const [A, B] = fillMat(n);
    const wantMat = name === 'js' ? jsMatmulSum(A, B, n) : null;
    await row('matmul128', name === 'js' ? 1 : Nmat, () => ad.matmulSum(A, B, n),
              (v) => {
                if (wantMat != null && Math.abs(Number(v) - wantMat) > 1e-3)
                  throw new Error('matmul mismatch');
              });

    await row('norm16', Nsmall, () => ad.norm(a16), (v) => {
      const want = jsNorm(a16);
      if (Math.abs(Number(v) - want) > 1e-6)
        throw new Error('norm ' + v + ' vs ' + want);
    });
  } else {
    for (const k of ['dot16', 'dot1m', 'matmul128', 'norm16']) {
      out.skip.push(k);
      log('RESULT', name, k, 'SKIP', 'no numpy');
    }
  }

  try { await ad.destroy(); }
  catch (e) { log('RESULT', name, 'destroy', 'WARN', (e.message || e).toString().slice(0, 80)); }

  out.ok = true;
  if (json) console.log(JSON.stringify(out));
  return out;
}

function fmtNs(ns) {
  if (ns == null) return '—';
  if (ns >= 1e6) return (ns / 1e6).toFixed(2) + 'ms';
  if (ns >= 1e3) return (ns / 1e3).toFixed(1) + 'µs';
  return Math.round(ns) + 'ns';
}

function printTable(results) {
  const cols = ['sqrt', 'dot16', 'dot1m', 'matmul128', 'norm16',
                'kwargs_sorted', 'except', 'callback'];
  const names = results.map((r) => r.peer);
  const width = Math.max(14, ...cols.map((c) => c.length), ...names.map((n) => n.length));
  const pad = (s, w) => String(s).padEnd(w || width);
  const kw = 22;
  console.log('\n== DX probes');
  console.log([pad('peer'), pad('kind'), pad('sync'), pad('proxy'),
               pad('kwargs', kw), pad('Float64Array→', 16), pad('except')].join(' '));
  for (const r of results) {
    if (!r.ok) {
      console.log(pad(r.peer), 'FAIL', (r.error || '').split('\n')[0].slice(0, 80));
      continue;
    }
    const c = r.caps;
    console.log(pad(r.peer), pad(c.kind), pad(c.sync ? 'sync' : 'await'),
                pad(c.proxy ? 'proxy' : 'explicit'),
                pad(c.kwargs, kw), pad(c.describe_ta, 16), pad(c.except_type || '—'));
  }

  console.log('\n== ns/call (idiomatic buffer path per peer)');
  console.log([pad('workload'), ...names.map(pad)].join(' '));
  for (const col of cols) {
    const cells = results.map((r) => pad(r.ok ? fmtNs(r.times[col]) : 'FAIL'));
    console.log([pad(col), ...cells].join(' '));
  }
  console.log('\n== spawn / import helpers');
  for (const r of results) {
    if (!r.ok) continue;
    console.log(r.peer, 'spawn_ms', r.caps.spawn_ms,
                'import_helpers_ms', r.caps.import_helpers_ms,
                'numpy', r.caps.numpy ? 'yes' : 'no');
  }
}

function runChild(name) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [__filename, '--peer', name, '--json'], {
      env: Object.assign({}, process.env, { OPENBLAS_NUM_THREADS: '1' }),
      cwd: REPO,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => { stdout += d; });
    child.stderr.on('data', (d) => {
      stderr += d;
      process.stderr.write(d);
    });
    const t = setTimeout(() => {
      try { child.kill('SIGKILL'); } catch (e) { /* ignore */ }
    }, 180000);
    child.on('close', (code) => {
      clearTimeout(t);
      const line = stdout.trim().split('\n').filter(Boolean).pop();
      try {
        const obj = JSON.parse(line);
        resolve(obj);
      } catch (e) {
        resolve({
          peer: name,
          ok: false,
          caps: {},
          times: {},
          skip: [],
          error: 'child exit ' + code + ' ' +
                 (stderr || stdout).split('\n')[0].slice(0, 160),
        });
      }
    });
  });
}

async function runAll() {
  const only = arg('only', null);
  const list = only
    ? String(only).split(',').map((s) => s.trim()).filter(Boolean)
    : PEERS;
  console.log('# cc-python peer bench');
  console.log('# date', new Date().toISOString());
  console.log('# host', process.platform, process.arch, 'node', process.version);
  console.log('# venv', pickVenv() || 'none');
  console.log('# peers', list.join(','));
  const results = [];
  for (const name of list) {
    console.log('\n--', name);
    results.push(await runChild(name));
  }
  printTable(results);
  for (const r of results) {
    if (!r.ok) {
      console.log('RESULT', r.peer, 'FAIL', (r.error || '').slice(0, 120));
      continue;
    }
    for (const [k, ns] of Object.entries(r.times)) {
      if (ns == null) continue;
      console.log('RESULT', r.peer, k, Math.round(ns));
    }
  }
  console.log('done');
}

(async () => {
  const peer = arg('peer', null);
  if (peer && peer !== true) {
    if (!SETUPS[peer]) {
      console.error('unknown peer', peer, 'want', PEERS.join('|'));
      process.exit(2);
    }
    const r = await runPeer(peer);
    process.exit(r.ok || arg('json', false) ? 0 : 1);
  } else {
    await runAll();
  }
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
