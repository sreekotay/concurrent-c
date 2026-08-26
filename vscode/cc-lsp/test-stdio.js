#!/usr/bin/env node
/*
 * One-shot stdio check (suite is test-suite.js).
 *
 *   node test-stdio.js
 *   node test-stdio.js --file examples/hello.ccs --expect-clean
 */
"use strict";

const path = require("path");
const { LspClient, fileUri, read, here } = require("./lsp-harness");

function parseArgs(argv) {
  let file = path.join(here, "testdata", "broken.ccs");
  let expectDiag = true;
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === "--file" && argv[i + 1]) {
      file = path.resolve(argv[++i]);
      expectDiag = false;
    } else if (argv[i] === "--expect-clean") {
      expectDiag = false;
    } else if (argv[i] === "--expect-diag") {
      expectDiag = true;
    } else if (argv[i] === "-h" || argv[i] === "--help") {
      console.log("usage: node test-stdio.js [--file PATH] [--expect-diag|--expect-clean]");
      process.exit(0);
    }
  }
  return { file, expectDiag };
}

async function main() {
  const { file, expectDiag } = parseArgs(process.argv);
  const c = new LspClient();
  c.start();
  await c.initialize();
  const uri = fileUri(file);
  c.didOpen(uri, read(file), 1);
  const d = await c.waitDiag(uri);
  console.error(`published ${d.diagnostics.length} diag(s) for ${d.uri}`);
  for (const x of d.diagnostics) {
    const r = x.range && x.range.start;
    console.error(`  ${r ? `${r.line + 1}:${r.character + 1}` : "?"} ${x.message}`);
  }
  if (expectDiag && d.diagnostics.length === 0) throw new Error("expected at least one diagnostic");
  if (!expectDiag && d.diagnostics.length > 0) {
    throw new Error(`expected clean publish, got ${d.diagnostics.length}`);
  }
  await c.shutdown();
  console.log("cc-lsp stdio: ok");
}

main().catch((err) => {
  console.error(err.message || err);
  process.exit(1);
});
