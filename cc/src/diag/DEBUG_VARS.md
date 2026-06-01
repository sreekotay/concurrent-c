# CC_DEBUG_* environment variables (I6)

All debug output uses the prefix `[cc:<phase>]`.

| Variable | Effect |
|----------|--------|
| `CC_DEBUG_REPARSE` | Log each `cc__reparse_source_to_ast` stage name |
| `CC_DEBUG_LOWER` | Log major lowering / edit-buffer apply steps |
| `CC_DEBUG_SPANS` | Log source-map insertions and span lookups |
| `CC_DEBUG_DIAG` | Log every `cc_diag_emit` at point of emission |
| `CC_DEBUG_REPARSE_DUMP_DIR` | Write intermediate buffers per reparse stage |
| `CC_DUMP_LOWERED=<path>` | Write the lowered source handed to TCC to `<path>` |
| `CC_KEEP_PP` | Keep the temporary preprocessed file instead of unlinking it |
| `CC_DEBUG_STUB_NODES` | Dump stub-AST node counts (arenas/nurseries) after parse |
| `CC_DEBUG_PRE_EXPAND` | Log CPP pre-expand attempts and TCC errors |
| `CC_DEBUG_PRE_EXPAND_DUMP=<path>` | Dump the post-CPP-expand buffer to a file |

CLI: `--show-lowered=<phase>` dumps the post-phase buffer to stderr (I7).
`--emit-c-inspect[=PATH]` writes the merged translation unit for inspection.

## Retired

| Variable | Status |
|----------|--------|
| `CC_BATCH_PHASE3=1` | Removed 2026-05-28. Two-stage batched Phase 3 is now the only path; UFCS in stage 1, closure_calls + autoblock + await_normalize in stage 2. See [PIPELINE.md](../visitor/PIPELINE.md). |
| `CC_PRE_EXPAND` | Inert (collapsed 2026-05-29). CPP pre-expand is now the only initial-parse path; the opt-out has no effect. |
| `CC_PRE_EXPAND_REPARSE` | Removed 2026-05-26. Was an opt-in CPP-expand of the final reparse buffer; broke AST/visitor coordinate alignment. |

See [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).
