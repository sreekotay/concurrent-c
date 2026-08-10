/* Seeded random walk over the in-process cc-python surface.
 *
 *   FUZZ_SEED=42 FUZZ_OPS=200 node --expose-gc stress/bridge/js_python_fuzz.js
 *   CHAOS_SCALE=quick|full|soak …   # default op budget if FUZZ_OPS unset
 *
 * Monitoring (so a hang is obvious — not a silent wait):
 *   FUZZ_TIMEOUT=60          wall seconds (default: quick 60 / full 180 / soak 600)
 *   FUZZ_HEARTBEAT_SECS=5    stderr pulse with last op + elapsed
 *   FUZZ_PROGRESS_OPS=25     stdout progress every N ops
 *   FUZZ_OP_TIMEOUT_MS=10000 per-await cap (destroy / task)
 *
 * Every RESULT line carries the seed so a failure is replayable:
 *   FUZZ_SEED=<n> node --expose-gc stress/bridge/js_python_fuzz.js
 *
 * Expected articulate errors (create-during-destroy, keep-past-return,
 * closed/released) are counted, not failures. Unexpected throws fail the walk.
 */
'use strict';

const path = require('path');

const SCALE = (process.env.CHAOS_SCALE || 'quick').toLowerCase();
const SOAK = SCALE === 'soak';
const FULL = SCALE === 'full' || SOAK;
const DEFAULT_OPS = SOAK ? 2000 : FULL ? 800 : 200;
const DEFAULT_TIMEOUT = SOAK ? 600 : FULL ? 180 : 60;
const OPS = Math.max(1, Number(process.env.FUZZ_OPS || DEFAULT_OPS) | 0);
const SEED = (process.env.FUZZ_SEED !== undefined && process.env.FUZZ_SEED !== '')
  ? (Number(process.env.FUZZ_SEED) >>> 0)
  : ((Date.now() ^ (process.pid << 16)) >>> 0);
const WALL_SECS = Math.max(5, Number(process.env.FUZZ_TIMEOUT || DEFAULT_TIMEOUT) | 0);
const HEARTBEAT_SECS = Math.max(1, Number(process.env.FUZZ_HEARTBEAT_SECS || 5) | 0);
const PROGRESS_OPS = Math.max(1, Number(process.env.FUZZ_PROGRESS_OPS || 25) | 0);
const OP_TIMEOUT_MS = Math.max(500, Number(process.env.FUZZ_OP_TIMEOUT_MS || 10000) | 0);

const T0 = Date.now();
const elapsed = () => ((Date.now() - T0) / 1000).toFixed(1);

function log(line) {
  // write (not console.log): shows up under tee/pipes without waiting for exit
  process.stdout.write(line + '\n');
}
function pulse(line) {
  process.stderr.write('[+' + elapsed() + 's] ' + line + '\n');
}

pulse('js_python_fuzz boot seed=' + SEED + ' ops=' + OPS + ' scale=' + SCALE
  + ' timeout=' + WALL_SECS + 's heartbeat=' + HEARTBEAT_SECS + 's');
pulse('loading npm/cc-python …');
const ccpy = require(path.join(__dirname, '../../npm/cc-python'));
pulse('addon loaded');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const gcNow = async () => {
  if (typeof global.gc === 'function') {
    global.gc();
    await sleep(0);
  }
};

function withTimeout(p, ms, label) {
  let t;
  const timeout = new Promise((_, rej) => {
    t = setTimeout(() => {
      rej(new Error('FUZZ_OP_TIMEOUT ' + label + ' after ' + ms + 'ms'
        + ' (seed=' + SEED + ')'));
    }, ms);
    if (t.unref) t.unref();
  });
  return Promise.race([Promise.resolve(p), timeout]).finally(() => clearTimeout(t));
}

/* mulberry32 */
function makeRng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const rng = makeRng(SEED);
const pick = (n) => (rng() * n) | 0;
const result = (fmt, ...args) => {
  const parts = args.map((a) => String(a));
  let i = 0;
  const body = fmt.replace(/%[sd]/g, () => parts[i++]);
  log('RESULT fuzz_seed=' + SEED + ' ' + body);
};
const ok = (name, cond) => {
  log('OK ' + name + ' ' + (cond ? 'true' : 'false'));
  return cond;
};

const OPS_LIST = [
  'sync_call',
  'task_call',
  'lease_fsum',
  'keep_past',
  'release',
  'destroy_await',
  'destroy_kick',
  'gc',
  'recreate',
  'stats',
];

let lastOp = 'boot';
let lastIdx = -1;
let wallTimer = null;
let hbTimer = null;

function armWatchdogs() {
  wallTimer = setTimeout(() => {
    pulse('FUZZ_TIMEOUT after ' + WALL_SECS + 's last_op=' + lastOp
      + '@' + lastIdx + ' seed=' + SEED);
    log('RESULT fuzz_seed=' + SEED + ' timeout 1 last_op=' + lastOp
      + ' last_idx=' + lastIdx);
    process.exit(124);
  }, WALL_SECS * 1000);
  wallTimer.unref?.();

  hbTimer = setInterval(() => {
    pulse('alive op=' + lastOp + '@' + lastIdx
      + ' ops_done=' + (lastIdx + 1) + '/' + OPS);
  }, HEARTBEAT_SECS * 1000);
  hbTimer.unref?.();
}

function disarmWatchdogs() {
  if (wallTimer) clearTimeout(wallTimer);
  if (hbTimer) clearInterval(hbTimer);
}

(async () => {
  armWatchdogs();
  log('js_python_fuzz seed=' + SEED + ' ops=' + OPS + ' scale=' + SCALE);
  result('start ops=%d scale=%s', OPS, SCALE);

  let py = null;
  let math = null;
  let handles = [];
  let destroyPending = null;
  let articulate = 0;
  let opsOk = 0;
  let fails = 0;
  const fail = (msg, e) => {
    fails++;
    const text = e && e.stack ? e.stack : String(e);
    process.stderr.write('FUZZ FAIL seed=' + SEED + ' op=' + msg + ': ' + text + '\n');
  };

  const drainPending = async (label) => {
    if (!destroyPending) return;
    await withTimeout(destroyPending, OP_TIMEOUT_MS, label || 'destroyPending');
    destroyPending = null;
  };

  const ensureDomain = async () => {
    if (destroyPending) {
      await drainPending('ensureDomain/drain');
      py = null;
      math = null;
      handles = [];
    }
    if (py && !py.closed) return;
    try {
      py = ccpy.create();
      math = py.import('math');
      handles = [];
    } catch (e) {
      if (/destroy is still in progress/.test(e.message)) {
        articulate++;
        await drainPending('ensureDomain/retry-drain');
        py = ccpy.create();
        math = py.import('math');
        handles = [];
        return;
      }
      throw e;
    }
  };

  const kickDestroy = () => {
    if (!py || py.closed) return;
    const p = py.destroy();
    destroyPending = Promise.resolve(p).catch(() => {});
    py = null;
    math = null;
    handles = [];
  };

  for (let i = 0; i < OPS; i++) {
    const op = OPS_LIST[pick(OPS_LIST.length)];
    lastOp = op;
    lastIdx = i;
    if (i === 0 || (i + 1) % PROGRESS_OPS === 0) {
      pulse('progress ' + (i + 1) + '/' + OPS + ' op=' + op);
      log('PROGRESS fuzz_seed=' + SEED + ' ' + (i + 1) + '/' + OPS + ' op=' + op);
    }
    try {
      switch (op) {
        case 'recreate':
        case 'sync_call':
        case 'task_call':
        case 'lease_fsum':
        case 'keep_past':
        case 'release':
        case 'stats':
          await ensureDomain();
          break;
        default:
          break;
      }

      if (op === 'sync_call') {
        const v = math.floor(3.7 + rng());
        if (v !== 3 && v !== 4) fail('sync_call bad ' + v);
        else opsOk++;
      } else if (op === 'task_call') {
        const v = await withTimeout(py.task(math.floor)(9.1), OP_TIMEOUT_MS, 'task_call');
        if (v !== 9) fail('task_call bad ' + v);
        else opsOk++;
      } else if (op === 'lease_fsum') {
        const n = 64 + pick(256);
        const buf = new Float64Array(n);
        buf.fill(1);
        const v = await withTimeout(py.task(math.fsum)(buf), OP_TIMEOUT_MS, 'lease_fsum');
        if (v !== n) fail('lease_fsum bad ' + v);
        else opsOk++;
      } else if (op === 'keep_past') {
        const b = py.import('builtins');
        const g = b.dict();
        b.exec('G=[]\ndef keep(mv):\n  G.append(mv)\n  return len(mv)\n', g);
        const keep = b.eval('keep', g);
        const buf = new Float64Array(8);
        buf.fill(1);
        let caught = false;
        try { keep(buf); }
        catch (e) {
          caught = /retained by the callee|borrow ends/.test(e.message);
          if (caught) articulate++;
          else throw e;
        }
        if (!caught) fail('keep_past not caught');
        else opsOk++;
        if (pick(2) === 0) {
          let lane = false;
          try {
            await withTimeout(py.task(keep)(buf), OP_TIMEOUT_MS, 'keep_past_lane');
          } catch (e) {
            lane = /retained by the callee|borrow ends/.test(e.message);
            if (lane) articulate++;
            else throw e;
          }
          if (!lane) fail('keep_past lane not caught');
        }
      } else if (op === 'release') {
        const h = math.sqrt;
        handles.push(h);
        py.release(h);
        try { h(4); fail('release still callable'); }
        catch (e) {
          if (/released|another bridge|closed/.test(e.message)) {
            articulate++;
            opsOk++;
          } else throw e;
        }
      } else if (op === 'destroy_await') {
        if (py && !py.closed) {
          await withTimeout(py.destroy(), OP_TIMEOUT_MS, 'destroy_await');
          py = null;
          math = null;
          handles = [];
        }
        await drainPending('destroy_await/pending');
        opsOk++;
      } else if (op === 'destroy_kick') {
        if (py && !py.closed) {
          kickDestroy();
          try {
            const probe = ccpy.create();
            await withTimeout(probe.destroy(), OP_TIMEOUT_MS, 'destroy_kick/probe');
            opsOk++;
          } catch (e) {
            if (/destroy is still in progress/.test(e.message)) {
              articulate++;
              opsOk++;
            } else throw e;
          }
        } else {
          opsOk++;
        }
      } else if (op === 'gc') {
        await gcNow();
        opsOk++;
      } else if (op === 'recreate') {
        if (py && !py.closed) {
          await withTimeout(py.destroy(), OP_TIMEOUT_MS, 'recreate/destroy');
        }
        py = null;
        await ensureDomain();
        opsOk++;
      } else if (op === 'stats') {
        const s = py.stats();
        if (typeof s !== 'number' || s < 0) fail('stats bad ' + s);
        else opsOk++;
      }
    } catch (e) {
      fail(op + '@' + i, e);
      try {
        await drainPending('fail/drain');
        if (py && !py.closed) {
          await withTimeout(py.destroy(), OP_TIMEOUT_MS, 'fail/destroy');
        }
      } catch (_) {}
      py = null;
      math = null;
      handles = [];
    }
  }

  lastOp = 'teardown';
  try {
    await drainPending('final/drain');
    if (py && !py.closed) {
      await withTimeout(py.destroy(), OP_TIMEOUT_MS, 'final/destroy');
    }
  } catch (_) {}

  disarmWatchdogs();
  result('ops_ok %d', opsOk);
  result('articulate %d', articulate);
  result('fails %d', fails);
  const clean = ok('fuzz_walk', fails === 0);
  if (!clean) {
    process.stderr.write(
      'FUZZ FAILED seed=' + SEED + ' — replay: FUZZ_SEED=' + SEED
      + ' FUZZ_OPS=' + OPS + ' node --expose-gc stress/bridge/js_python_fuzz.js\n');
    process.exit(1);
  }
  log('fuzz done');
  pulse('done ok');
})().catch((e) => {
  disarmWatchdogs();
  process.stderr.write('FUZZ ERROR seed=' + SEED + ': '
    + (e && e.stack ? e.stack : e) + '\n');
  process.stderr.write('replay: FUZZ_SEED=' + SEED + ' FUZZ_OPS=' + OPS
    + ' node --expose-gc stress/bridge/js_python_fuzz.js\n');
  process.exit(1);
});
