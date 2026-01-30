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

## Concurrency debugging (ThreadSanitizer)

Use the provided scripts for regular race detection:

- Quick TSan run: `./scripts/test_tsan.sh`
- Full TSan run: `./scripts/test_tsan.sh --all`
- Stress tests under TSan: `./scripts/stress_sanitize.sh tsan`

Notes:
- On macOS, TSan requires `clang`.
- TSan runs are slower; keep them focused on stress/race tests.

## Scheduler synchronization invariants (fiber scheduler)

Key invariants in `cc/runtime/fiber_sched.c` that should not be violated:

- `running_lock` protects `mco_resume()`/`cc__fiber_unpark()` from concurrent resume.
  Any unpark path must wait until `running_lock == 0`.
- `join_lock` + `join_waiter_fiber` provide a handshake so joiners never miss a wakeup.
  `done=1` is set under the join lock and the waiter is read atomically.
- `state=FIBER_DONE` is set before signaling waiters.
  Joiners must wait for `state==FIBER_DONE` before freeing or reusing fibers.
- `unpark_pending` handles the park/unpark race: an unpark before park sets the
  flag, and park re-checks it both before and after setting `state=PARKED`.
- Fiber state transitions are linear: `CREATED -> READY -> RUNNING -> PARKED -> READY -> RUNNING -> DONE`.

If you add new scheduler behaviors, update these invariants and extend the stress tests.

