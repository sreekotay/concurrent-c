# Visitor & Passes

- Pass 0: const/comptime function collection (populate symbol table).
- Pass 1: single comprehensive visitor for types, moves/provenance, async semantics, @scoped checks, comptime branch selection, monomorph instantiation, and C codegen (sync + async state machines).

