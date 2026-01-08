# Codegen

Emit C11 for CC constructs. Sync functions lower directly; async functions become state machines. Handle UFCS desugaring, channel ops (async vs sync), `send_take` ownership transfer, slice ABI, results/options lowering. Integrates with visitor; expose a `cc_codegen_emit` entry point once ready.

