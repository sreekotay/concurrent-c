# Result unwrap & error-syntax lowering

Normative semantics live in `[spec/concurrent-c-spec-complete.md` §2.2](../../spec/concurrent-c-spec-complete.md). This document describes the **compiler implementation** only. It covers both the new-surface pass (`pass_result_unwrap.c`, primary) and the legacy `@err` surface pass (`pass_err_syntax.c`, still live during phases 1-3).

## Passes


| Pass              | Module                                      | Role                                                                                                                                                                                                                                                                                                                                   | Status                |
| ----------------- | ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- |
| Unwrap destroy    | `cc/src/visitor/pass_unwrap_destroy.c`, `.h` | Textual pre-pass. Rewrites the success-destructor suffix `EXPR !> … @destroy { body }` / `EXPR ?> … @destroy { body }` into the original statement followed by a synthesized `@defer { body };`. For built-in owned types (`CCNursery*`, `CCArena`) the body is wrapped with the type's pre-/post-destroy hooks so the nursery/arena lifecycle matches `@create(...) @destroy { ... }`. Runs before `pass_result_unwrap` and `pass_defer_syntax` in both `preprocess.c` and `visit_codegen.c`. | **Active**            |
| Result unwrap     | `cc/src/visitor/pass_result_unwrap.c`, `.h` | Primary. Lowers `?>` (default-value operator, expression-only RHS) and `!>` (error-handler operator, expression- and statement-position) to `cc_is_ok` / `cc_is_err` / `cc_value` / `cc_error`. Implements the `@err(e);` forward inside `!>` bodies, the `@errhandler` divergence check, and the slice-7 unhandled-result diagnostic. | **Active**            |
| Legacy err syntax | `cc/src/visitor/pass_err_syntax.c`, `.h`    | Existing `@err` / `=<!` / `: default` / `@errhandler` handler dispatch pass. Runs after `pass_result_unwrap` so that any `!>` forms lowered into the legacy `@err` shorthand are still processed. Skips `@err(IDENT);` tokens (followed immediately by `;`), which are structured forwards from the new-surface pass.                  | **Live (phases 1-3)** |


Both passes are text-rewrite passes over CC source, converging in a short fixed-iteration loop per invocation.

## Pipeline order

### In `cc/src/preprocess/preprocess.c`

Inside `cc__apply_phase3_host_lowering_passes` (used by the normal preprocess-to-string path):

1. `cc__rewrite_unwrap_destroy_suffix` — guard: source contains `@destroy` *and* (`!>` or `?>`). Rewrites success-destructor suffixes into `@defer` blocks before either the unwrap or defer passes see them.
2. `cc__rewrite_result_unwrap` — guard: source contains `?>` or `!>`, or `CC_STRICT_RESULT_UNWRAP=1`.
3. `cc__rewrite_err_syntax` — guard: source contains `@errhandler`, `@err`, `=<!`, or `<?`.
4. `cc__lower_with_deadline_syntax`
5. `cc__rewrite_match_syntax`
6. `cc__rewrite_optional_constructors`
7. `cc__rewrite_result_constructors`
8. ... (remaining phase-3 passes)

Earlier in phase 2 (`cc__canonicalize_cc_for_comptime` / the type-syntax pass), the result-function name registry is populated: each `T !> (E) NAME(...)` declaration that the type-syntax pass recognizes is added to the registry via `cc_result_fn_registry_add`. The registry is then consulted by the slice-7 scan at the end of `cc__rewrite_result_unwrap`.

### In `cc/src/visitor/visit_codegen.c`

`cc__rewrite_unwrap_destroy_suffix` runs first, then `cc__rewrite_result_unwrap`, then `cc__rewrite_err_syntax` in the codegen-time rewrite block. Re-running the destroy-suffix pass here is required because `visit_codegen` re-reads the raw `.ccs` source rather than the preprocessor output, and the synthesized `@defer { ... };` must be visible to the later `cc__rewrite_defer_syntax` pass inside `visit_codegen.c`.

### In `cc/src/visitor/pass_closure_literal_ast.c`

Closure bodies are re-run through the err-syntax and result-unwrap passes so that `?>` / `!>` / `@err` inside captured blocks lower correctly.

## Operator role split (slice 9)

The two operators have cleanly separated responsibilities:

- `**?>` — default value operator.** Reads `EXPR ?> DEFAULT_EXPR` or `EXPR ?>(ident) DEFAULT_EXPR`. The RHS is a pure C expression producing `T`. No divergent statements, no blocks, no bare shorthand. Analogous to `??` in Swift/C#/JS or `?:` in Kotlin.
- `**!>` — error-handler operator.** Runs at both statement and expression position. At **expression position** the body must *visibly diverge*; at **statement position** the body may fall through. The bare form `CALL !>;` dispatches to the enclosing `@errhandler` (statement position) or inlines the handler body with a synthesized binder (expression position).

## Success destructor — `!> @destroy { ... }` / `?> @destroy { ... }`

Normative semantics: [`spec/concurrent-c-spec-complete.md` §2.2](../../spec/concurrent-c-spec-complete.md). This is the implementation sketch.

### Surface

```
T* p = CALL() !>(e) { … } @destroy { cleanup(p); };
T* p = CALL() !>         @destroy { cleanup(p); };     // no error body
char* s = CALL() ?>(e) "fallback" @destroy { log(s); };
```

`expr !> BODY @destroy { D }` means `(expr !> BODY) @sdestroy { D }`: on *success* of the unwrap, `D` is scheduled to run at scope exit (in reverse-declaration order, alongside any other `@defer`s). On the error path, control has already left the scope via the handler body / handler divergence, so `D` never runs. The same desugaring applies to `?> … @destroy`: `D` runs at scope exit regardless of whether the unwrap took the success or the default branch, because both branches yield a bound value.

### Lowering (`pass_unwrap_destroy.c`)

A textual pre-pass that runs *before* `pass_result_unwrap` and `pass_defer_syntax`:

1. Forward-scan for `@destroy` at top level (comment/string-aware).
2. Confirm the enclosing statement contains `!>` or `?>` at depth 0. If not, the `@destroy` is part of an `@create(...) @destroy` form and is left untouched for `pass_create`.
3. Back-scan to the start of the enclosing statement. The host statement is emitted verbatim up to the `@destroy` token, then the `@destroy { body }` range is replaced with whitespace (preserving line numbers) and a `;` is appended to close the host statement.
4. A synthesized `@defer { BODY };` is emitted immediately after. `BODY` is the user's `@destroy` body, with built-in lifecycle hooks injected for owned types.

### Built-in owned types

If the host statement is a declaration and the declared type matches a known built-in owned type, the synthesized `@defer` body is wrapped with that type's hooks. This keeps the lifecycle identical to the `@create(...) @destroy { ... }` form:

| Declared type | Pre-hook (before user body) | Post-hook (after user body) |
| --- | --- | --- |
| `CCNursery*` (with `*`) | `cc_nursery_wait(name);` | `cc_nursery_free(name);` |
| `CCArena` (no `*`) | — | `cc_arena_destroy(&name);` |

The variable name is extracted by walking left from the top-level `=` in the host statement to the preceding identifier. Any other type falls through with no hooks — `@defer { user_body };` only.

Example: `CCNursery* n = cc_nursery_create() !> { abort(); } @destroy { printf("done\n"); };` expands (conceptually) to

```c
CCNursery* n = cc_nursery_create() !> { abort(); };
@defer { cc_nursery_wait(n); printf("done\n"); cc_nursery_free(n); };
```

### Tests

- `tests/null_unwrap_bang_ok_smoke.ccs` — `!> @destroy` on a plain pointer decl.
- `tests/null_unwrap_qmark_destroy_smoke.ccs` — `?> @destroy` with a fallback value.
- `examples/hello.ccs` — `!> @destroy` on a `CCNursery*` declaration, exercising the built-in owned-type path (`cc_nursery_wait` / `cc_nursery_free` wrapping the user body).

## Text-rewrite strategy (result-unwrap)

All scans are comment- and string-aware. The algorithm:

1. **Forward-scan** for the first `?>` or `!>` operator at depth 0 in a comment/string-free context.
2. **Backward-scan** from the operator to an expression-start boundary — one of `;`, `{`, `}`, `,`, `=`, `(`, `?`, `:`, `&&`, `||`, or SOF — with balanced paren/bracket/brace tracking. For `!>`, the scan also records whether the boundary indicates *statement position* (preceded by `;`, `{`, `}`, or SOF, modulo a labelled-statement prefix) or *expression position* (preceded by `(`, `,`, `=`, `?`, `:`, `&&`, `||`, `@`, or an immediate `return` keyword). The dispatch tables in `cc__rewrite_bang_once` split on this flag.
3. **Optionally consume** a `(ident)` binder. Rules:
  - The `(...)` contents must be a single bare identifier. If not, the `(` is left alone and treated as the start of a parenthesized RHS expression. This keeps `?> (7 + 8)` working as a defaulted expression.
  - Empty `()` and non-identifier contents emit the diagnostic `expected identifier in '!> (...)'` (or the `?>` analogue).
4. **RHS parse for `?>` (value only, slice 9).** The next non-ws token is inspected. If it is `{`, a `return` / `break` / `continue` / `goto` keyword, or an `@err` introducer, the parser emits `'?>' RHS must be a value expression; use '!>' for error-handling logic` at the `?>` line and stops. Otherwise the RHS is scanned as a normal C expression terminated by `;`, `,`, or a matching close-paren, and lowered as a ternary:
  ```c
   ({ __typeof__(CALL) tmp = (CALL);
      cc_is_ok(tmp) ? cc_value(tmp) : (DEFAULT_EXPR); })
  ```
   The optional binder `(e)` is bound inside the `: (...)` branch so that `fallback_for(e.kind)` works.
5. **Body parse for `!>` at statement position** (unchanged from earlier slices):
  - `;` → bare form, dispatch to registered `@errhandler`.
  - `{` → block body. The block is scanned for `@err(IDENT);` forwards; dead-code analysis fires if any statement (comment/ws-stripped) follows a `@err(IDENT);` in the same block.
  - Otherwise → single statement. Body may fall through.
6. **Body parse for `!>` at expression position** (slice 9, `cc__rewrite_bang_expr_once`):
  - `;` (bare) → synthesize a fresh binder (`__cc_pu_be_N`), locate the enclosing `@errhandler` via `cc__pu_find_outer_errhandler`, substitute its parameter name → synthesized binder, and splice the handler body into the if-branch. If no `@errhandler` is in scope, emit `'!>;' at expression position requires an enclosing '@errhandler' in scope`. If the outer handler body does not diverge, emit `@errhandler body must visibly diverge when used as an expression-position '!>;' delegate` at the handler site.
  - `{ ... }` → block body. The block must diverge (`cc__pu_body_diverges`); otherwise emit `expression-position '!>' body must diverge (return/break/continue/goto/@err/exit/abort/etc.)` at the `!>` line.
  - Single statement → likewise, must diverge (`cc__pu_stmt_diverges`). The closing `;` of the body remains in the source so that the enclosing declaration/assignment statement keeps its terminator after the substitution.
  - Lowering shape in all three cases:
    ```c
    ({ __typeof__(CALL) tmp = (CALL);
       if (cc_is_err(tmp)) {
           [__typeof__(cc_error(tmp)) BINDER = cc_error(tmp);]
           <BODY>
       }
       cc_value(tmp); })
    ```
  - The call LHS is evaluated exactly once; `BODY` must diverge so control never falls through the `({ ... })` with an uninitialised `cc_value(tmp)`.
7. **Binder scoping.** The binder `(e)` is only injected into the generated error-branch block, so it is naturally invisible outside that scope. `@err(X);` inside a `!>` body is rejected (with the diagnostic `@err(X) forward references unknown binder`) unless `X` is the immediate binder name.
8. **Handler divergence check.** At each `@errhandler(TYPE NAME) { BODY }` declaration, the pass inspects `BODY` to confirm the last statement (recursively descending into `{ ... }` compound tails) is one of: `return...;`, `break;`, `continue;`, `goto LBL;`, `@err(ident);`, or a call to a hardcoded noreturn name (see allowlist below). Per spec §2.2, divergence is required only when the handler is reached via a non-returning path: an `@err(e);` forward, or an expression-position `!>;` that inlines the handler body. The implementation fires the diagnostic on two code paths accordingly:
  - `[pass_result_unwrap.c:1181](../../cc/src/visitor/pass_result_unwrap.c)` — triggered when an `@err(e);` forward targets a non-divergent handler, emitting `@errhandler body must visibly diverge (end with return/break/continue/goto, @err(e);, or a call to exit/abort/longjmp/etc.)`.
  - `[pass_result_unwrap.c:1468](../../cc/src/visitor/pass_result_unwrap.c)` — triggered when an expression-position bare `!>;` inlines a non-divergent handler, emitting `@errhandler body must visibly diverge when used as an expression-position '!>;' delegate`.
   Statement-position `call() !>;` with a non-divergent handler is fine — control returns to the statement after the call, so the handler body does not need to diverge in that case.
9. **Noreturn allowlist (hardcoded).** `exit`, `_Exit`, `_exit`, `abort`, `longjmp`, `siglongjmp`, `pthread_exit`, `__builtin_unreachable`, `__builtin_trap`. A call to any of these at the tail of a handler body counts as divergence.
10. **Result-fn name registry (slice 7).** Populated by the type-syntax / preprocess pass (`cc_result_fn_registry_add`) when a declaration `T !> (E) NAME(...)` is parsed. The strict unhandled-call scan in `pass_result_unwrap.c` uses `cc_result_fn_registry_contains` to decide whether a bare `NAME(...);` at statement position is a result-typed call that has been discarded.

## Legacy-pass conflict (slice 9)

The expression-position `!>(e) @err(e);` form relies on the `@err(IDENT);` forward being handled by the result-unwrap pass, not by the legacy `pass_err_syntax.c` (which expects `@err(DECL) { BODY }`). To prevent the legacy pass from rewriting new-style forwards, `cc__at_err_postfix` peeks past the balanced `(...)` after `@err` and returns 0 when the next non-whitespace byte is `;`. This single-character lookahead is sound because the legacy surface always requires a compound open-brace after `)`.

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

- `**CC_STRICT_RESULT_UNWRAP=1`** — enables the unhandled-result diagnostic. Phase 3 will flip the default to on.
- No flag is needed to enable `?>` / `!>` parsing and lowering; those operators are live unconditionally.

## Tests (new surface)

### Smoke


| Test                                                     | Proves                                                                                                                                                                                                |
| -------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tests/result_unwrap_basic_smoke.ccs`                    | `?>` with no binder, pure-expression RHS, default value path.                                                                                                                                         |
| `tests/result_unwrap_binder_smoke.ccs`                   | `?>(e)` binder, pure-expression RHS, both success and error branches.                                                                                                                                 |
| `tests/result_unwrap_bang_divergent_return_smoke.ccs`    | `!>(e) return cc_err(...);` at expression position propagates from a nested call.                                                                                                                     |
| `tests/result_unwrap_bang_divergent_break_smoke.ccs`     | `!> break;` at expression position exits the enclosing loop on error.                                                                                                                                 |
| `tests/result_unwrap_bang_continue_smoke.ccs`            | `!> continue;` at expression position skips the loop body on error.                                                                                                                                   |
| `tests/result_unwrap_bang_expr_block_binder_smoke.ccs`   | `!>(e) { ...; return cc_err(...); };` block body with binder, multi-statement divergent tail.                                                                                                         |
| `tests/result_unwrap_bang_expr_block_nobinder_smoke.ccs` | `!> { ...; continue; };` block body with no binder, used inside a loop.                                                                                                                               |
| `tests/result_unwrap_bang_expr_return_smoke.ccs`         | `f() !>(e) return cc_err(CC_ERROR(e.kind, "prop"));` in a declaration initializer (expression position, single-stmt body).                                                                            |
| `tests/result_unwrap_bang_expr_bare_smoke.ccs`           | `f() !>;` at expression position delegates to the enclosing `@errhandler` via synthesized binder.                                                                                                     |
| `tests/result_unwrap_bang_expr_err_forward_smoke.ccs`    | `f() !>(e) @err(e);` at expression position forwards through the legacy-pass skip.                                                                                                                    |
| `tests/result_unwrap_qmark_arg_position_smoke.ccs`       | `?>` used in function-argument, sub-expression, and conditional-test positions.                                                                                                                       |
| `tests/result_unwrap_qmark_return_smoke.ccs`             | `return EXPR ?> DEFAULT;` and `return EXPR ?>(e) …;` — the `return` keyword survives the `?>` lowering's `__typeof__(...)` span, including near identifiers with `return` as a prefix (`returnable`). |
| `tests/result_unwrap_bang_bare_smoke.ccs`                | `call() !>;` dispatches to registered `@errhandler`.                                                                                                                                                  |
| `tests/result_unwrap_bang_single_smoke.ccs`              | `call() !> break;` / `!> continue;` single-statement body.                                                                                                                                            |
| `tests/result_unwrap_bang_block_smoke.ccs`               | `call() !> { ... };` block body with trailing `;`.                                                                                                                                                    |
| `tests/result_unwrap_bang_block_nosemi_smoke.ccs`        | `call() !> { ... }` block body without trailing `;`.                                                                                                                                                  |
| `tests/result_unwrap_bang_binder_smoke.ccs`              | `call() !> (e) stmt` binder with single statement.                                                                                                                                                    |
| `tests/result_unwrap_bang_binder_single_smoke.ccs`       | `call() !> (e) STMT;` with an expression statement body.                                                                                                                                              |
| `tests/result_unwrap_bang_binder_block_smoke.ccs`        | `call() !> (e) { ... };` binder + block body.                                                                                                                                                         |
| `tests/result_unwrap_bang_forward_smoke.ccs`             | `@err(e);` forwards to a divergent `@errhandler`.                                                                                                                                                     |
| `tests/result_unwrap_handler_noreturn_call_smoke.ccs`    | A handler ending in `exit(...)` counts as divergent.                                                                                                                                                  |
| `tests/result_unwrap_side_effect_once_smoke.ccs`         | LHS calls are evaluated exactly once across all `?>` / `!>` forms (ticked counters).                                                                                                                  |
| `tests/result_unwrap_unhandled_off_smoke.ccs`            | Without the strict flag, bare result-call statements still compile.                                                                                                                                   |
| `tests/result_unwrap_unhandled_consumed_smoke.ccs`       | With the strict flag on, `?>`, `!>`, result-typed assignment, `return`, and `(void)` discard all compile cleanly.                                                                                     |
| `tests/result_unwrap_void_cast_smoke.ccs`                | `(void)f()` is the explicit-discard escape hatch under the strict flag.                                                                                                                               |


### Compile-fail


| Test                                                      | Proves                                                                                                |
| --------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `tests/result_unwrap_qmark_missing_rhs_fail.ccs`          | `expr ?> ;` with nothing on the RHS is rejected (`missing default expression after '?>'`).            |
| `tests/result_unwrap_qmark_empty_binder_fail.ccs`         | `expr ?>() RHS` is rejected (empty binder).                                                           |
| `tests/result_unwrap_qmark_bad_binder_fail.ccs`           | `expr ?>(123) RHS` is rejected (non-identifier binder).                                               |
| `tests/result_unwrap_qmark_divergent_rhs_fail.ccs`        | `expr ?>(e) return ...;` — divergent statement on `?>` RHS is rejected (slice 9).                     |
| `tests/result_unwrap_qmark_block_rhs_fail.ccs`            | `expr ?>(e) { return ...; };` — block on `?>` RHS is rejected (slice 9).                              |
| `tests/result_unwrap_qmark_continue_rhs_fail.ccs`         | `expr ?> continue;` — `continue` on `?>` RHS is rejected (slice 9).                                   |
| `tests/result_unwrap_bang_expr_block_no_diverge_fail.ccs` | `expr !>(e) { log(...); };` at expression position whose last statement is not divergent is rejected. |
| `tests/result_unwrap_bang_expr_block_empty_fail.ccs`      | `expr !>(e) { };` — empty block body at expression position is rejected.                              |
| `tests/result_unwrap_bang_expr_no_diverge_fail.ccs`       | `expr !>(e) { printf(...); };` — non-divergent single block at expression position is rejected.       |
| `tests/result_unwrap_bang_expr_bare_no_handler_fail.ccs`  | `expr !>;` at expression position without an enclosing `@errhandler` is rejected.                     |
| `tests/result_unwrap_bang_missing_body_fail.ccs`          | `call() !> (e) ;` is rejected (`expected body after '!> (e)'`).                                       |
| `tests/result_unwrap_bang_empty_binder_fail.ccs`          | `call() !> () BODY` is rejected (empty binder).                                                       |
| `tests/result_unwrap_bang_bad_binder_fail.ccs`            | `call() !> (1e2) BODY` is rejected (non-identifier binder).                                           |
| `tests/result_unwrap_bang_forward_unbound_fail.ccs`       | `@err(X);` references an identifier that is not the enclosing binder.                                 |
| `tests/result_unwrap_bang_forward_deadcode_fail.ccs`      | A statement after `@err(e);` in the same block is unreachable and rejected.                           |
| `tests/result_unwrap_handler_no_diverge_fail.ccs`         | `@errhandler` body without a divergent tail is rejected at declaration site.                          |
| `tests/result_unwrap_handler_no_diverge_void_fail.ccs`    | Same, in a `void`-returning function.                                                                 |
| `tests/result_unwrap_unhandled_bare_fail.ccs`             | With `CC_STRICT_RESULT_UNWRAP=1`, a bare result-typed call is an error (`unhandled-result`).          |


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
- `**void!>(E)`** success values are exercised only lightly; the normative spec allows them but the coverage is not exhaustive.

