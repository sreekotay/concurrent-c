# Codegen

> **Status:** This directory is an empty placeholder. There is no standalone codegen
> module and no `cc_codegen_emit` entry point — code emission lives in the AST-visitor
> layer at [`cc/src/visitor/visit_codegen.c`](../visitor/visit_codegen.c). This file is
> kept as a marker for a possible future extraction; see
> [`cc/docs/ARCHITECTURE.md`](../../docs/ARCHITECTURE.md) for the actual pipeline.

Intended scope (currently implemented in `visit_codegen.c`): emit C11 for CC constructs — sync functions lower directly; async functions become state machines; UFCS desugaring, channel ops (async vs sync), `send_take` ownership transfer, slice ABI, and results lowering.

