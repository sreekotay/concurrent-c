#!/usr/bin/env node
"use strict";

const fs = require("fs");
const path = require("path");
const { LspClient, fileUri, read, here, repo } = require("./lsp-harness");

function lspJunk(dir) {
  return fs.readdirSync(dir).filter(
    (n) => n.startsWith(".cc-lsp-") || n.endsWith(".cc-lsp-out.c")
  );
}

const brokenPath = path.join(here, "testdata", "broken.ccs");
const okPath = path.join(here, "testdata", "ok.ccs");
const helloPath = path.join(repo, "examples", "hello.ccs");
const brokenText = read(brokenPath);
const okText = read(okPath);

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function withClient(fn) {
  const c = new LspClient({ quiet: process.env.CC_LSP_TEST_VERBOSE !== "1" });
  c.start();
  try {
    const r = await fn(c);
    if (!c.died) await c.shutdown();
    return r;
  } catch (e) {
    if (!c.died) c.kill();
    throw e;
  }
}

const tests = [];
function test(name, fn) {
  tests.push({ name, fn });
}

test("initialize caps", async () => {
  await withClient(async (c) => {
    const result = await c.initialize();
    const caps = result.capabilities || {};
    assert(caps.textDocumentSync === 1, `textDocumentSync=${caps.textDocumentSync}`);
    assert(caps.hoverProvider === false, `hoverProvider=${caps.hoverProvider}`);
  });
});

test("broken open publishes a diag", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(brokenPath);
    c.didOpen(uri, brokenText, 1);
    const d = await c.waitDiag(uri);
    assert(d.diagnostics.length >= 1, "expected a diagnostic");
    const msg = d.diagnostics.map((x) => x.message).join("\n");
    assert(/after 'm'|expected/.test(msg), `unexpected diag: ${msg}`);
  });
});

test("ok open publishes empty", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(okPath);
    c.didOpen(uri, okText, 1);
    const d = await c.waitDiag(uri);
    assert(d.diagnostics.length === 0, `expected clean, got ${JSON.stringify(d.diagnostics)}`);
  });
});

test("hello.ccs publishes empty", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(helloPath);
    c.didOpen(uri, read(helloPath), 1);
    const d = await c.waitDiag(uri, 20000);
    assert(d.diagnostics.length === 0, `hello dirty: ${JSON.stringify(d.diagnostics)}`);
    assert(
      !fs.existsSync(path.join(repo, "examples", "out")),
      "check must not create examples/out"
    );
  });
});

test("didChange broken then clean (debounce)", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(okPath);
    c.didOpen(uri, okText, 1);
    await c.waitDiag(uri);
    c.didChange(uri, brokenText, 2);
    const dirty = await c.waitDiag(uri);
    assert(dirty.diagnostics.length >= 1, "change to broken should diag");
    c.didChange(uri, okText, 3);
    const last = await c.waitDiag(uri);
    assert(last.diagnostics.length === 0, "change back to ok should clear");
  });
});

test("didClose publishes empty", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(brokenPath);
    c.didOpen(uri, brokenText, 1);
    await c.waitDiag(uri);
    c.didClose(uri);
    const d = await c.waitDiag(uri);
    assert(d.diagnostics.length === 0, "close should clear diags");
  });
});

test("cch with ifdef fields is clean", async () => {
  const hdr = path.join(here, "testdata", "ifdef_field.cch");
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(hdr);
    c.didOpen(uri, read(hdr), 1);
    const d = await c.waitDiag(uri, 20000);
    assert(
      d.diagnostics.length === 0,
      `header should be clean, got ${JSON.stringify(d.diagnostics)}`
    );
  });
});

test("quoted include + no project junk", async () => {
  const uses = path.join(here, "testdata", "uses_inc.ccs");
  const planted = path.join(here, "testdata", ".cc-lsp-planted.ccs");
  fs.writeFileSync(planted, "junk");
  try {
    await withClient(async (c) => {
      await c.initialize();
      const uri = fileUri(uses);
      c.didOpen(uri, read(uses), 1);
      const d = await c.waitDiag(uri, 20000);
      assert(
        d.diagnostics.length === 0,
        `quote include dirty: ${JSON.stringify(d.diagnostics)}`
      );
      assert(!fs.existsSync(planted), "planted .cc-lsp-* leftover should be swept");
      assert(
        lspJunk(path.dirname(uses)).length === 0,
        `testdata junk: ${lspJunk(path.dirname(uses)).join(",")}`
      );
      assert(
        !fs.existsSync(path.join(path.dirname(uses), "out")),
        "check must not create testdata/out"
      );
    });
  } finally {
    try {
      fs.unlinkSync(planted);
    } catch (_) {
      /* swept */
    }
  }
});

test("hover during in-flight check still replies", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(helloPath);
    c.didOpen(uri, read(helloPath), 1);
    const msg = await c.request("textDocument/hover", {
      textDocument: { uri },
      position: { line: 0, character: 4 },
    });
    assert(msg.result === null, `hover while checking=${JSON.stringify(msg.result)}`);
    assert(!c.died, `server died during in-flight hover ${JSON.stringify(c.died)}`);
    const d = await c.waitDiag(uri, 20000);
    assert(d.diagnostics.length === 0, `hello dirty: ${JSON.stringify(d.diagnostics)}`);
  });
});

test("hover returns null", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(okPath);
    c.didOpen(uri, okText, 1);
    await c.waitDiag(uri);
    const msg = await c.request("textDocument/hover", {
      textDocument: { uri },
      position: { line: 0, character: 4 },
    });
    assert(msg.result === null, `hover=${JSON.stringify(msg.result)}`);
  });
});

test("stress: 24 didChange, last gen wins", async () => {
  await withClient(async (c) => {
    await c.initialize();
    const uri = fileUri(okPath);
    c.didOpen(uri, okText, 1);
    await c.waitDiag(uri);
    let ver = 1;
    for (let i = 0; i < 24; i++) {
      ver++;
      c.didChange(uri, i % 2 === 0 ? brokenText : okText, ver);
    }
    /* last i=23 is odd → okText. Intermediate publishes may be dirty. */
    const deadline = Date.now() + 20000;
    let last = null;
    let clean = false;
    while (Date.now() < deadline && !clean) {
      last = await c.waitDiag(uri, deadline - Date.now());
      clean = last.diagnostics.length === 0;
    }
    assert(last, "no publish after stress burst");
    assert(clean, `last gen should be clean, got ${JSON.stringify(last && last.diagnostics)}`);
    assert(!c.died, `server died during stress ${JSON.stringify(c.died)}`);
  });
});

async function main() {
  const only = process.argv.slice(2).filter((a) => !a.startsWith("-"));
  const list = only.length ? tests.filter((t) => only.some((s) => t.name.includes(s))) : tests;
  let failed = 0;
  for (const t of list) {
    const start = Date.now();
    try {
      await t.fn();
      console.log(`ok   ${t.name}  (${Date.now() - start}ms)`);
    } catch (e) {
      failed++;
      console.log(`FAIL ${t.name}  (${Date.now() - start}ms)`);
      console.error(`     ${e.message || e}`);
    }
  }
  console.log(failed ? `${failed}/${list.length} failed` : `${list.length} ok`);
  process.exit(failed ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
