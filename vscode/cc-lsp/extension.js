const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const { spawn } = require("child_process");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;
let output;
let binWatcher;
let restartTimer;
let restarting = false;

function workspaceRoot() {
  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  return folder ? folder.uri.fsPath : "";
}

function findCcc() {
  const root = workspaceRoot();
  const configured = vscode.workspace.getConfiguration("concurrent-c").get("cccPath");
  if (configured && fs.existsSync(configured)) return configured;
  if (root) {
    const local = path.join(root, "cc", "bin", "ccc");
    if (fs.existsSync(local)) return local;
  }
  return "ccc";
}

function findServer() {
  const configured = vscode.workspace.getConfiguration("concurrent-c").get("serverPath");
  if (configured && fs.existsSync(configured)) return configured;
  const root = workspaceRoot();
  if (root) {
    const dev = path.join(root, "vscode", "cc-lsp", "bin", "cc-lsp");
    if (fs.existsSync(dev)) return dev;
  }
  const bundled = path.join(__dirname, "bin", "cc-lsp");
  if (fs.existsSync(bundled)) return bundled;
  return "cc-lsp";
}

function findInstallScript() {
  const root = workspaceRoot();
  if (!root) return "";
  const sh = path.join(root, "vscode", "cc-lsp", "install-local.sh");
  return fs.existsSync(sh) ? sh : "";
}

function serverArgs() {
  const folder = workspaceRoot();
  const ccc = findCcc();
  const args = ["--stdio"];
  if (folder) args.push("--root", folder);
  if (ccc !== "ccc") args.push("--ccc", ccc);
  return args;
}

function serverOptions() {
  const folder = workspaceRoot();
  return {
    command: findServer(),
    args: serverArgs(),
    transport: TransportKind.stdio,
    options: folder ? { cwd: folder } : undefined,
  };
}

async function startClient(context) {
  const server = findServer();
  output.appendLine(`starting ${server} ${serverArgs().join(" ")}`);

  /* A function here must return a ChildProcess / streams, not an
   * Executable. Passing `{ command, args }` as a function result makes
   * the client do `result.stderr.on(...)` and throw. */
  client = new LanguageClient(
    "concurrent-c-lsp",
    "Concurrent-C Language Server",
    serverOptions(),
    {
      documentSelector: [
        { scheme: "file", language: "concurrent-c" },
        { scheme: "untitled", language: "concurrent-c" },
        { scheme: "file", pattern: "**/*.{ccs,cch,shcc}" },
      ],
      outputChannel: output,
    },
  );

  context.subscriptions.push(client);
  try {
    await client.start();
    output.appendLine("language client started");
  } catch (err) {
    output.appendLine(`language client failed: ${err}`);
    void vscode.window.showErrorMessage(`cc-lsp failed to start: ${err}`);
  }
}

async function restartClient(reason) {
  if (!client || restarting) return;
  restarting = true;
  try {
    output.appendLine(`restarting language server (${reason})`);
    if (typeof client.restart === "function") {
      await client.restart();
    } else {
      await client.stop();
      await client.start();
    }
    output.appendLine("language client restarted");
  } catch (err) {
    output.appendLine(`restart failed: ${err}`);
    void vscode.window.showErrorMessage(`cc-lsp restart failed: ${err}`);
  } finally {
    restarting = false;
  }
}

function watchServerBinary() {
  if (binWatcher) {
    binWatcher.close();
    binWatcher = undefined;
  }
  const server = findServer();
  if (!server || server === "cc-lsp" || !fs.existsSync(path.dirname(server))) return;
  try {
    binWatcher = fs.watch(path.dirname(server), (_ev, fn) => {
      if (fn && fn !== path.basename(server)) return;
      if (!vscode.workspace.getConfiguration("concurrent-c").get("restartOnBinaryChange")) {
        return;
      }
      clearTimeout(restartTimer);
      restartTimer = setTimeout(() => {
        void restartClient("binary changed");
      }, 400);
    });
  } catch (err) {
    output.appendLine(`binary watch failed: ${err}`);
  }
}

function runBuildOnly() {
  const sh = findInstallScript();
  if (!sh) {
    return Promise.reject(new Error("vscode/cc-lsp/install-local.sh not in this workspace"));
  }
  output.appendLine(`building ${sh} --build-only`);
  return new Promise((resolve, reject) => {
    const child = spawn(sh, ["--build-only"], {
      cwd: workspaceRoot() || path.dirname(sh),
    });
    child.stdout.on("data", (d) => output.append(d.toString()));
    child.stderr.on("data", (d) => output.append(d.toString()));
    child.on("error", reject);
    child.on("close", (code) => {
      if (code === 0) resolve();
      else reject(new Error(`build exited ${code}`));
    });
  });
}

async function activate(context) {
  output = vscode.window.createOutputChannel("Concurrent-C Language Server");
  context.subscriptions.push(output);
  context.subscriptions.push(
    new vscode.Disposable(() => {
      clearTimeout(restartTimer);
      if (binWatcher) binWatcher.close();
    }),
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("concurrent-c.restartServer", () =>
      restartClient("command"),
    ),
    vscode.commands.registerCommand("concurrent-c.rebuildServer", async () => {
      try {
        await vscode.window.withProgress(
          {
            location: vscode.ProgressLocation.Notification,
            title: "Building cc-lsp",
          },
          () => runBuildOnly(),
        );
        /* fs.watch restarts; do it here too if watch is off. */
        if (!vscode.workspace.getConfiguration("concurrent-c").get("restartOnBinaryChange")) {
          await restartClient("rebuild");
        }
      } catch (err) {
        output.appendLine(`rebuild failed: ${err}`);
        void vscode.window.showErrorMessage(`cc-lsp rebuild failed: ${err}`);
      }
    }),
  );

  await startClient(context);
  watchServerBinary();
}

async function deactivate() {
  clearTimeout(restartTimer);
  if (binWatcher) binWatcher.close();
  if (client) await client.stop();
}

module.exports = { activate, deactivate };
