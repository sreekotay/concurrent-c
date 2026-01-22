# TCC Patches for Concurrent C

This directory contains patches to TCC (Tiny C Compiler) that enable CC language extensions.

## Patch Overview

**0001-cc-ext-hooks.patch** (~3700 lines)

A unified patch containing all CC extensions to TCC:

1. **Core Infrastructure** (`tcc.h`, `libtcc.c`)
   - AST stub recording system for CC visitor passes
   - `ext_parser` hooks for external statement/expression parsing
   - UFCS scratch state (`cc_last_member_*`, `cc_last_recv_type`)

2. **Statement Extensions** (`tccgen.c`)
   - `@arena` and `@arena_init` block parsing
   - `@nursery` block parsing
   - `@defer` statement parsing
   - `spawn` statement parsing

3. **Expression Extensions** (`tccgen.c`, `tccpp.c`)
   - `await` unary operator
   - UFCS tolerance (member-call syntax on non-struct receivers)
   - Receiver type extraction for type-qualified UFCS
   - `=>` arrow token for closures

4. **Closure Literals** (`tccgen.c`)
   - `() => expr` and `() => { block }` syntax
   - `[captures](...) => ...` with capture lists
   - `@unsafe [...](...) => ...` for unsafe closures

## Applying Patches

The patches are automatically applied during the build:

```bash
make tcc-patch-apply   # Apply patches to third_party/tcc
make tcc               # Build TCC with CC extensions
```

## Regenerating Patches

After modifying TCC sources:

```bash
make tcc-patch-regen   # Regenerate 0001-cc-ext-hooks.patch
```

## Upstream Compatibility

- Base: TCC `origin/mob` branch
- The patch is designed to be minimal and isolated behind `CONFIG_CC_EXT`
- Goal: keep changes upstreamable or at least easy to rebase

## Files Modified

| File | Changes |
|------|---------|
| `tcc.h` | AST node types, TCCState extensions, new tokens |
| `tcc.c` | CC output type flag |
| `libtcc.c` | `cc_tcc_parse_to_ast()` API, ext_parser setup |
| `tccgen.c` | Statement/expression parsing extensions |
| `tccpp.c` | `=>` arrow token lexing |