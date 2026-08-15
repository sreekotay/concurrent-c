# C23 tokenizer + PEG syntax (SERDES experiment)

**Not a C compiler.** `@grammar(rules)` recognition of ISO C23 *tokens* and a
typedef-free *syntax* of preprocessed translation-phase-7 source. Same factory
shape as `examples/serdes/json`: a shared `.rules` file, `include` + specialize.

```bash
./cc/bin/ccc run examples/serdes/c23/c23_smoke.ccs
```

## What it is

| file | face |
|------|------|
| `c23_tok.rules` | Phase-3 pp-tokens: comments, C23 `1'000`, `u8"…"`, ISO puncts / digraphs |
| `c23_syn.rules` | `include` the tokenizer, then a PEG `translation_unit` |
| `c23_smoke.ccs` | `cc_match` / `cc_collect` over a handful of snippets |

Keywords use `not idcont`, so `ifdef` is one identifier, not `if` + `def`.
Expressions are rewritten non-left-recursively (PEG). Assignment is
right-associative.

## What it is not

- No preprocessor (`#include`, macros, `#if`, `#embed`).
- Identifiers are never recolored as typedef-names. `T x;` / `T *p;` parse
  when `T` sits in specifier position as a bare ident (the experiment's
  stand-in for a typedef-name). `T (*p)` does not: `(` after an ident is
  left to the declarator so `void f(` is a function, not a typedef-name
  `f`. `int (*p)(void);` still works because `int` is a keyword.
  `(foo)(x)` is a cast.
- No GNU / Concurrent-C surface (`=>`, `!>`, `@`, backticks).
- `cc_match` is all-or-nothing; there is no recovery and no AST typecheck.

The native front stays a whitelist AST (`cc/shadow/pp_tok.rules` +
`pp_stmt.rules`). This directory is a SERDES experiment, not a succession path.
