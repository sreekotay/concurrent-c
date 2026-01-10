# Debugging Concurrent-C programs (VS Code / Cursor)

This repo’s driver (`ccc`) already defines stable output locations (see `spec/concurrent-c-build.md`):

- Generated C: `out/<stem>.c`
- Linked binary: `bin/<stem>`

## Quickstart: debug the currently open `.ccs`

This repo includes:

- `.vscode/tasks.json` (build/run via `./cc/bin/ccc`)
- `.vscode/launch.json` (LLDB launch config)

In VS Code or Cursor:

1. Open a `.ccs` file (e.g. `examples/hello.ccs`)
2. Run **Run and Debug → Debug Concurrent-C (current file)**

It will:

- Run `./cc/bin/ccc build --no-cache --keep-c --cc-flags "-g -O0 -fno-omit-frame-pointer" <file>`
- Launch `${workspaceFolder}/bin/${fileBasenameNoExtension}` under LLDB

## What the toolchain needs for a good debugging experience

### 1) Debug symbols

`ccc` should pass through user-provided C compiler flags:

- Use `--cc-flags "-g -O0 -fno-omit-frame-pointer"` for predictable stepping/backtraces.

### 2) Stable, inspectable outputs

The build spec’s defaults are ideal for debugging:

- Always emit generated C under `out/`
- Put executables under `bin/`
- Keep generated outputs on failure (so you can open `out/<stem>.c` and compile commands)

### 3) Source mapping back to `.ccs`

To step through `.ccs` instead of the generated `.c`, the generated C should preserve original locations.

Minimum viable approach:

- Emit `#line` directives in the generated C mapping back to the original `.ccs` filename + line numbers.

With `#line` mapping, debuggers and diagnostics will typically attribute locations to the `.ccs` file even though the compiler is compiling `out/<stem>.c`.

