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
| `CC_TCC_EXT_AVAILABLE` | Sentinel macro for Concurrent-C compile paths |
| `pp_line` negative-delta fix | Preserve user `#line` resumes after synthetic injections |
| dwarf `unsigned i` locals | Quiet `-Wsign-compare` on two readers |

## Retired (do not reintroduce)

| Piece | Why gone |
|-------|----------|
| `cc_ast_record*` / `CCASTStub*` / `cc_ast_record.h` | Stub-AST side table for the deleted visitor front |
| `cc_tcc_parse*_to_ast` / `cc_tcc_free_ast` | Parse-to-stub-AST API; zero product callers |
| `TCCExtParser` / `tcc_set_ext_parser` | External parser hooks |
| UFCS host-parse tolerance / `cc_ufcs_*` TCCState | Product UFCS is `shadow_lower`; lowered C is ordinary C |
| `TOK_CC_ARROW` (`=>`) | Closures lower before libtcc |
| `CC_REC_*` recording macros | Recording APIs retired |
| Column / `cc_tok_off` tracking | Existed for stub-AST provenance |

## Sentinel

Patched builds must have `CONFIG_cc_ext=yes` in `third_party/tcc/config.mak`
and produce `libtcc.a`.
