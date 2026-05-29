# CC_DEBUG_* environment variables (I6)

All debug output uses the prefix `[cc:<phase>]`.

| Variable | Effect |
|----------|--------|
| `CC_DEBUG_REPARSE` | Log each `cc__reparse_source_to_ast` stage name |
| `CC_DEBUG_LOWER` | Log major lowering / edit-buffer apply steps |
| `CC_DEBUG_SPANS` | Log source-map insertions and span lookups |
| `CC_DEBUG_DIAG` | Log every `cc_diag_emit` at point of emission |
| `CC_DEBUG_REPARSE_DUMP_DIR` | Write intermediate buffers per reparse stage |

CLI: `--show-lowered=<phase>` dumps the post-phase buffer to stderr (I7).

## Retired

| Variable | Status |
|----------|--------|
| `CC_BATCH_PHASE3=1` | Removed 2026-05-28. Two-stage batched Phase 3 is now the only path; UFCS in stage 1, closure_calls + autoblock + await_normalize in stage 2. See [PIPELINE.md](../visitor/PIPELINE.md). |

See [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).
