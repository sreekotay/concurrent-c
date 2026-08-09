/* Runtime selection: which Python a process gets, chosen from code
 * (usePython: venv dir / interpreter / libpython), from the ambient
 * environment (VIRTUAL_ENV, ./.venv), or by discovery — most-specific
 * wins, every explicit or ambient form fails loudly on a broken target,
 * and the choice is process-wide (per-domain runtimes arrive with the
 * fork tier).  Selection rungs run in CHILD processes — one runtime per
 * process is the semantics under test.  Deterministic booleans; the
 * paired smoke pins them. */
'use strict';

const out = (name, cond) => console.log(name, cond ? 'true' : 'false');
const { spawnSync, execSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const REPO = process.cwd();
const ccpy = require(REPO + '/npm/cc-python');

function child(script, opts) {
  const o = opts || {};
  const env = Object.assign({}, process.env, o.env || {});
  delete env.VIRTUAL_ENV; // rungs opt in explicitly
  if (o.env && o.env.VIRTUAL_ENV) env.VIRTUAL_ENV = o.env.VIRTUAL_ENV;
  const r = spawnSync(process.execPath, ['-e', script], {
    cwd: o.cwd || REPO,
    env,
    encoding: 'utf8',
  });
  return { status: r.status, stdout: r.stdout.trim(), stderr: r.stderr };
}

const pyexe = execSync('command -v python3', { encoding: 'utf8' }).trim();
const venv = fs.mkdtempSync(path.join(os.tmpdir(), 'ccpy-env-')) + '/venv';
execSync(`python3 -m venv --without-pip '${venv}'`);
const req = `const ccpy = require(${JSON.stringify(REPO + '/npm/cc-python')});`;

// 1. Introspection before any load: articulate emptiness, not a guess.
{
  const p = ccpy.python();
  out('unloaded_reports_unloaded', p.loaded === false && p.version === '');
}

// 2. usePython(interpreter executable): spawn-resolve finds ITS runtime.
{
  const r = child(`${req}
    ccpy.usePython(${JSON.stringify(pyexe)});
    const py = ccpy.create();
    const info = ccpy.python();
    console.log(py.import('math').sqrt(16) === 4 && info.loaded &&
                /^3\\./.test(info.version) && info.how === 'cc_py_use'
                ? 'ok' : 'bad ' + JSON.stringify(info));
    py.destroy();`);
  out('use_python_executable', r.stdout === 'ok');
}

// 3. usePython(venv dir): CPython adopts the venv — sys.prefix IS the
//    venv and base_prefix stays the base runtime.
{
  const r = child(`${req}
    ccpy.usePython(${JSON.stringify(venv)});
    const py = ccpy.create();
    const sys = py.import('sys');
    console.log(String(sys.prefix) === ${JSON.stringify(venv)} &&
                String(sys.base_prefix) !== String(sys.prefix)
                ? 'ok' : 'bad ' + String(sys.prefix));
    py.destroy();`);
  out('use_python_venv_adopts', r.stdout === 'ok');
}

// 4. usePython(libpython path): the direct form.
{
  const r = child(`${req}
    ccpy.usePython(${JSON.stringify(pyexe)});
    const lib = ccpy.python().lib; // resolve without loading? create first
    const py = ccpy.create();
    console.log(ccpy.python().lib);
    py.destroy();`);
  const lib = r.stdout.split('\n').pop();
  const r2 = child(`${req}
    ccpy.usePython(${JSON.stringify(lib)});
    const py = ccpy.create();
    console.log(py.import('math').floor(7.9) === 7 ? 'ok' : 'bad');
    py.destroy();`);
  out('use_python_libpath', /libpython/.test(lib) && r2.stdout === 'ok');
}

// 5. One runtime per process: after a load, a matching choice is a
//    no-op and a different one answers articulately.
{
  const r = child(`${req}
    ccpy.usePython(${JSON.stringify(pyexe)});
    const py = ccpy.create();
    ccpy.usePython(${JSON.stringify(pyexe)}); // matching: silent
    let msg = '';
    try { ccpy.usePython(${JSON.stringify(venv)}); }
    catch (e) { msg = e.message; }
    console.log(/already loaded/.test(msg) ? 'ok' : 'bad ' + msg);
    py.destroy();`);
  out('second_runtime_refused', r.stdout === 'ok');
}

// 6. Ambient VIRTUAL_ENV: run node "inside" the venv, zero configuration.
{
  const r = child(`${req}
    const py = ccpy.create();
    console.log(String(py.import('sys').prefix) === ${JSON.stringify(venv)} &&
                ccpy.python().how === 'VIRTUAL_ENV' ? 'ok' : 'bad');
    py.destroy();`, { env: { VIRTUAL_ENV: venv } });
  out('ambient_virtual_env', r.stdout === 'ok');
}

// 7. Ambient ./.venv: the uv/poetry convention — a project-local venv is
//    picked up from the working directory.
{
  const proj = fs.mkdtempSync(path.join(os.tmpdir(), 'ccpy-proj-'));
  execSync(`python3 -m venv --without-pip '${proj}/.venv'`);
  const r = child(`${req}
    const py = ccpy.create();
    const p = String(py.import('sys').prefix);
    console.log(p.endsWith('/.venv') && ccpy.python().how === './.venv'
                ? 'ok' : 'bad ' + p);
    py.destroy();`, { cwd: proj });
  out('ambient_dot_venv', r.stdout === 'ok');
}

// 8. A present-but-broken selection is an ERROR, never a fall-through to
//    the wrong Python.
{
  const bogus = fs.mkdtempSync(path.join(os.tmpdir(), 'ccpy-bogus-'));
  const r = child(`${req}
    let msg = '';
    try { ccpy.usePython(${JSON.stringify(bogus)}); }
    catch (e) { msg = e.message; }
    console.log(/not a venv/.test(msg) ? 'ok' : 'bad ' + msg);`);
  out('broken_target_is_loud', r.stdout === 'ok');
  const r2 = child(`${req}
    let msg = '';
    try { ccpy.create(); } catch (e) { msg = e.message; }
    console.log(/VIRTUAL_ENV|not a venv|no pyvenv/.test(msg)
                ? 'ok' : 'bad ' + msg);`, { env: { VIRTUAL_ENV: bogus } });
  out('broken_ambient_is_loud', r2.stdout === 'ok');
}

console.log('env suite done');
