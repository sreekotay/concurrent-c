# Result unwrap & error-syntax lowering

Normative semantics live in [`spec/concurrent-c-spec-complete.md` §2.2](../../spec/concurrent-c-spec-complete.md). This document describes the **compiler implementation** only. It covers both the new-surface pass (`pass_result_unwrap.c`, primary) and the legacy `@err` surface pass (`pass_err_syntax.c`, still live during phases 1-3).

## Passes

| Pass | Module | Role | Status |
|------|--------|------|--------|
| Result unwrap | `cc/src/visitor/pass_result_unwrap.c`, `.h` | Primary. Lowers `?>` (expression) and `!>` (statement) to `cc_is_ok` / `cc_is_err` / `cc_value` / `cc_error`. Implements the `@err(e);` forward inside `!>` bodies, the `@errhandler` divergence check, and the slice-7 unhandled-result diagnostic. | **Active** |
| Legacy err syntax | `cc/src/visitor/pass_err_syntax.c`, `.h` | Existing `@err` / `=<!` / `: default` / `@errhandler` handler dispatch pass. Runs after `pass_result_unwrap` so that any `!>` forms lowered into the legacy `@err` shorthand are still processed. | **Live (phases 1-3)** |

Both passes are text-rewrite passes over CC source, converging in a short fixed-iteration loop per invocation.

## Pipeline order

### In `cc/src/preprocess/preprocess.c`

Inside `cc__apply_phase3_host_lowering_passes` (used by the normal preprocess-to-string path):

1. `cc__rewrite_result_unwrap` — guard: source contains `?>` or `!>`, or `CC_STRICT_RESULT_UNWRAP=1`.
2. `cc__rewrite_err_syntax` — guard: source contains `@errhandler`, `@err`, `=<!`, or `<?`.
3. `cc__lower_with_deadline_syntax`
4. `cc__rewrite_match_syntax`
5. `cc__rewrite_optional_constructors`
6. `cc__rewrite_result_constructors`
7. ... (remaining phase-3 passes)

Earlier in phase 2 (`cc__canonicalize_cc_for_comptime` / the type-syntax pass), the result-function name registry is populated: each `T !> (E) NAME(...)` declaration that the type-syntax pass recognizes is added to the registry via `cc_result_fn_registry_add`. The registry is then consulted by the slice-7 scan at the end of `cc__rewrite_result_unwrap`.

### In `cc/src/visitor/visit_codegen.c`

The same `cc__rewrite_result_unwrap` call is repeated immediately before `cc__rewrite_err_syntax` in the codegen-time rewrite block (so lowerings inserted by later codegen passes still see both operators lowered before emission).

### In `cc/src/visitor/pass_closure_literal_ast.c`

Closure bodies are re-run through the err-syntax and result-unwrap passes so that `?>` / `!>` / `@err` inside captured blocks lower correctly.

## Text-rewrite strategy (result-unwrap)

All scans are comment- and string-aware. The algorithm:

1. **Forward-scan** for the first `?>` or `!>` operator at depth 0 in a comment/string-free context.
2. **Backward-scan** from the operator to an expression-start boundary — one of `;`, `{`, `}`, `,`, `=`, `(`, `?`, `:`, `&&`, `||`, or SOF — with balanced paren/bracket/brace tracking. This gives the LHS span (for `?>`) or the call span (for `!>`).
3. **Optionally consume** a `(ident)` binder. Rules:
   - The `(...)` contents must be a single bare identifier. If not, the `(` is left alone and treated as the start of a parenthesized RHS expression. This keeps `?> (7 + 8)` working as a defaulted expression.
   - Empty `()` and non-identifier contents emit the diagnostic `expected identifier in '!> (...)'` (or the `?>` analogue).
4. **RHS parse** for `?>`:
   - If the next non-ws token is the identifier `return` / `break` / `continue` at a word boundary, capture a divergent statement up to its terminating `;` at depth 0.
   - Otherwise scan forward as a C expression up to a statement/expression end marker.
5. **Body parse** for `!>`:
   - `;` → bare form, dispatch to registered `@errhandler`.
   - `{` → block body. The block is scanned for `@err(IDENT);` forwards; dead-code analysis fires if any statement (comment/ws-stripped) follows a `@err(IDENT);` in the same block.
   - Otherwise → single statement.
6. **Binder scoping.** The binder `(e)` is only injected into the generated error-branch block, so it is naturally invisible outside that scope. `@err(X);` inside a `!>` body is rejected (with the diagnostic `@err(X) forward references unknown binder`) unless `X` is the immediate binder name.
7. **Handler divergence check.** At each `@errhandler(TYPE NAME) { BODY }` declaration, the pass inspects `BODY` to confirm the last statement (recursively descending into `{ ... }` compound tails) is one of: `return...;`, `break;`, `continue;`, `goto LBL;`, `@err(ident);`, or a call to a hardcoded noreturn name (see allowlist below). A non-divergent body emits `@errhandler body must visibly diverge` at the handler site. The check fires whether or not any particular `@err(e);` actually forwards to this handler in the current translation unit; the implementation emits the diagnostic only when an `@err(e);` forward targets the handler (matches the slice-5/6 test behaviour — a handler reached only by `!>;` without forwards is not divergence-checked).
8. **Noreturn allowlist (hardcoded).** `exit`, `_Exit`, `_exit`, `abort`, `longjmp`, `siglongjmp`, `pthread_exit`, `__builtin_unreachable`, `__builtin_trap`. A call to any of these at the tail of a handler body counts as divergence.
9. **Result-fn name registry (slice 7).** Populated by the type-syntax / preprocess pass (`cc_result_fn_registry_add`) when a declaration `T !> (E) NAME(...)` is parsed. The strict unhandled-call scan in `pass_result_unwrap.c` uses `cc_result_fn_registry_contains` to decide whether a bare `NAME(...);` at statement position is a result-typed call that has been discarded.

## Unhandled-result diagnostic

Triggered only when `CC_STRICT_RESULT_UNWRAP=1`. It runs as the final step of `cc__rewrite_result_unwrap`, after all `?>` / `!>` sites have already been lowered into `cc_is_ok` / `cc_is_err` temporaries — so by construction any residual `NAME(...)` at statement position is unconsumed.

Gates (all must be true for the diagnostic to fire):

1. `NAME` is in the result-function registry.
2. The character immediately after the balanced `)` (skipping ws/comments) is `;`.
3. The non-ws char immediately before `NAME` is `;`, `{`, `}`, or SOF — i.e. statement position.
4. `NAME` is not preceded by a `(void)` cast.
5. `NAME` is not preceded by the identifier `return` at a word boundary.

Conservative by design: a label prefix (`LBL: f();`) is a false negative, and indirect calls through function pointers are not flagged. See limitations below.

## Transition flag

- **`CC_STRICT_RESULT_UNWRAP=1`** — enables the unhandled-result diagnostic. Phase 3 will flip the default to on.
- No flag is needed to enable `?>` / `!>` parsing and lowering; those operators are live unconditionally.

## Tests (new surface)

### Smoke

| Test | Proves |
|------|--------|
| `tests/result_unwrap_basic_smoke.ccs` | `?>` with no binder, pure-expression RHS, default value path. |
| `tests/result_unwrap_binder_smoke.ccs` | `?>(e)` binder, pure-expression RHS, both success and error branches. |
| `tests/result_unwrap_divergent_return_smoke.ccs` | `?>(e) return cc_err(...);` propagation from a nested call. |
| `tests/result_unwrap_divergent_break_smoke.ccs` | `?> break;` exits the enclosing loop on error. |
| `tests/result_unwrap_qmark_continue_smoke.ccs` | `?> continue;` skips the loop body on error. |
| `tests/result_unwrap_qmark_arg_position_smoke.ccs` | `?>` used in function-argument, sub-expression, and conditional-test positions. |
| `tests/result_unwrap_bang_bare_smoke.ccs` | `call() !>;` dispatches to registered `@errhandler`. |
| `tests/result_unwrap_bang_single_smoke.ccs` | `call() !> break;` / `!> continue;` single-statement body. |
| `tests/result_unwrap_bang_block_smoke.ccs` | `call() !> { ... };` block body with trailing `;`. |
| `tests/result_unwrap_bang_block_nosemi_smoke.ccs` | `call() !> { ... }` block body without trailing `;`. |
| `tests/result_unwrap_bang_binder_smoke.ccs` | `call() !> (e) stmt` binder with single statement. |
| `tests/result_unwrap_bang_binder_single_smoke.ccs` | `call() !> (e) STMT;` with an expression statement body. |
| `tests/result_unwrap_bang_binder_block_smoke.ccs` | `call() !> (e) { ... };` binder + block body. |
| `tests/result_unwrap_bang_forward_smoke.ccs` | `@err(e);` forwards to a divergent `@errhandler`. |
| `tests/result_unwrap_handler_noreturn_call_smoke.ccs` | A handler ending in `exit(...)` counts as divergent. |
| `tests/result_unwrap_side_effect_once_smoke.ccs` | LHS calls are evaluated exactly once across all `?>` / `!>` forms (ticked counters). |
| `tests/result_unwrap_unhandled_off_smoke.ccs` | Without the strict flag, bare result-call statements still compile. |
| `tests/result_unwrap_unhandled_consumed_smoke.ccs` | With the strict flag on, `?>`, `!>`, result-typed assignment, `return`, and `(void)` discard all compile cleanly. |
| `tests/result_unwrap_void_cast_smoke.ccs` | `(void)f()` is the explicit-discard escape hatch under the strict flag. |

### Compile-fail

| Test | Proves |
|------|--------|
| `tests/result_unwrap_qmark_missing_rhs_fail.ccs` | `expr ?> ;` with nothing on the RHS is rejected (`missing default expression after '?>'`). |
| `tests/result_unwrap_qmark_empty_binder_fail.ccs` | `expr ?>() RHS` is rejected (empty binder). |
| `tests/result_unwrap_qmark_bad_binder_fail.ccs` | `expr ?>(123) RHS` is rejected (non-identifier binder). |
| `tests/result_unwrap_bang_missing_body_fail.ccs` | `call() !> (e) ;` is rejected (`expected body after '!> (e)'`). |
| `tests/result_unwrap_bang_empty_binder_fail.ccs` | `call() !> () BODY` is rejected (empty binder). |
| `tests/result_unwrap_bang_bad_binder_fail.ccs` | `call() !> (1e2) BODY` is rejected (non-identifier binder). |
| `tests/result_unwrap_bang_forward_unbound_fail.ccs` | `@err(X);` references an identifier that is not the enclosing binder. |
| `tests/result_unwrap_bang_forward_deadcode_fail.ccs` | A statement after `@err(e);` in the same block is unreachable and rejected. |
| `tests/result_unwrap_handler_no_diverge_fail.ccs` | `@errhandler` body without a divergent tail is rejected at declaration site. |
| `tests/result_unwrap_handler_no_diverge_void_fail.ccs` | Same, in a `void`-returning function. |
| `tests/result_unwrap_unhandled_bare_fail.ccs` | With `CC_STRICT_RESULT_UNWRAP=1`, a bare result-typed call is an error (`unhandled-result`). |

## Legacy `@err` / `=<!` surface

The legacy pass (`pass_err_syntax.c`) remains live and continues to handle:

- `expr @err;` — bare unwrap routed to the enclosing `@errhandler`.
- `expr @err(e) { ... }` — local handler with optional delegation via `@errhandler(e);`.
- `expr @err { ... }` / `expr @err stmt` — no-binding shorthands.
- `lhs =<! expr @err` — conditional assignment variant.
- `expr : default` — inline default value.

Legacy smoke/fail tests that still pass:

- `tests/err_syntax_smoke.ccs`
- `tests/err_syntax_control_flow_smoke.ccs`
- `tests/err_syntax_assignment_delegate_smoke.ccs`
- `tests/err_syntax_shorthand_smoke.ccs`
- `tests/err_syntax_shorthand_delegate_fail.ccs`
- `tests/err_syntax_target_lhs_smoke.ccs`
- `tests/err_syntax_local_control_flow_smoke.ccs`
- `tests/err_syntax_no_handler_fail.ccs`
- `tests/err_syntax_delegate_without_outer_fail.ccs`

These are the phase-1 migration safety net. They will be deleted alongside the legacy pass in phase 4.

## Limitations (v1 of the new pass)

- **Same-block-only dead-code analysis.** Unreachable code after `@err(e);` is detected only within the immediate enclosing `{ ... }`. A `@err(e);` inside a nested `if (cond) { @err(e); }` does not taint later siblings of the outer block.
- **Hardcoded noreturn list.** The divergence check does not consult `_Noreturn` attributes from headers; only the compiled-in allowlist counts as a divergent call.
- **Direct-call-only unhandled scan.** The slice-7 diagnostic only fires on `NAME(...)` where `NAME` is a known result-function identifier. Indirect calls (`fnptr()`, `obj.method()`) and result-typed expression statements that are not single call identifiers are not flagged.
- **Best-effort string awareness on the back-scan.** Statement-start boundary detection trusts that `?>` / `!>` are not embedded inside single-quoted character literals sharing a line with other punctuation. This has not been a problem in practice.
- **`void!>(E)`** success values are exercised only lightly; the normative spec allows them but the coverage is not exhaustive.
