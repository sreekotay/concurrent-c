# Backwards compatibility

A Concurrent-C unit can name **what it is** and **which lowerer it needs** on
line 1. Newer toolchains keep older bootstrap seeds and use them when a pin
does not match the running version. Unmarked files still compile: a `.ccs` /
`.cch` / `.shcc` suffix is the fallback when the header is absent.

Normative rules: [spec §1.7](../spec/concurrent-c-spec-complete.md). Shipping a
new seed: [build-when.md](build-when.md). Seed layout:
[bootstrap README](../cc/bootstrap/shadow_lower/README.md).

## Strategy

The lowerer (`shadow_lower`) is a frozen C snapshot, not “whatever is in
`cc/shadow` today.” Each promote writes a folder named the full pin
(`MAJOR.MINOR.PATCH-SEED`, for example `0.3.2-124`). `last-good` points at the
running pin. `ccc --version` prints that pin (`ccc 0.3.2-124`).

A source file that pins `version=0.3.2` keeps lowering with a 0.3.2 seed after
the toolchain moves to 0.4.x. A file with no pin always uses the running
lowerer. A pin whose seed is not in the tree is an error — the driver does not
quietly substitute a different lowerer.

## Unit header

Kind is not an extension property. Line 1 names it; emit replaces that line
with the generated C/H banner.

| Kind | Line 1 |
|------|--------|
| source (`ccs`) | `#!ccc ccs [version=…]` |
| header (`cch`) | `#!ccc cch [version=…]` |
| script (`shcc`) | `#!/usr/bin/env -S ./cc/bin/ccc [--as=shcc] [version=…]` |

Scripts must be kernel-executable. `--as=shcc` on the OS shebang is optional: a
`ccc` interpreter shebang without `--as` is still script kind. `#!ccc shcc` is
ill-formed.

`--as=ccs|cch|shcc` and `version=` / `--ccc-version=` on the command line must
agree with the file header when both are present. A header that disagrees with
a `.ccs` / `.cch` / `.shcc` suffix is ill-formed.

## Version pins

Form: `MAJOR[.MINOR[.PATCH[-SEED]]]`. The pin is a **component prefix** of a
bootstrap folder name, not a digit prefix of the seed.

| Pin | Matches `0.3.2-122`? |
|-----|----------------------|
| `0.3.2-122` | yes (exact) |
| `0.3.2` | yes |
| `0.3` | yes |
| `0` | yes |
| `0.3.2-12` | no (`12` is not seed `122`) |
| `0.4` | no |

When the running toolchain matches the pin, that lowerer is used. Otherwise
`ccc` host-cc’s the **newest** matching seed under
`cc/bootstrap/shadow_lower/`. No matching folder is a hard error.

```text
#!ccc ccs version=0.3.2
```

```bash
ccc version=0.3.2-110 --emit-c-only path.ccs
ccc --ccc-version=0.3.2 run path.ccs
```

Unpinned units, and pins that prefix the running version, use the current
`shadow_lower`. Historical pins pay a one-time host-cc of that seed (cached
under the build cache).

## What you can leave unmarked

Existing `.ccs` / `.cch` / `.shcc` trees without a header keep working. The
suffix selects kind; the running lowerer runs. Add a header when you want the
kind to travel with the file (extensionless paths, or a pin).

## What a pin is for

Pin a file (or a CLI invocation) when its syntax or lowering must stay on a
known seed: a recipe written against 0.3.2, a CI job that must not pick up a
newer lowerer, a historical repro. Do not pin day-to-day hacking on this repo
unless you mean to freeze that unit.

Keep `last-good` plus one or two prior pins for rollback. Older seeds are safe
to delete once cold smoke is green — they are not required to build the
current toolchain, only to honor pins that name them.
