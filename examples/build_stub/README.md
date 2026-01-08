# Build stub fixture

Use this to verify `cc build` wiring with the stub build.cc loader and CLI `-D` overrides.

Commands:
```
cc build examples/build_stub/main.cc /tmp/out.c --dump-consts
# Expect (stub defaults):
# CONST DEBUG=1
# CONST USE_TLS=1
# CONST NUM_WORKERS=4

cc build examples/build_stub/main.cc /tmp/out.c --dump-consts -DUSE_TLS=0 -DNUM_WORKERS=8
# Expect CLI overrides applied after build.cc:
# CONST DEBUG=1
# CONST USE_TLS=0
# CONST NUM_WORKERS=8
```

Notes:
- build.cc is discovered alongside the input here; if you also place one in CWD, it will error to avoid ambiguity.
- Output file `/tmp/out.c` is produced by the stub compiler (currently just copies input).
- A quick automated check is available: `sh examples/build_stub/test-build-stub.sh` (requires `cc/bin/cc` built already).

