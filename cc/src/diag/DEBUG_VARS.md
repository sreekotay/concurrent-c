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

| Variable | Effect |
|----------|--------|
| `CC_BATCH_PHASE3=1` | Experimental: batch UFCS + closure_calls + autoblock + await_normalize in one apply (off by default) |

See [COMPILER_CLEANUP_STATUS.md](../../docs/COMPILER_CLEANUP_STATUS.md).
