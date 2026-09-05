# stress/break

Programs the compiler must handle and diagnostics it must give. Every
fragility named in [`docs/compiler_internals.md`](../../docs/compiler_internals.md)
gets a program here before the code that removes it lands.

Run: `./tools/cc_test --filter stress/break`. The directory is discovered by
`cc_test` like `tests/` and uses the same sidecars: `.stdout` needles for a
program that must compile and run, `.compile_err` needles for a `_fail`
program, `_smoke` for a build that must be warning-free.

`<stem>.xfail` marks a test whose sidecars describe the behaviour the
compiler should have and does not yet; the harness reports it as XFAIL and
does not count it. When the behaviour lands the test reports XPASS and
counts as a failure until the marker is deleted. The first line of the
`.xfail` file says what the compiler does today.

Stems are global across `tests/` and this directory; prefix them `break_`.
