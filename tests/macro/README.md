# Macro-generated CC syntax tests (M5.5)

**Status:** [COMPILER_CLEANUP_STATUS.md](../../cc/docs/COMPILER_CLEANUP_STATUS.md) — M5.5 partial (hooks only).

## Not in CI yet

Examples like `#define CHAN(T) T[~4 >]` require **token synthesis into TCC's lexer** after macro expansion. Today:

- `cc_macro_recognizer_register()` runs at parse time
- Recognizer callbacks are stubs (decline — no synthesized tokens)
- `macro_chan_out_syntax_smoke.ccs` was removed to avoid failing the suite

## When to add tests

After TCC fork wires `tcc_ext_set_type_position_recognizer` to push lowered channel/result/slice tokens:

- `macro_chan_out_syntax_smoke.ccs` — `#define CHAN(T) T[~4 >]`
- `macro_chan_in_syntax_smoke.ccs`, `macro_slice_syntax_smoke.ccs`, `macro_result_syntax_smoke.ccs`
- `macro_postfix_unwrap_smoke.ccs`, `macro_expansion_chain_smoke.ccs` (diag battery)

## Related

- `CC_BATCH_PHASE3=1` — experimental Phase 3 batching (off by default)
