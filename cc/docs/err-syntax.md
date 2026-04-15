# `@err` / `@errhandler` lowering

Normative semantics live in `spec/concurrent-c-spec-complete.md` (§2.2). This document describes the **compiler implementation** only.

## Pass

- **Module:** `cc/src/visitor/pass_err_syntax.c`, `pass_err_syntax.h`
- **Entry point:** `cc__rewrite_err_syntax(ctx, in, in_len, out, out_len)`
- **Return value:** `1` if the buffer changed, `0` if nothing to do, `-1` on hard error (diagnostic emitted).

## Pipeline order

1. Run **after** passes that must see original CC surface syntax (e.g. closure literal rewrites), **before** `cc__rewrite_defer_syntax`.
2. **`preprocess.c`:** `cc__rewrite_err_syntax_text` runs in **`cc__apply_phase3_host_lowering_passes`** (used by `cc_preprocess_to_string_ex`, i.e. normal parses) before `@errhandler` reaches the external parser; `cc_preprocess_simple` also runs it before `@defer(err)` stripping.
3. **`visit_codegen.c`:** same guard (`@err` or `=<!`), immediately **before** each `@defer` rewrite block (main source, pre-final-UFCS sweep, and closure definitions).

Re-invoke the pass in a short loop (fixed max iterations) until a run returns `0`, so nested expansions converge.

## Token

- Conditional result assignment uses **`=<!`** (one punctuator). Do **not** use `=!` (`a = !a`).

## Lowering strategy

1. **Brace depth** tracks the stack of `{ … }` like `pass_defer_syntax.c`.
2. **`@errhandler(T param) { body }`** is **removed** from the output; `body` (inner statements) and the full parameter list (`T param`) are **pushed** on a handler stack with `reg_depth = depth` at the site of the statement.
3. On **`}`**, pop all handlers whose `reg_depth` equals the current depth **before** decrementing depth.
4. **`expr @err`**, **`expr @err(e) { ... }`**, **`expr @err { ... }`**, **`expr @err stmt`**, **`lhs =<! expr @err`**, **`lhs =<! expr : default`**: find statement bounds, emit C using `cc_is_err` / `cc_is_ok` / `cc_error` (`ccc/cc_result.cch`), temporaries, and colon branches as needed. Statement start scans backward with **paren/bracket/brace depth** so a `;` inside a prior **`@errhandler { … }`** is not mistaken for the end of the previous statement. After a block **`{`**, leading **`@errhandler`** spans are skipped by advancing to the **first non-handler token**, not the handler’s trailing index. For **`lhs =<! … @err`**, declaration-like left sides are emitted as **`lhs;`** followed by success-path assignment to the declared name; non-declaration assignment targets are reused directly. Result-typed declarations assign the **full result** temp, not **`cc_value(__tmp)`** (payload only).
5. **Default handler** for bare **`@err`**: **inline** the innermost stack frame’s body at the **`expr @err`** site, prefixed with `T param = cc_error(__tmp);`.
6. **Local** **`@err(e) { … }`**: emit the local block with `e = cc_error(__tmp)`; **`@errhandler(e);`** inside the local body is replaced by the **next** outer handler body (parameter names substituted).
7. **No-binding local shorthand** **`@err { … }`** / **`@err stmt`**: lower through the same local-body path, but with no synthesized error binding. Delegation via **`@errhandler(e);`** is rejected in these forms.
8. **Break / continue:** bodies are inlined at the use site (never emitted only at the `@errhandler` line), matching the spec.

## Limitations (v1)

- Statement-boundary scans omit full string-literal awareness in a few backward walks; avoid `;` or `@` inside string literals in the same statement as `@err`.
- **`void!>(E)`** success values are not exercised in tests; use non-void results for unwrap.

## Tests

- `tests/err_syntax_smoke.ccs` — default handler, `=<!` + `@err`, `: default`.
- `tests/err_syntax_control_flow_smoke.ccs` — `break` / `continue` run at the `@err` site, not the `@errhandler` line.
- `tests/err_syntax_assignment_delegate_smoke.ccs` — assignment into an existing result variable plus local-handler delegation; string/comment text containing `@errhandler(e);` must not be rewritten.
- `tests/err_syntax_shorthand_smoke.ccs` — no-binding shorthand local handlers (`@err { ... }`, `@err stmt`) plus scalar `=<!`.
- `tests/err_syntax_shorthand_delegate_fail.ccs` — shorthand handlers reject `@errhandler(e);` because no local error binding exists.
- `tests/err_syntax_target_lhs_smoke.ccs` — non-trivial `=<!` targets (`box.field`, `items[idx]`) lower through the direct-assignment path.
- `tests/err_syntax_local_control_flow_smoke.ccs` — named local `@err(e) { break/continue; }` on scalar `=<!` targets.
- `tests/err_syntax_no_handler_fail.ccs` — bare `@err` without any active handler is rejected.
- `tests/err_syntax_delegate_without_outer_fail.ccs` — local delegation requires an enclosing default `@errhandler`.
