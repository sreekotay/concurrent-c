#!/usr/bin/env node
/* cc-node broker: the Node end of Python's JS bridge.
 *
 * Line-delimited JSON over stdio, strict request/response.  Handles are
 * integers into one table; results follow the bridge materialization
 * rule — plain data (finite numbers, strings, booleans, null, arrays
 * and plain objects of the same) crosses as a value, everything else
 * stays a handle.  A thenable result is awaited before the reply, so
 * async package APIs need nothing special from the Python side.  A
 * Python callable crosses as {$f: id}; invoking it sends a nested `cb`
 * request and BLOCKS on a synchronous read for the answer — legal
 * because the protocol is strictly alternating, so nothing else can be
 * in flight.  stdin EOF is the host vanishing: exit. */
'use strict';

const fs = require('fs');
const { createRequire } = require('module');

/* Resolve packages from the HOST's cwd — `npm install lodash` next to
 * your Python program is the point. */
const requireCwd = createRequire(process.cwd() + '/');

/* ---- one buffered reader over fd 0, sync and async ---- */
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
      n = fs.readSync(0, chunk, 0, chunk.length, null);
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
    fs.read(0, chunk, 0, chunk.length, null, (err, n) => {
      if (err) return err.code === 'EOF' ? resolve(null) : reject(err);
      if (n === 0) return resolve(null);
      rbuf.data = Buffer.concat([rbuf.data, chunk.subarray(0, n)]);
      resolve(readLineAsync());
    });
  });
}

function send(obj) {
  fs.writeSync(1, JSON.stringify(obj) + '\n');
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

function encodeResult(v) {
  if (v === undefined) return { u: 1 };
  if (typeof v === 'number' && !Number.isFinite(v))
    return { nf: Number.isNaN(v) ? 'nan' : v > 0 ? 'inf' : '-inf' };
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
        case 'require': r = requireCwd(req.name); break;
        case 'import': r = await import(req.name); break;
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
