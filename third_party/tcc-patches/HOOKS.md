# TCC Extension Points for Concurrent C

Technical reference for the hooks left in TCC after the legacy multipass front
was removed. Product lowering is native `shadow_lower`; libtcc is used for
comptime / `cpp_expand` / `--exe` on already-lowered C.

All extensions are guarded by `#ifdef CONFIG_CC_EXT`.

> See also: [ARCHITECTURE.md](ARCHITECTURE.md), [README.md](README.md).

## What remains

| Piece | Role |
|-------|------|
| `CONFIG_CC_EXT` / `--config-cc_ext` | Build flag wiring in TCC `Makefile` |
| UFCS host-parse tolerance in `tccgen.c` | Still active under `CONFIG_CC_EXT` (PR2 candidate to delete once proven unused on lowered C) |
| `TOK_CC_ARROW` (`=>`) in `tccpp.c` / `tcc.h` | Lexed for accidental sugar; product closures are lowered before libtcc |

## Retired (do not reintroduce)

| Piece | Why gone |
|-------|----------|
| `cc_ast_record*` / `CCASTStub*` / `cc_ast_record.h` | Stub-AST side table for the deleted visitor front |
| `cc_tcc_parse*_to_ast` / `cc_tcc_free_ast` | Parse-to-stub-AST API; zero product callers |
| `TCCExtParser` / `tcc_set_ext_parser` | External parser hooks (`cc_ext_parser.c` deleted) |
| `cc_tcc_set_symsig_sink` / `cc_parser_mode` | Parser-mode-only exports |

`CC_REC_*` macros remain as unconditional no-ops so any leftover call sites
compile without linking recording symbols.

## Sentinel

Patched builds must have `CONFIG_cc_ext=yes` in `third_party/tcc/config.mak`
and produce `libtcc.a`. Do **not** `nm | grep cc_ast_record` — that symbol is
intentionally gone.
