# CC_DEBUG_* environment variables (I6)

> **Historical.** These variables instrumented the removed multipass
> text-rewrite / reparse path. `ccc` is native (`shadow_lower`); use
> `ccc --emit-c-inspect` / `out/.cc-build/native/` for that front.
> Remaining preprocess/comptime seams may still honor a subset.

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
| `CC_DEBUG_CANON` | Log each phase-1 canonical pass as it applies |
| `CC_DEBUG_UFCS_NODES` | Dump every UFCS candidate AST node (file/line/method/recv) |

CLI: `--show-lowered=<phase>` dumps the post-phase buffer to stderr (I7).
`--emit-c-inspect[=PATH]` writes the merged translation unit for inspection.
