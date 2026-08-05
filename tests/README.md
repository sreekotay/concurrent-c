### Test runner conventions (`tools/cc_test`)

The repo provides a minimal test runner: `./tools/cc_test`.

It discovers `tests/**/*.{c,ccs,shcc}` recursively and runs each as either:
- **run test**: compile → link → run
- **compile-fail test**: compile is expected to fail

#### Sidecar files (all optional)

For a test `tests/foo.ccs` (or `.c` / `.shcc`):

- `tests/foo.stdout`
  - Each non-empty, non-`#` line is a **required substring** in the program’s stdout.
  - Multi-run: sections split on a line that is exactly `---` (see `.args`).

- `tests/foo.stderr`
  - Each non-empty, non-`#` line is a **required substring** in the program’s stderr.
  - Same `---` sectioning as `.stdout`.

- `tests/foo.compile_err`
  - Marks the test as **compile-fail**.
  - Each non-empty, non-`#` line is a **required substring** in the host C compiler error output (used for sourcemap checks).

- `tests/foo.build_stderr`
  - For **successful** builds: each non-empty, non-`#` line is a **required substring**
    in the `ccc build` stderr (compile-time warnings / notes).

- `tests/foo.args`
  - One argv line per run (`#` comments skipped; blank line = no args).
  - Builds once, then runs the binary once per line.

- `tests/foo.exit`
  - Expected process exit code(s). One integer for all runs, or one per `.args` line.
  - Missing → expect `0`.

- `tests/foo.stdin`
  - If present, redirected into the run (`< foo.stdin`).

- `tests/foo.ldflags`
  - Extra host linker flags (example: `-lpthread`).

- `tests/foo.requires_async`
  - If present, the test is **skipped unless** `CC_ENABLE_ASYNC=1` is set in the environment.

`.shcc` scripts use the same sidecars. Symlinks under `tests/` may point at
real tools (e.g. `script_minify_smoke.shcc` → `examples/.../minify.shcc`).

#### Flags

- `./tools/cc_test --list`: list selected tests
- `./tools/cc_test --filter SUBSTR`: only run tests whose name/path contains `SUBSTR`
- `./tools/cc_test --quick`: skip stress / lostwake / `*_race*` tests (**default**)
- `./tools/cc_test --full`: include those heavy tests (also `CC_TEST_FULL=1`)
- `./tools/cc_test --verbose`: print commands
- `./tools/cc_test --jobs N`: parallel runs (default: online CPU count, cap 16; `CC_TEST_JOBS`)
- `./tools/cc_test --no-cache` / `--use-cache`: control `ccc build` cache

`./scripts/test.sh` defaults to the fast loop (cheap preambles + `--quick`)
with the **native** front (`shadow_lower`). Use `--legacy` /
`CC_TEST_FRONTEND=legacy` for the older multipass path. Use
`CC_TEST_FULL=1` / `--full` for redis line-map / functional / tcc-patch /
variant-shape / stress.
