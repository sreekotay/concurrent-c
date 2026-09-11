# CC Compiler Source

The driver, `cc_main.c`. It resolves paths and modes, reads `build.cc`, runs
the comptime pipeline, lowers quoted `.cch` faces, calls the lowerer
(`out/cc/bin/cclower_cc`, sources in `cc/lower/`), then host-compiles and
links what comes back.

Subdirectories:

- `ast/`: CC-specific node metadata used by the remaining sugar helpers
- `build/`: `build.cc` parsing and the host compiler profile
- `visitor/`: text sugar used by header lowering and comptime
  (`pass_*_syntax`, unwrap/destroy, errhandler lookup)
- `header/`: quoted `.cch` to `.h` lowering
- `preprocess/`: include rewriting, the comptime seam, emit plan,
  variant and type registry
- `comptime/`: comptime evaluator (libtcc) and the monomorph instantiation
  cache, archived as `libshadow_comptime.a`
- `ir/`: the IR and its verifier
- `diag/`: diagnostics
- `tools/`: `lower_headers` and the `cpp_expand` probe
- `util/`: shared helpers

`cc_main.c` dispatches `.ccs` / `.shcc` units to the lowerer.
