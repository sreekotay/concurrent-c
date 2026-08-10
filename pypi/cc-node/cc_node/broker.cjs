#!/usr/bin/env node
/* cc-node broker: the Node end of Python's JS bridge.
 *
 * Line-delimited JSON on DEDICATED wire fds, strict request/response —
 * stdin/stdout/stderr stay the user's, so console.log in evaluated
 * code reaches the real stdout and can never collide with a protocol
 * reply.  Requests arrive on fd 3, replies leave on fd 4; a parent
 * that cannot pin fd numbers (python's pass_fds keeps the parent's
 * numbering) says which via CC_WIRE_IN / CC_WIRE_OUT.  Handles are
 * integers into one table; results follow the bridge materialization
 * rule — plain data (finite numbers, strings, booleans, null, arrays
 * and non-empty plain objects of the same) crosses as a value; an
 * empty plain object stays a handle (so `eval('({})')` remains a live
 * JS object — materializing it to Python `{}` dropped property access).
 * A thenable result is awaited before the reply, so async package APIs
 * need nothing special from the Python side.  A Python callable crosses
 * as {$f: id}; invoking it sends a nested `cb` request and BLOCKS on a
 * synchronous read for the answer — legal because the protocol is
 * strictly alternating, so nothing else can be in flight.  EOF on the
 * request fd is the host vanishing: exit.
 *
 * cc/include/ccc/script/js.cch embeds this file verbatim (the CC
 * isolated tier speaks the same wire); js_iso_smoke pins the two
 * byte-identical, so an edit here must land there too. */
'use strict';

const fs = require('fs');
const { createRequire } = require('module');

const IN_FD = Number(process.env.CC_WIRE_IN || 3);
const OUT_FD = Number(process.env.CC_WIRE_OUT || 4);

/* Resolve packages from the HOST's cwd — `npm install lodash` next to
 * your Python program is the point. */
const requireCwd = createRequire(process.cwd() + '/');

/* ---- one buffered reader over the request fd, sync and async ---- */
const rbuf = { data: Buffer.alloc(0) };

function takeLine() {
  const i = rbuf.data.indexOf(10);
  if (i < 0) return null;
  const line = rbuf.data.subarray(0, i).toString('utf8');
  rbuf.data = rbuf.data.subarray(i + 1);
  return line;
}

function readLineSync() {
  for (;;) {
    const l = takeLine();
    if (l !== null) return l;
    const chunk = Buffer.alloc(65536);
    let n = 0;
    try {
      n = fs.readSync(IN_FD, chunk, 0, chunk.length, null);
    } catch (e) {
      if (e.code === 'EAGAIN') continue;
      if (e.code === 'EOF') return null;
      throw e;
    }
    if (n === 0) return null;
    rbuf.data = Buffer.concat([rbuf.data, chunk.subarray(0, n)]);
  }
}

function readLineAsync() {
  const l = takeLine();
  if (l !== null) return Promise.resolve(l);
  return new Promise((resolve, reject) => {
    const chunk = Buffer.alloc(65536);
    fs.read(IN_FD, chunk, 0, chunk.length, null, (err, n) => {
      if (err) return err.code === 'EOF' ? resolve(null) : reject(err);
      if (n === 0) return resolve(null);
      rbuf.data = Buffer.concat([rbuf.data, chunk.subarray(0, n)]);
      resolve(readLineAsync());
    });
  });
}

function send(obj) {
  try {
    fs.writeSync(OUT_FD, JSON.stringify(obj) + '\n');
  } catch (e) {
    if (e.code === 'EPIPE') process.exit(0); /* host went away */
    throw e;
  }
}

/* ---- handles + materialization ---- */
const handles = new Map();
let nextH = 1;
const put = (v) => {
  const id = nextH++;
  handles.set(id, v);
  return id;
};
const getH = (id) => {
  if (!handles.has(id)) throw new Error('cc-node: unknown or released handle');
  return handles.get(id);
};

/* Plain data crosses by value; non-finite numbers are NOT plain (JSON
 * would silently null them — they go as tagged scalars or handles). */
function isPlain(v, depth) {
  if (depth > 16) return false;
  if (v === null) return true;
  const t = typeof v;
  if (t === 'number') return Number.isFinite(v);
  if (t === 'string' || t === 'boolean') return true;
  if (t !== 'object') return false;
  const proto = Object.getPrototypeOf(v);
  if (Array.isArray(v)) return v.every((x) => isPlain(x, depth + 1));
  if (proto === Object.prototype || proto === null)
    return Object.values(v).every((x) => isPlain(x, depth + 1));
  return false;
}

/* Typed buffers cross as tagged bytes: small inline as base64, big
 * through the shared-memory spill (one memcpy per side; the receiver
 * consumes-and-unlinks; files are 0600, exclusive-create, inside the
 * host's private bridge dir).  Same discipline as cc-python's wire. */
const TA_KIND = new Map([
  [Float64Array, 'f64'], [Float32Array, 'f32'],
  [Int32Array, 'i32'], [BigInt64Array, 'i64'], [Uint8Array, 'u8'],
]);
const TA_CTOR = {
  f64: Float64Array, f32: Float32Array,
  i32: Int32Array, i64: BigInt64Array, u8: Uint8Array,
};
const SHM_SPILL = 1 << 16;
const SHM_DIR = process.env.CC_NODE_SHM_DIR ||
                (fs.existsSync('/dev/shm') ? '/dev/shm'
                                           : require('os').tmpdir());
let shmSeq = 0;

function encodeBuffer(kind, buf) {
  if (buf.byteLength > SHM_SPILL) {
    const p = require('path').join(
        SHM_DIR, 'ccnode-c' + process.pid + '-' + (++shmSeq));
    fs.writeFileSync(p, buf, { flag: 'wx', mode: 0o600 });
    return { shm: p, t: kind };
  }
  return { ta: kind, b64: buf.toString('base64') };
}

function encodeResult(v) {
  if (v === undefined) return { u: 1 };
  if (typeof v === 'number' && !Number.isFinite(v))
    return { nf: Number.isNaN(v) ? 'nan' : v > 0 ? 'inf' : '-inf' };
  if (v !== null && typeof v === 'object') {
    const kind = TA_KIND.get(v.constructor);
    if (kind)
      return encodeBuffer(kind,
                          Buffer.from(v.buffer, v.byteOffset, v.byteLength));
    if (Buffer.isBuffer(v)) return encodeBuffer('u8', v);
    // Empty plain {} / Object.create(null) stay handles — callers mint
    // bags for later property use.  Non-empty plain objects still cross
    // by value (data returns).
    const proto = Object.getPrototypeOf(v);
    if (!Array.isArray(v) &&
        (proto === Object.prototype || proto === null) &&
        Object.keys(v).length === 0)
      return { h: put(v) };
  }
  if (v === null || isPlain(v, 0)) return { v };
  return { h: put(v) };
}

/* ---- Python callables ---- */
let nextCbId = 1;

function makeCallback(fid) {
  const f = (...args) => {
    const cbid = nextCbId++;
    send({ cb: fid, cbid, args: args.map(encodeResult) });
    const line = readLineSync();
    if (line === null) throw new Error('cc-node: host went away');
    const m = JSON.parse(line);
    if (m.cbr !== cbid)
      throw new Error('cc-node: protocol violation during callback');
    if (m.e !== undefined) throw new Error(m.e);
    return decodeVal(m.v);
  };
  return f;
}

function decodeVal(a) {
  if (a && typeof a === 'object') {
    if (a.$h !== undefined) return getH(a.$h);
    if (a.$f !== undefined) return makeCallback(a.$f);
    if (a.$nf !== undefined)
      return a.$nf === 'nan' ? NaN : a.$nf === 'inf' ? Infinity : -Infinity;
    if (a.$ta !== undefined || a.$shm !== undefined) {
      let buf;
      if (a.$shm !== undefined) {
        buf = fs.readFileSync(a.$shm);
        try { fs.unlinkSync(a.$shm); } catch (e) { /* consumed */ }
      } else {
        buf = Buffer.from(a.b64, 'base64');
      }
      const C = TA_CTOR[a.t !== undefined ? a.t : a.$ta];
      return new C(buf.buffer, buf.byteOffset,
                   buf.byteLength / C.BYTES_PER_ELEMENT);
    }
    if (Array.isArray(a)) return a.map(decodeVal);
    const o = {};
    for (const k of Object.keys(a)) o[k] = decodeVal(a[k]);
    return o;
  }
  return a;
}

/* ---- dispatch ---- */
async function main() {
  for (;;) {
    const line = await readLineAsync();
    if (line === null) process.exit(0);
    let req;
    try {
      req = JSON.parse(line);
    } catch {
      continue;
    }
    const id = req.id;
    try {
      let r;
      switch (req.op) {
        case 'require':
          try {
            r = requireCwd(req.name);
          } catch (e) {
            if (e && e.code === 'MODULE_NOT_FOUND') {
              throw new Error(
                  String(e.message) +
                  ' — npm install into this working directory\'s ' +
                  'node_modules (require resolves from cwd), or pass ' +
                  'create(node=...) for a different Node');
            }
            throw e;
          }
          break;
        case 'import':
          try {
            r = await import(req.name);
          } catch (e) {
            const msg = String(e && e.message !== undefined ? e.message : e);
            if (/Cannot find module|ERR_MODULE_NOT_FOUND/i.test(msg)) {
              throw new Error(
                  msg +
                  ' — install the package for this Node (cwd node_modules ' +
                  'or a path import), or pass create(node=...)');
            }
            throw e;
          }
          break;
        case 'eval': r = (0, eval)(req.src); break;
        case 'get': {
          const o = getH(req.h);
          const v = o[req.name];
          r = typeof v === 'function' ? v.bind(o) : v;
          break;
        }
        case 'call': {
          const f = getH(req.h);
          if (typeof f !== 'function')
            throw new Error('cc-node: handle is not callable');
          r = f(...(req.args || []).map(decodeVal));
          break;
        }
        case 'str': r = String(getH(req.h)); break;
        case 'release': handles.delete(req.h); r = handles.size; break;
        case 'stats': r = handles.size; break;
        case 'close': send({ id, v: true }); process.exit(0); break;
        default: throw new Error('cc-node: unknown op ' + req.op);
      }
      if (r && typeof r.then === 'function') r = await r; /* async is free */
      send({ id, ...encodeResult(r) });
    } catch (e) {
      send({ id, e: String(e && e.message !== undefined ? e.message : e) });
    }
  }
}

main();
