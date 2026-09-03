# Concurrent-C Language Server

Diagnostics LSP **written in Concurrent-C**. Also ships the `.ccs` TextMate grammar so highlighting does not depend on a second extension staying enabled.

```text
editor  --stdio JSON-RPC-->  cc-lsp (this package)
                                  |
                                  v
                           ccc build --emit-c-only
```

Pair with [`../ccs-syntax/`](../ccs-syntax/) for highlighting. See [`../roadmap.md`](../roadmap.md).

## Commands

```bash
./bin/cc-lsp --stdio              # language server (default)
./bin/cc-lsp --check path.ccs     # print CLI-shaped diagnostics
./bin/cc-lsp --smoke              # parser self-check
```

`ccc` is resolved from `--ccc`, `CC_LSP_CCC`, `CCC`, `<workspace>/cc/bin/ccc`, then PATH.

Diagnostics: `didOpen` / `didSave` kick immediately; `didChange` debounces
(~150 ms, one waiter per file). `.ccs` and `.cch` both run
`ccc build --emit-c-only` (a header is a unit; `build` without `run`
so `.shcc` is not auto-executed). The session fiber does not
wait on `ccc`. Hover is off (the handler returns `null`).

Unsaved buffers are written under `$TMPDIR/cc-lsp-<pid>/` (never next
to the source, never `cwd` into the project). `ccc --out-dir` stays
inside that wave so `out/.cc-build` does not appear beside the file.
A `#line 1 "real/path"` stamp keeps quoted includes (`"browse.cch"`)
resolving. Opening a file also deletes leftover `.cc-lsp-*` /
`*.cc-lsp-out.c` from older servers.

## Iterate (no window reload)

After the extension is installed once, the inner loop is rebuild the
binary. The client prefers `<workspace>/vscode/cc-lsp/bin/cc-lsp` and
restarts itself when that file changes.

```bash
./vscode/cc-lsp/install-local.sh --build-only
```

Or Command Palette: **Concurrent-C: Rebuild Language Server** /
**Concurrent-C: Restart Language Server**. Task: `cc-lsp: build`.

Reload the window only when `extension.js` / `package.json` change.

## Tests (no editor)

```bash
./vscode/cc-lsp/test.sh                 # --smoke + starter/stress suite
node vscode/cc-lsp/test-suite.js hover  # run tests whose names contain "hover"
CC_LSP_TEST_VERBOSE=1 ./vscode/cc-lsp/test.sh
node vscode/cc-lsp/test-stdio.js --file examples/hello.ccs --expect-clean
```

`--smoke` / `--check` never speak JSON-RPC. The suite drives initialize,
diagnostics, debounce, close, hover, and a 24× `didChange` burst.

Task: `cc-lsp: test`. Not part of `./scripts/test.sh` (compiler gate).

## Build + install from clone

```bash
cd vscode/cc-lsp
./install-local.sh --both
# Developer → Reload Window   # once
```

`install-local.sh` builds `cc_lsp.ccs` with `ccc` and copies the extension (plus this tiny JS launcher) into VS Code / Cursor.

The JS file only starts the CC binary. It does not parse diagnostics.

## License

MIT
