# cccportable — consumer host-C snapshot

A portable tree so teammates and CI compile emitted C with host `cc` and never
invoke `ccc`. It is not a compiler sysroot: no `.cch`, no `shadow_lower`.
`--sysroot` remains host-cc cross-compile.

## Author (has `ccc`)

Copy the snapshot from the **resolved** toolchain (prefix or checkout), not
from cwd `out/`:

```bash
ccc portable-install vendor/cccportable
```

`DIR` must be missing, empty, or already stamped (`CCCPORTABLE.txt`). Occupied
unstamped directories are refused.

Generate C with `--no-line` when writing into the foreign tree (`gen.sh`):

```bash
ccc --emit-c-only --no-line myfile.ccs -o include/generated/myfile.c
ccc --cccportable vendor/cccportable --print-cflags
ccc --cccportable vendor/cccportable --print-libs
```

`--print-*` is author-only. Paste the flags into the consumer Makefile once.
Do not write `$(ccc --print-cflags)` there — teammates do not have `ccc`.

`--cccportable` does not remap lowerer faces. Using it with emit/compile/link
is an error. `CCCPORTABLE` is read only by `--print-*`.

A stamp mismatch on install or `--print-*` is the only hard version check.
Refresh the install and re-emit by hand. Emitted `.c` may carry a
`ccc --version` comment; that comment does not fail the build.

`--emit-c-only` to a path outside `out/` with `#line` still on prints a
warning. Put `--no-line` in `gen.sh`. The warning does not enable `--no-line`.

## Consumer (no `ccc`)

Compile `concurrent_c.c` **once**, always with `-DCC_ENABLE_ASYNC` (same ABI
as `ccc` itself). The define is not an app-TU flag.

Multi-`.c`:

```bash
cc -Ivendor/cccportable/include -c include/generated/myfile.c
cc -Ivendor/cccportable/include -DCC_ENABLE_ASYNC \
   -c vendor/cccportable/runtime/concurrent_c.c
cc -o app myfile.o concurrent_c.o -lpthread -lm
```

One-shot (what `--print-libs` describes):

```bash
cc include/generated/myfile.c -Ivendor/cccportable/include \
   -DCC_ENABLE_ASYNC vendor/cccportable/runtime/concurrent_c.c -lpthread -lm
```

## Independent knobs

| Knob | Role |
|------|------|
| `portable-install` / `--cccportable` | Vendor headers + runtime |
| `#pragma(@prelude) off` | No automatic prolog (any unit) |
| `#pragma(@linenumbers) off` / `--no-line` | No `#line` in emit |
| `--no-runtime` | Do not link the runtime |

A comptime-only script needs no portable tree:

```
#pragma(@prelude) off
```

```bash
ccc --emit-c-only --no-runtime --no-line bare.shcc -o bare.c
cc -o bare bare.c
```
