# pigz_cc

This directory contains the feature-complete Concurrent-C implementation of `pigz`.

## Relationship to `pigz_idiomatic.ccs`
While this directory aims for a production-ready, feature-complete tool, a simplified reference implementation is maintained at `../pigz_idiomatic.ccs`. That version is kept as a clean, "easy to debug" example of core Concurrent-C patterns (Result types, Arena ownership, etc.) without the full complexity of production features like dictionary chaining.

## Goals
- Achieve 1:1 feature parity with original `pigz`.
- Demonstrate high-performance structured concurrency.
- Implement advanced patterns like dictionary chaining and ownership transfer.

## Structure
- `pigz_cc.ccs`: Main entry point and implementation.
- (Additional files will be added as the project grows)
