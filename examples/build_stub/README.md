# Build stub fixture

Use this to verify `ccc build` wiring with the (current) build.cc const loader and CLI `-D` overrides.

Commands:
```
./cc/bin/ccc build --emit-c-only examples/build_stub/main.ccs -o /tmp/out.c --dump-consts
# Expect (from examples/build_stub/build.cc):
# CONST DEBUG=1
# CONST USE_TLS=1
# CONST NUM_WORKERS=4

./cc/bin/ccc build --emit-c-only examples/build_stub/main.ccs -o /tmp/out.c --dump-consts -DUSE_TLS=0 -DNUM_WORKERS=8
# Expect CLI overrides applied after build.cc:
# CONST DEBUG=1
# CONST USE_TLS=0
# CONST NUM_WORKERS=8

./cc/bin/ccc build --build-file examples/build_stub/build.cc --dump-comptime --dry-run
# Expect:
# COMPTIME build_file=...
# COMPTIME consts (...)
# COMPTIME targets (...)

./cc/bin/ccc build graph --build-file examples/build_stub/build.cc --format json
./cc/bin/ccc build graph --build-file examples/build_stub/build.cc --format dot
./cc/bin/ccc build graph --build-file examples/build_stub/build.cc --format json --graph-out /tmp/ccc_graph.json
./cc/bin/ccc build graph --build-file examples/build_stub/build.cc --format dot --graph-out /tmp/ccc_graph.dot
```

Notes:
- build.cc is discovered alongside the input here; if you also place one in CWD, it will error to avoid ambiguity.
- Output file `/tmp/out.c` is the emitted C output (we pass `--emit-c-only`).
- A quick automated check is available: `sh examples/build_stub/test-build-stub.sh` (requires `cc/bin/ccc` built already).

