### Test runner conventions (`tools/cc_test`)

The repo provides a minimal test runner: `./tools/cc_test`.

It discovers `tests/*.c` and `tests/*.cc` and runs each as either:
- **run test**: compile → link → run, expecting exit code 0
- **compile-fail test**: compile is expected to fail

#### Sidecar files (all optional)

For a test `tests/foo.cc`:

- `tests/foo.stdout`
  - Each non-empty, non-`#` line is a **required substring** in the program’s stdout.

- `tests/foo.stderr`
  - Each non-empty, non-`#` line is a **required substring** in the program’s stderr.

- `tests/foo.compile_err`
  - Marks the test as **compile-fail**.
  - Each non-empty, non-`#` line is a **required substring** in the host C compiler error output (used for sourcemap checks).

- `tests/foo.ldflags`
  - Extra host linker flags (example: `-lpthread`).

- `tests/foo.requires_async`
  - If present, the test is **skipped unless** `CC_ENABLE_ASYNC=1` is set in the environment.

#### Flags

- `./tools/cc_test --list`: list selected tests
- `./tools/cc_test --filter SUBSTR`: only run tests whose name/path contains `SUBSTR`
- `./tools/cc_test --verbose`: print commands


