# cccportable — consumer host-C snapshot

A portable tree so teammates and CI compile emitted C with host `cc` and never
invoke `ccc`. It is not a compiler sysroot: no `.cch`, no `shadow_lower`.
`--sysroot` remains host-cc cross-compile.

The snapshot pins the headers and runtime it vendors; it does not select a
source lowerer. Use a unit or CLI version pin for that:
[backwards compatibility](backwards_compatibility.md).

Authors need `ccc` first ([install](../README.md#install) /
[getting started](getting-started.md#install)). Consumers do not.

| Topic | Where |
|-------|--------|
| `ccc portable-install` / `--cccportable` / `CCCPORTABLE` | this page; [build spec](../spec/concurrent-c-build.md) |
| `#pragma(@prelude) off` / `#pragma(@linenumbers) off` | [spec §1.8](../spec/concurrent-c-spec-complete.md#18-file-start-pragmas) |
| `CC_ENABLE_ASYNC` / `CC_PARSER_MODE` | [Defines](#defines) |
| Cheatsheet one-liners | [cheatsheet](cheatsheet.md) |

## Author (has `ccc`)

Copy the snapshot from the **resolved** toolchain (prefix or checkout), not
from cwd `out/`:

```bash
ccc portable-install vendor/cccportable
```

`DIR` must be missing, empty, or already stamped (`CCCPORTABLE.txt`). Occupied
unstamped directories are refused. The snapshot includes face-tree host-C
`*.h` files that lowering copies into `out/include` (notably
`include/ccc/vendor/ffc.h`). Consumers still use one `-I`.

Generate C with `--no-line` when writing into the foreign tree (`gen.sh`):

```bash
ccc --emit-c-only --no-line myfile.ccs -o include/generated/myfile.c
ccc --cccportable vendor/cccportable --print-cflags
ccc --cccportable vendor/cccportable --print-libs
```

`--print-*` is author-only. Paste the flags into the consumer Makefile once.
Do not write `$(ccc --print-cflags)` there — teammates do not have `ccc`.

`--cccportable` does not remap lowerer faces. Using it with emit/compile/link
is an error. `CCCPORTABLE` is read only by `--print-*`:

```bash
CCCPORTABLE=vendor/cccportable ccc --print-cflags
CCCPORTABLE=vendor/cccportable ccc --print-libs
```

An explicit `--cccportable DIR` wins over the environment.

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

## Defines

Anyone who host-cc's `--emit-c-only` output (portable snapshot or checkout
`out/include`) sets these themselves. `ccc` does not put them on the
emitted `.c`.

| Define | Host-cc of emitted C |
|--------|----------------------|
| `CC_ENABLE_ASYNC` | Always, on the `concurrent_c.c` TU only. Same ABI as `ccc`. Not an app-TU flag. |
| `CC_PARSER_MODE` | Do not set. `ccc` defines it only for parse / reparse / comptime so headers take stub branches TinyCC can swallow. Those `#ifdef`s remain in the lowered `.h` files. Defining it here takes the stubs (`cc_move` becomes a marker, containers stay gated). That looks like a successful compile. |

`--print-libs` already includes `-DCC_ENABLE_ASYNC`. It never prints
`-DCC_PARSER_MODE`.

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

File-start means after the unit header (`#!ccc ccs` / shebang). Other
operands are ill-formed. `--no-line` on the command line overrides the
linenumbers pragma. Details:
[spec §1.8](../spec/concurrent-c-spec-complete.md#18-file-start-pragmas).
