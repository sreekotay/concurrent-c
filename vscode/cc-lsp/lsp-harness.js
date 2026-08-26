"use strict";

const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const here = __dirname;
const repo = path.resolve(here, "../..");
const defaultServer = process.env.CC_LSP || path.join(here, "bin", "cc-lsp");
const defaultCcc = process.env.CC_LSP_CCC || path.join(repo, "cc", "bin", "ccc");

function encode(obj) {
  const body = Buffer.from(JSON.stringify(obj), "utf8");
  return Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]);
}

class LspReader {
  constructor(stream) {
    this.buf = Buffer.alloc(0);
    this.q = [];
    this.wait = [];
    stream.on("data", (c) => this._push(c));
  }

  _push(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
    for (;;) {
      const sep = this.buf.indexOf("\r\n\r\n");
      if (sep < 0) return;
      const header = this.buf.slice(0, sep).toString("utf8");
      const m = /Content-Length:\s*(\d+)/i.exec(header);
      if (!m) throw new Error(`bad LSP header: ${JSON.stringify(header)}`);
      const len = Number(m[1]);
      const start = sep + 4;
      if (this.buf.length < start + len) return;
      const msg = JSON.parse(this.buf.slice(start, start + len).toString("utf8"));
      this.buf = this.buf.slice(start + len);
      if (this.wait.length) this.wait.shift()(msg);
      else this.q.push(msg);
    }
  }

  next(ms) {
    if (this.q.length) return Promise.resolve(this.q.shift());
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error("timeout waiting for LSP message")), ms);
      this.wait.push((msg) => {
        clearTimeout(t);
        resolve(msg);
      });
    });
  }
}

class LspClient {
  constructor(opts) {
    this.opts = opts || {};
    this.server = this.opts.server || defaultServer;
    this.ccc = this.opts.ccc || defaultCcc;
    this.quiet = !!this.opts.quiet;
    this.nextId = 1;
    this.child = null;
    this.reader = null;
    this.died = null;
    this.stderr = "";
  }

  start() {
    if (!fs.existsSync(this.server)) {
      throw new Error(`cc-lsp not found: ${this.server}\n  ./vscode/cc-lsp/install-local.sh --build-only`);
    }
    const args = ["--stdio", "--root", repo];
    if (fs.existsSync(this.ccc)) args.push("--ccc", this.ccc);
    if (!this.quiet) process.stderr.write(`spawn ${this.server} ${args.join(" ")}\n`);
    this.child = spawn(this.server, args, {
      cwd: repo,
      stdio: ["pipe", "pipe", "pipe"],
    });
    this.child.stderr.on("data", (d) => {
      this.stderr += d.toString();
      if (!this.quiet) process.stderr.write(d);
    });
    this.child.on("exit", (code, signal) => {
      this.died = { code, signal };
    });
    this.reader = new LspReader(this.child.stdout);
    return this;
  }

  send(obj) {
    if (!this.child || !this.child.stdin.writable) throw new Error("server stdin closed");
    this.child.stdin.write(encode(obj));
  }

  notify(method, params) {
    this.send({ jsonrpc: "2.0", method, params: params || {} });
  }

  async request(method, params, ms) {
    const id = this.nextId++;
    this.send({ jsonrpc: "2.0", id, method, params: params || {} });
    const deadline = Date.now() + (ms || 5000);
    const leftovers = [];
    try {
      while (Date.now() < deadline) {
        const msg = await this.reader.next(deadline - Date.now());
        if (msg.id === id) return msg;
        leftovers.push(msg);
      }
      throw new Error(`timeout waiting for id=${id} (${method})`);
    } finally {
      this.reader.q.unshift(...leftovers);
    }
  }

  async wait(pred, ms) {
    const deadline = Date.now() + (ms || 10000);
    while (Date.now() < deadline) {
      const msg = await this.reader.next(deadline - Date.now());
      if (pred(msg)) return msg;
    }
    throw new Error("timeout waiting for matching LSP message");
  }

  async waitDiag(uri, ms) {
    const msg = await this.wait(
      (m) => m.method === "textDocument/publishDiagnostics" && (!uri || m.params.uri === uri),
      ms || 15000,
    );
    return msg.params;
  }

  async initialize() {
    const msg = await this.request("initialize", {
      processId: process.pid,
      rootUri: "file://" + repo,
      capabilities: {},
    });
    if (!msg.result || !msg.result.capabilities) {
      throw new Error(`bad initialize: ${JSON.stringify(msg)}`);
    }
    this.notify("initialized", {});
    return msg.result;
  }

  didOpen(uri, text, version) {
    this.notify("textDocument/didOpen", {
      textDocument: {
        uri,
        languageId: "concurrent-c",
        version: version || 1,
        text,
      },
    });
  }

  didChange(uri, text, version) {
    this.notify("textDocument/didChange", {
      textDocument: { uri, version },
      contentChanges: [{ text }],
    });
  }

  didSave(uri, text) {
    const params = { textDocument: { uri } };
    if (text != null) params.text = text;
    this.notify("textDocument/didSave", params);
  }

  didClose(uri) {
    this.notify("textDocument/didClose", { textDocument: { uri } });
  }

  async shutdown() {
    const msg = await this.request("shutdown", null, 8000);
    this.notify("exit");
    await new Promise((resolve) => this.child.stdin.end(resolve));
    const status = await new Promise((resolve) => {
      if (this.died) return resolve(this.died);
      this.child.on("exit", (code, signal) => resolve({ code, signal }));
      setTimeout(() => {
        this.child.kill("SIGTERM");
        resolve({ code: null, signal: "timeout" });
      }, 2000);
    });
    if (status.signal) throw new Error(`server died ${status.signal}`);
    if (status.code !== 0 && status.code !== null) {
      throw new Error(`server exit ${status.code}`);
    }
    return msg;
  }

  kill() {
    if (this.child && !this.died) this.child.kill("SIGTERM");
  }
}

function fileUri(p) {
  return "file://" + path.resolve(p);
}

function read(p) {
  return fs.readFileSync(p, "utf8");
}

module.exports = {
  LspClient,
  LspReader,
  encode,
  fileUri,
  read,
  repo,
  here,
  defaultServer,
  defaultCcc,
};
