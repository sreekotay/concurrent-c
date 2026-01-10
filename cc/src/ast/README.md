# CC AST Side Tables

Store CC-specific metadata (async flags, scoped markers, slice provenance annotations, comptime function markers) without forking TCC node layouts. Prefer side tables keyed by node IDs. Target plan:
- Define a CC AST shim that captures CC constructs (async fn, await, send/recv/send_take, subslice, result/optional, nursery, @match) and source spans.
- Layer side tables over TCC nodes initially; migrate to full CC nodes as parser hooks land.
- Carry type info and slice flags (unique, transferable, subslice) in expression metadata to support compile-time checks (send_take eligibility, move/borrow rules).

