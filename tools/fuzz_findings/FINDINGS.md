# Findings

Triaged reproducers live beside this file; the logs record every hit.

## Fixed

Three comment-inertness bugs found by `fuzz_comment_insert.shcc` at seed 1,
all pinned by `tests/comment_inert_boundaries_smoke.ccs`:

- `?>` / `!>` binder scan skipped whitespace only, so `mk(41) ?>/*x*/ (e) 0`
  lost the binder and failed to parse.
- The inferred-Result-constructor pass looked for a function body's `{` with a
  whitespace-only scan, so `bool !>(CCError) f(int v) /*x*/{` left the
  enclosing function's ok/err types unset and `cc_ok(...)` stayed generic.
- The closure-literal pass looked for the capture list after `=>` with a
  whitespace-only scan, so `() =>/*x*/ [wp] { … }` was reported as the retired
  `[captures]() =>` syntax.

One emit-block hole found while probing factory error paths, pinned by
`tests/comptime_factory_arg_oob_fail.ccs`:

- `arg(i)` past the type-argument count interpolated nothing and the build
  SUCCEEDED with a malformed fragment. `arg(i)` is now bounds-checked and
  reports through `cc_emit_error`; the factory site fails the build.

## Fixed — `@destroy` ran on a declaration that never executed

Function-scope cleanups all run from one `__cc_cleanup_N:` label that every
early exit jumps to, and the label ran every registered cleanup — including
ones whose declaration sat after the exit and had never executed, destroying
an uninitialized value. Being an uninitialized read it was layout-dependent:
`py_unreached_destroy_after_early_return_crash.shcc` faulted releasing a
`CCPyObj`, while the same shape with a `CCArena` survived.

Fixed with a per-function high-water mark: each function-scope cleanup stamps
`__cc_defer_hw_N` at its declaration, and the label runs cleanup k only when
the mark passed it. Function-scope declarations execute in source order, so
reaching the k-th implies the first k-1 also ran. Pinned by
`tests/destroy_unreached_decl_smoke.ccs`.

## Fixed — comment inertness at declaration, call and sigil boundaries

Seed 7 (800 iterations) went 42 findings -> 0.  Every one was the same root
cause in a different pass: a scan for a delimiter that skipped whitespace but
not comments, so a comment in the gap made the construct look absent.  A few
instead copied a token span verbatim, so comment bytes reached a name mangler
and landed inside a C identifier.

By pass:

- `edit_buffer.c` — the backward scan from a body `{` to the parameter list's
  `)` decides what counts as a function definition; a comment there moved the
  closure-declaration insertion point past the function that needed it.
- `errhandler_lookup.c` — the scan from `)` to the handler body took the
  single-statement branch, whose statement scan then ran past the closing
  brace and swallowed the following statement.  Silent, not diagnosed.
- `preprocess.c` — type-argument capture copied the raw span, so
  `Pair::[int, int]` mangled to `Pair_int/ptrxptr/_int`.  Captures now route
  through `cc__copy_type_arg_text`, and separator scanning skips comments so a
  `,` inside one is not a split point.
- `preprocess.c` — the `!>(E)` error-type trailing trim, every gap in
  `T !>(E) name(params) {`, the `cc_ok`/`cc_err` name-to-paren gap, and the
  `@comptime for` / `@comptime if` heads.
- `preprocess.c` — `cc__skip_leading_decl_specs` stopped at a comment sitting
  at a statement head, so the type span began with the comment.
- `text.h` — the shared declaration parser copied the type span verbatim, so a
  comment inside declaration specifiers became part of the recorded type and
  the receiver went untyped.  One fix, all three local-type index builders.
- `ufcs.c` — the method-name-to-`(` probes on both the call and declaration
  sides, and the arity probe: a call whose parentheses hold only a comment has
  arity zero, not one.
- `emit_plan.c` — all 20 intrinsic-call lexers (`cc_emit_cstr`,
  `cc_emit_format`, `cc_generic_register`, `cc__ci_collect_instantiate`).
- `pass_create.c` — the backward walk from `@create` to its `=`.
- `pass_channel_syntax.c` — the element-type start for a channel handle.  A
  comment at the statement head made `int[~1 >]` classify as a pointer
  element, so the program built and returned the wrong answer.
- `pass_unwrap_destroy.c` (`@destroy` body), `pass_result_unwrap.c` (`!>`
  binder), `cc_closure_markers.c` and `pass_closure_literal_ast.c` (closure
  arrow, `@unsafe` prefix).

`cc_skip_ws_len`, the comment-blind forward skip, is retired: all 66 uses were
inter-token gaps, so all 66 became `cc_skip_ws_and_comments`.

Pinned by `tests/comment_inert_boundaries_smoke.ccs` and
`tests/comment_inert_decl_positions_smoke.ccs`; the reproducers beside this
file all pass.

## Fixed — backward matching and span trimming

Third round.  The residue after the forward-scan sweep was a different shape:
backward BRACKET MATCHING that counted `(`/`)` on raw bytes, and span trims
whose consumers then read the edges as tokens.

The two silent wrong answers both came from `cc__is_if_controlled_return` /
`cc__is_if_controlled_stmt` (`pass_defer_syntax.c`), which decide whether a
braceless `if (...) STMT` needs a brace wrap before a `@defer` injection.
Answering no turns

    if (guard == 0) { retval = 10; ret_set = 1; goto cleanup; }

into

    if (guard == 0) retval = 10; ret_set = 1; goto cleanup;

— an unconditional `goto` and an uninitialized return value, with no
diagnostic.  Three defects fed it: a whitespace-only rewind to the `)`, a raw
backward paren match, and a whitespace-only rewind from `(` to `if`.  The raw
match also miscompiled `if (strcmp(s, "(") == 0) return x;` with no comment
involved at all.  Pinned by `tests/defer_if_controlled_inert_smoke.ccs`.

Also fixed: `cc__va_trim` (`variant_lower.c`), whose five consumers classify a
span by its edge bytes — five reproducers, one function; the backward paren
matchers in `edit_buffer.c` and `cc_closure_markers.c`, both the same shape as
the defer one; the UFCS destination-type span; the channel bracket-spec
tokenizer; the `@errhandler(e);` delegate split; the `@with_deadline(...)`
body probe; closure capture-list entries; and the `__typeof__` callee probe in
async frame lifting.

`cc_rfind_char_top_level(s, lo, rparen, "")` is the canonical backward
paren match: excluding the `)` leaves the opener unmatched, so the masked
scan stops just past it with comments and strings already skipped.

## Fixed — comment-insertion soak, all seven reproducers

Four groups, four different reasons a comment stopped being filler.

- **Sigils and `ident::[` matched their delimiter as a literal.**
  `memcmp(src + i, "@slice(", 7)` and friends required the `(` or `[` to be
  the very next byte, so a comment in that gap hid the construct and it reached
  the C compiler unlowered.  Two helpers now match the token and then hop the
  gap: `cc__sigil_delim_after` for `@slice` / `@string` / `@emit` / `@link`,
  `cc__ident_generic_bracket` for `Name::[`, the latter also covering the
  built-in container spellings whose name and `::[` were one literal.

- **The UFCS receiver back-walk skipped horizontal whitespace only.**  That is
  by design — a newline before `.` ends the receiver — so the naive swap to
  `cc_rskip_ws_and_comments` would newly lower multi-line chains.
  `cc__rskip_hspace_and_comments` skips comments but stops at a real newline,
  and a block comment that itself spans a newline stops it too.  The captured
  receiver expression also has a trailing comment trimmed: leaving it in ended
  the text with a comment close rather than a paren, so the call-receiver chain
  hoist declined and the hop stayed unlowered.

- **The AST path trusted an end column computed against comment-free text.**
  This was the group filed as unlocated.  tcc reports columns after comments
  are gone, so a comment INSIDE a call shifts the closing paren's column while
  the `.` before it keeps its own: the member verifies, the primary path is
  taken, and the span ends short of the real `)`.  A comment before or after
  the call shifts the `.` too, the verification fails, and the text scanner
  picks it up — which is exactly why only the inside case ever misbehaved, and
  why the receiver-side skips did not explain it.  The span end now comes from
  scanning the text.  The same pass's fallback scanner also skipped whitespace
  only between a method name and its `(`.

- **`comment_err_syntax_smoke_off509` was a fuzzer artifact**, as filed: it
  split the single token `=<!`, and `b = <! always_ok()` fails identically with
  a plain space.  The operator list was missing `=<!`, `<?` and `<!`; it has
  them now and the reproducer is retired.

## Fixed — the UFCS reparse gate was comment-blind

Filed as "a declaration initializer loses its UFCS call to a comment", which
was mis-scoped: EVERY position loses the call.  Statement-position arena calls
only appeared immune because they lower textually in preprocess before the
gate is consulted.

The defect was not in the recorder, the span probes, or the edit builder — all
comment-aware by now — but in `cc__has_final_ufcs_candidate`, the cheap
textual gate that decides whether the UFCS reparse runs AT ALL.  It skipped
spaces and tabs between a method name and its paren, so `a.detach/*c*/()`
failed the gate, the reparse never ran for the TU, and the call reached the C
compiler verbatim — reported as a missing struct member, nowhere near the
cause.

A gate must accept everything its pass would.  A false positive costs one
reparse; a false negative silently loses the pass for the whole TU, which is
the worst version of the no-silent-degradation rule because nothing anywhere
records that a decision was even made.  Both gaps now skip comments.

Pinned by `tests/ufcs_comment_gate_smoke.ccs`, whose ONLY candidate site
carries the comment, so a regressed gate takes the whole TU with it —
mutation-checked by restoring the whitespace-only skip, under which the test
fails.

## Fixed — loop-controlled return under a cleanup injection hung

`cc__is_if_controlled_return` / `cc__is_if_controlled_stmt`
(`pass_defer_syntax.c`) recognize only `if`, so a braceless
`while (cond) return x;` gets no brace wrap when a multi-statement cleanup
is injected.  The injection then lands inside the loop body:

    while (n == 0) __cc_retval = 10;

which never exits — the program hangs rather than returning.  Reproducer:
`loop_controlled_return_defer_hang.ccs` (arena `@destroy` plus one `@defer`;
neither alone is enough to trigger it).

Not comment-related, but the same predicate as the fixed comment bugs.  It
now accepts any head that controls a single statement — `if`, `while`, `for`,
and the `else` arm.  Wrapping one that did not need it is harmless, since a
block is a statement; missing one puts the injection inside a loop body.
Pinned by `tests/defer_loop_controlled_return_smoke.ccs`.

## Fixed — a void ok type was emitted as a struct with no Result behaviour

Three generator sites emitted a bare `typedef struct { bool ok; union {...} }`
for a void ok type instead of running the spec generator.  The struct alone is
not a Result: without `cc_ok_`/`cc_err_` the constructors stay undeclared, and
without `_is_err`/`_error` the sigil has nothing to test, so `!>` let execution
straight past a failure.  Both symptoms — `cc_ok()`/`cc_err()` refusing to
specialize, and `!>` silently continuing — were the one cause.

`CC_DECL_RESULT_SPEC_VOID` now generates the void shape and all three sites use
it: `preprocess.c` (parser-mode declarations and the delayed pass) and
`result_spec.c::cc_result_spec_emit_decl` (the real compile).  The two
hand-written specs are gone with it.

Pinned by `tests/result_bang_propagates_grid_smoke.ccs`, which walks ok type x
error type x position and asserts each FAILING call reaches its handler — the
one shape of test that can see a swallowed error, since a silent continue is
otherwise indistinguishable from success.  All four arms now propagate.

## Fixed — a Result declared only in an included header still swallowed

The `_Generic` arms for the unwrap primitives are enumerated per collected
spec, and the seeding walked only the STDLIB predeclared list.  A Result
declared in any other header — `CCResult_void_CCPyError` in `py.cch` — got no
arm, so `__cc_uw_is_err` fell to its default arm, which reads the first
pointer-word of the struct and compares it to NULL.  That is meaningless for a
`bool ok` layout, so `!>` walked past failures.

The lowering was correct the whole time — the emitted C reads
`if (__cc_uw_is_err(r)) { ...handler... }`.  Reading the emitted artifact is
what located this; two hypotheses ahead of it (multi-phase init, feeding the
preprocess spec table) were wrong and were reverted.

`visit_codegen.c` now also seeds from the result-fn registry, which holds the
concrete `CCResult_T_E` names seen in headers this TU includes — so naming one
in an arm is safe by construction.  Restricted to spellings whose mangled and
source forms coincide; valued header-only specs need unmangling the registry
does not carry yet.

## Withdrawn — an isolated interpreter's `exec` can silently do nothing

This never existed.  With propagation restored, an isolated interpreter
imports `cc.host` and runs `exec` correctly, and each interpreter gets its own
module instance with its own state — pinned by
`tests/py_host_module_isolated_smoke.ccs`.  The original symptom was the
swallowed `ModuleNotFoundError` above, raised correctly because the namespace
was not installed there at the time.



`py_second_interp_exec_silent.shcc`: with two interpreters, `p2.exec` of a
block whose first line is `import cc.host` reports success, exits 0, and
leaves p2's `__main__` empty — the assignment on the following line lands
nowhere.  The same block without the import runs correctly in p2, and the
same block WITH the import runs correctly when p2 is the only interpreter.

Narrowed but not solved:

- Not the attach or the GIL handoff.  `tests/py_isolated_interp_smoke.ccs`
  exercises interleaved calls across two interpreters and passes, and without
  the import line p1 and p2 keep independent `__main__` (verified by eval).
- Not the shared `PyModuleDef`.  Each interpreter now gets its own copy —
  a def is itself a PyObject and a single-phase-init one may not be shared —
  and the behaviour is unchanged.
- Not a swallowed error.  The exec returns ok and the script exits 0; with
  the namespace no longer installed in isolated interpreters, `import cc.host`
  there should raise ImportError and does not.

Multi-phase init (PEP 489) was tried as the fix and REGRESSED the working
case: with `m_slots` carrying `Py_mod_exec` and
`Py_mod_multiple_interpreters`, and the module built with
`PyModule_FromDefAndSpec2` + `PyModule_ExecDef`, the SINGLE-interpreter host
module smoke test began failing with the identical symptom — an `exec`
containing `import cc.host` reporting success and setting nothing.  Reverted.

That is the sharpest evidence so far, and it moves the suspect off the module
machinery: the construction path is what breaks it.  `cc__py_module_new` had
to `ImportModule("importlib.util")` and call `spec_from_loader` to obtain a
spec, i.e. it runs real Python during `cc_py_new`.  The next thing to try is
building the spec without importing importlib — or deferring installation
until after the first `exec` — and instrumenting `cc__py_main_globals()` to
print the identity of the dict it hands to `builtins.exec`.

The `cc` namespace is therefore installed only in the process interpreter for
now.  These modules use single-phase init, which CPython does not support in
more than one interpreter; multi-phase init (PEP 489) with
`Py_mod_multiple_interpreters` is required before the namespace can be
installed in an isolated one, and is the same work that makes a host module
importable under `cc_py_new`'s second handle.


## Fixed — a sigil in template prose is text again

`cc__ir_node_sigil_offset` located a node's `!>` / `?>` with a plain `strstr`,
on the stated guarantee that the IR carver had already ensured the node's only
sigil was real code.  Template lowering invalidates that guarantee: the
template becomes a C string literal INSIDE the same span, `strstr` finds the
sigil in the user's text, and the rewrite splices `@err` there.  The script
register is where it shows, because a `.ccs` lowers templates before the sigil
pass runs.  Both users of the offset now scan lexically.

The fix filed here — a template MODE in `CCInertScan` and `CCScannerState`,
inert prose with `${...}` as code — was written and then REVERTED.  It is the
wrong shape twice over.  It broke two tests immediately: `${{ ... }}` verbatim
spans can contain a backtick, and an `@emit` template deliberately carries
`@comptime if` / `for` blocks and bare splices that later passes must see, so
"template contents are inert" is false for the register that generates code.
And it was unnecessary: the one `strstr` was the whole defect.

Pinned by `tests/string_template_sigil_inert_smoke.shcc`, a `.shcc` on purpose
— the same source as a `.ccs` passes with or without the fix.

## Fixed — two spellings sharing one Result box now say so

`const` is stripped from a Result's ok type by `cc__skip_leading_decl_specs`,
whose keyword list cannot tell `const` qualifying a return type from `static`
qualifying a declaration.  That is the intended rule — a box holding `char*`
and one holding `const char*` are the same box, and C applies `const` at the
binding site — but a program declaring BOTH spellings silently got the first
one's const-ness for both.

Detection could not live at `cc_result_spec_table_add`: by the time a spec
reaches it the qualifier is gone and both declarations look identical.  It
happens where the span is captured, in a `_ex` variant that reports which type
qualifiers it ate.  Storage class is not tracked — it legitimately varies
between declarations of one box; `const`, `volatile`, `restrict` and `_Atomic`
do not.

A WARNING rather than an error, which the attempt to make it an error showed
was the right call: `result_type_canonical_smoke` deliberately declares both
spellings to pin the sharing rule, so failing would have contradicted the
documented behaviour.  The defect was the silence, not the sharing.  The
warning is pinned by that test's `.build_stderr`.

## Fixed — field reflection reports `@as`, so composition is walkable

`f.is_as` is emitted as 0/1, so `@comptime if (f.is_as)` guards a walk over
composition:

    @comptime for (f in type_of(Wrap).fields) {
        @comptime if (f.is_as) {
            @comptime for (m in type_of(f.type).methods) { ... m(&w.f) ... }
        }
    }

Two pieces were missing.  The parse recorded no marker — both spellings reach
the member scan, since the parse-input rewrite turns `@as` into a block-comment
marker — and the declarator parser then choked on the marker itself, reporting
`Wrap` as having unsupported field forms.  The marker is an attribute on the
member, not part of its declarator, so it is stripped after normalization.

Pinned by `tests/comptime_as_composition_smoke.ccs`: only the `@as` member is
walked, an ordinary `int` field beside it is skipped, and the projection
`&w.base` is correct because the user wrote the field.

## Not a defect — comptime braces delimit a splice, they do not scope

Recorded here as "`@comptime if` does not evaluate a negated condition", on the
evidence that a two-arm shape selection redeclared its locals.  The predicate
was never the problem: `!` has always folded, and the merged TU shows each arm
selected correctly, one per method.

What redeclared was the splice.  `@comptime if` and `@comptime for` are text
unrollers — the braces delimit the text to splice and introduce no scope — so
two iterations declaring `r` declare it twice in the enclosing scope.  Nothing
can wrap the splice, because the same constructs emit declarations at file
scope, where a block is a syntax error.  An explicit inner block is the whole
fix, and it is a rule rather than a workaround.

The diagnostic pointed at the closing brace of the loop rather than at either
declaration, which is what made a scope collision read as a live predicate.

Pinned by `tests/comptime_splice_scope_smoke.ccs`: a negated arm selecting on
its own, a two-arm selection whose arms differ in shape (one consumes a Result)
under an explicit per-iteration block, and an unbraced body that may only
declare a name once across every iteration.  Stated in spec §14.4 and §14.11.

## Not a defect — generated code consumes Results through the value API

Recorded here as "generated code cannot use the Result surface", on the
evidence that an `@emit` fragment is spliced as host C after the passes that
lower CC syntax, so a template containing `!>` or `@errhandler` fails with
`'@' statements require CC external parser`.

That part is true and stays true.  The conclusion drawn from it was wrong: a
generated wrapper does not need the sigil, because `cc_is_ok` / `cc_value` /
`cc_error` are plain macros over the Result struct, and `__typeof__` spells the
box without evaluating the call — so the mangled instance name never has to
appear in the template either:

    __typeof__(m m.args) r__ = m m.args;
    if (!cc_is_ok(r__)) return -1;
    return (int)cc_value(r__);

Pinned by `tests/comptime_generate_wrappers_smoke.ccs`, which now generates all
three shapes including the fallible one.

What remains true is narrower and worth stating on its own: the ergonomic
Result surface is the sigil, the sigil is statement-shaped, and generated code
is one of several places that cannot reach it.  See the next entry.

## Fixed — `!>` composes in a subexpression

`f(g() !>)`, `if (g() !> != 7)` and `int a = g() !>, b = 3;` were all syntax
errors, so every one cost a named temporary first.

`!>` has no closing delimiter — a handler body runs to its `;` — so the token
after it decides which form was written, and the bare form was only recognised
before a literal `;`.  It now applies wherever the next token CANNOT BEGIN A
STATEMENT: a closer or separator, or an operator that is only ever infix.  The
lowering was already a self-contained expression, so nothing else had to
change.

Prefixes that can open a statement still read as a handler body, because
`f() !> *p = 0;` is equally a multiplication and a body and neither reading is
inferable.  Parenthesising says which was meant, and `)` is a terminator, so
`(f() !>) * p` always works.

Pinned by `tests/result_bang_expression_position_smoke.ccs`; stated in spec
§"Unwrapping Results" under Form selection, which also settles the older
sentence claiming `!>` "works at both statement and expression position" —
what that meant is now written down.

Still open, and pre-existing: `int q[2] = { f(1) !>, 3 };` fails with
`expression expected before '__typeof__'`.  A braced initializer is claimed by
the statement-level path in `pass_err_syntax.c`, which wraps its lowering in a
plain `{ ... }` block rather than a statement expression — valid where a
statement is expected, not inside an initializer.  Identical failure with and
without the change above.

## Fixed — a header Result box is declared before its first use

Recorded here as "a valued `!>` return does not parse in a `.cch`".  The
spelling was never the problem; placement was.  `cc_lower_header_string`
emitted every collected box at the END of the lowered header, before the
closing include guard — after the functions whose return types name them.

That was invisible because the prelude predeclares the single-word instances,
so `int !>(E)` and `double !>(E)` compiled on the strength of the prelude and
the late declaration was dead code.  `long long !>(E)`, `unsigned int !>(E)`
and any user struct failed with `';' expected (got '<fn>')`.

Each box is now spliced immediately before its own first use, anchored to the
start of the last line that began at brace depth 0 — so an ok type declared in
the same header is already declared by then, and a box first named inside a
function body still lands at file scope.

The knock-on matters more than the parse: fallibility is reflected from the
`!>` spelling, so a library declaring its API in a header could not express
fallibility in a form `m.fallible` could see.  It can now.

Pinned by `tests/result_header_multiword_ok_smoke.ccs`.

## Fixed — local `.cch` lowering outside the repo root

`cc__build_stable_lowered_header_path` derived the lowered path from the repo
root and returned -1 when there was none, so lowering returned NULL, the
include stayed pointing at the `.cch`, and CPP inlined raw CC syntax.  Cost an
afternoon twice, the second time while writing the repro for the entry below.

Headers outside a repo now lower to `$TMPDIR/cc-lowered-<uid>/<hash>/`, keyed
by a hash of the absolute path so the location is stable across runs and two
same-named headers cannot collide.  A root that does not CONTAIN the header
counts as no root: the lookup falls back to the working directory's repo, which
is how a scratch-directory header got mapped nowhere.  The remaining give-ups
(dirname, mkdir, read, write) now name the header and the step.

Pinned by phase 2 of `tools/out_of_tree_smoke.shcc`.  The fixture carries a
generic FACTORY on purpose: a header's Result declarations survive without
lowering because they are scanned out of the include text in memory, so a
header full of `!>` returns passes either way and proves nothing.  A factory is
harvested from the lowered header, so it is the shape that needs lowering to
have happened.

## Fixed — the header `@comptime` stripper is backtick-aware and reports

`cc__strip_comptime_blocks_header` tracked comments, strings and char literals
but not backtick templates, so an apostrophe in template prose opened a char
literal that never closed and the block's own `}` was never found.  That made
`body_r <= body_l` true, which returned NULL for the WHOLE file: every
`@comptime` block stayed in the lowered `.h` and surfaced far away as
`declaration expected` in generated C.

Both scanners in the pass now treat a template as one lexical region, and an
unterminated block reports its own line instead of silently disabling the pass.

Pinned by `tests/header_comptime_backtick_smoke.ccs`.  Getting a fixture that
actually trips it took two tries: `CC_GENERIC_FACTORY` contains no literal
`@comptime` text, so a factory-only header never enters this pass at all.  It
needs a literal `@comptime` block whose template prose carries an odd number of
apostrophes, and a second block after it that must still be stripped.

## Fixed — the required compiler features are written down

`_Generic` (133 uses), GNU statement expressions and `__typeof__` are all required by the
lowering and appeared nowhere in `STABILITY.md`, which mentioned neither
Windows nor any required compiler feature.  Both constraints were found while
choosing a spelling for `cc_static_assert`, not by anyone's build.

`STABILITY.md` now carries them as a FEATURE LIST rather than a standard
version, since that is what a toolchain is checked against: naming C11 for
`_Generic` overstates the bar and rules out targets that work, while statement
expressions are the constraint that actually keeps MSVC out.

Removing the statement expressions means relowering `!>` and `?>` — a large
piece of work, still not urgent, and no longer an unwritten assumption.

## Fixed — the parse sessions run at the host's C version

`libtcc.c:1569` defaults `cversion = 199901` and none of the in-process
sessions (`executor.c`, `const_eval.c`, `cpp_expand.c`) passed `-std=c11`,
while the host got it.  Headers therefore took different branches during the
parse than in the real compile: glibc's `assert.h` substituted its pre-C11
compat macro for `_Static_assert` and silently discarded the message, and
`cc_atomic.h` warned `TINYC without C11: non-atomic CAS fallback`.

The worry when filing this was that forcing C11 on the parse would lie in the
other direction, since embedded C99 toolchains are a target.  It does not: the
host profile probe compiles a program using C11 `max_align_t` and FAILS THE
BUILD otherwise, so a C99-only host is already rejected.  The parse running C99
was the side that did not match anything.

One constant, `CC_HOST_C_STD_OPTION`, now feeds the probe's own flag, all four
in-process sessions, and both legacy tcc paths, so the requirement and the
sessions cannot drift.  Both symptoms are gone: `__STDC_VERSION__` reports C11
through the parse and the CAS warning no longer fires.  `STABILITY.md` states
`max_align_t` as the C11 dependency it is.

## Fixed — a `CC`-prefixed receiver now gets the whole dispatch ladder

Filed as two findings, which turned out to be one: for a receiver whose type
carried the `CC` prefix, a composed-name miss was TERMINAL.  Neither the
PascalCase family spelling nor the universal bare-name tier ran.

    | declaration                     | Widget | CCThing (before) |
    | ------------------------------- | ------ | ---------------- |
    | snake twin  `<snake>_area`      | works  | works            |
    | family      `<Type>_scale`      | works  | FAILS            |
    | bare name   `peek(T*)`          | works  | FAILS            |

The cause is one condition in `preprocess.c`, in the text UFCS pass:

    if (!scalar_literal && composed &&
        (is_cc_std || cc__ufcs_fn_name_in_text(src, n, wildcard_callee)))

`is_cc_std` is just "the type name starts with `CC` + uppercase".  It
short-circuited the declaredness check, so the text pass CLAIMED every
CC-prefixed member call and emitted `cc_thing_scale(&x, 7)` whether or not that
function existed anywhere.  A claim is final, so the AST pass — which owns both
missing tiers — never saw the call, and the error named a function the user had
never written.

It was standing in for a probe, not for a policy: stdlib callees are declared
in headers, and the in-text check alone cannot see them before the header
splice.  The sink path 200 lines earlier already had the right predicate:

    int real = composed && (cc__ufcs_fn_name_in_text(src, n, wildcard_callee) ||
                            cc_included_cch_contains_fn(wildcard_callee));

Using `real` restores compose-then-verify at both sites.  Scalar receivers are
excluded explicitly — their families take the receiver BY VALUE when the
declared first parameter says so, which is derived downstream, so claiming them
in the text pass would emit `&recv` for a value parameter.

Additive: 643 identical, 0 violations.  Pinned by
`tests/ufcs_cc_prefixed_ladder_smoke.ccs`, which walks all three tiers for a
plain and a CC-prefixed type.  `m.member` is now correct for every declarable
spelling, so a generator's export list matches the callable list.

## Fixed — `diff_additive.sh` reported a failed build as an additivity violation

`run_one` recorded `fail build-full` and `fail diff` and the summary counted
both as violations.  With the disk full, an otherwise-clean tree reported
`464 identical, 188 violations` — a full-looking run, every test visited, and
the number entirely an artifact.  It was convincing enough that a correct
change was reverted on the strength of it.

Every bucket is now counted and named: `identical, animated, negative,
violations, build errors`.  Naming them turned up two more instances of the
same thing hiding in the old `skip` count, which any test that failed its
baseline build fell into:

- 15 tests carrying a `.compile_err` sidecar — declared to be rejected — were
  counted as animated.  They now count as `negative`.  The sidecar is checked
  only after both builds fail, not up front: `--emit-c-only` stops before the
  host C compile, so a test whose rejection comes later still emits C and is
  worth diffing.
- One test needed its `.env` sidecar to build at all, which the harness never
  applied, so it failed both configurations and read as animated.  Sidecar
  env is applied now and the test rejoins real coverage.

A test that builds in neither configuration and declares no expected failure
is now a `build error`, which fails the run and says so.

## Fixed — a generic factory shipped in an installed header

`CC_GENERIC_FACTORY` worked in a `.ccs` and in a local `.cch`, but not under
`ccc/` — which is where a library puts one.  Three defects stacked, and each
hid the next:

1. `cc_harvest_local_header_factories` walked `g_lowered_local_headers` only.
   `cc_harvest_header_comptime_functions` already walked
   `g_included_cch_sources` for the same purpose, so the second list was the
   fix.  A local header can appear in both, and harvesting it twice defines its
   monomorphs twice, so the included pass skips what the local pass covered.

2. `cc_find_matching_brace` and its paren/bracket twins had no notion of a
   backtick template, so braces inside one were counted as code.  A factory
   body is mostly `@emit` templates and those contain braces, so the harvested
   block was TRUNCATED at the first brace that happened to balance — 3777 bytes
   of a 5262-byte factory.  The block then failed to compile and the error read
   `factory handler not found in registry`, which points nowhere near the
   cause.  Backtick is not a C token, so the matchers now skip a template as
   one lexical region.  This does NOT also fix the header `@comptime` stripper
   filed separately, though it was claimed to here:
   `cc__strip_comptime_blocks_header` is its own hand-rolled scanner and never
   calls these matchers.  That entry stays open.

3. The fragment validator judged the fragment by errors raised anywhere in its
   validation TU, including the best-effort type prelude prepended to it.  A
   user type naming another type the prelude does not carry reported as a
   fragment defect.  Errors in the prelude report against `<string>`; the
   fragment's own carry `<generic-fragment>` from its `#line`, so only those
   are on trial now.

Pinned by `tests/py_expose_smoke.ccs`, which exposes a CC type as a Python
module through `py_expose::[Counter]`, the factory shipping from `py.cch`.

## Fixed — the compiler caches grew without bound

`/root/.cache/concurrent-c` reached 27G in one session and filled the disk
twice.  Three content-addressed caches — `incexp/`, `comptime-hooks/`, and
`out/ccc-cache/comptime/` — key on input mtimes and toolchain fingerprints, so
every edit mints an entry and orphans the previous one, and nothing ever
reclaimed them.

`cc_cache_evict` trims a directory to a byte budget, oldest first, rate-limited
by a stamp file so a sweep costs one `stat` per compile rather than a
`readdir`.  Budgets are 1024 MB for `incexp/` and 256 MB for the two dylib
caches, `CC_CACHE_MAX_MB` overrides, `make -C cc clean-cache` empties them.
One full test run from cold leaves 437 MB, so the cap is not reached in normal
use — the 27G was many runs accumulating.

A directory that cannot be trimmed warns once per process, since silence there
restores exactly the unbounded growth this exists to prevent.

## Fixed — a comptime fragment spliced inside a generated `#ifndef` vanished

`cc_emit_plan_compute_prelude_insert_pos` skipped preprocessor directives one
line at a time, which walks straight INTO a `#ifndef X` / `#define X 1` guard
and anchors at the first ordinary line inside it.  When the guard is one CC
generates for a Result box, the macro is normally already defined by a header,
so the whole fragment is compiled away.  It is produced, it is spliced, the
emitted C contains it — and the use site reports `implicit declaration of
function 'py_expose_Counter'` with nothing pointing back.

The same loop stopped inside a multi-line `#define ... \` body, splicing a
function definition into a macro definition.

Both are fixed by tracking what the directive actually opened.  The skip is
narrow on purpose: only CC's own `#ifndef CCResult_..._DEFINED` blocks are
unanchor-able.  A first attempt refused to anchor inside ANY conditional and
broke `result_header_multiword_ok_smoke` — a header that declares Result boxes
under guards needs the fragment to land among them, so "inside a conditional"
is not the defect; "inside a conditional that is already satisfied" is.

## Fixed — a pointer ok type unmangled to a token that is not a type

`cc__result_ok_type_for` recovers the ok type by splitting the mangled box name
at the last `_CC`.  A pointer ok type mangles its star away
(`CCPy*` -> `CCPyptr`), so the fallback returned `CCPyptr` and the chain
lowering declared a temporary of that type.  There is a hardcoded table
carrying the demangling for `voidptr`, `charptr` and `CCDirIterptr`; anything
not in it silently produced a non-type, which surfaces as a confusing error in
generated code rather than as the unmangling that did not happen.  The fallback
restores the star.

## Fixed — a generic member chains

`py.expose::[A](..)!>.expose::[B](..)!>` works, at any depth, in statement and
expression position, with non-generic hops mixed in.  Three pieces, each of
which was a silent no-op on its own:

- **Reflection reads a snapshot.**  The TU is copied at the first
  generic-containers entry, and every instantiation reflects on that copy
  instead of the live buffer.  This is what the entry said closing it would
  take: it decouples WHEN an instantiation happens from WHAT its factory
  sees, so resolving a member-generic hop late — after `!>(CCError)` is
  lowered and a fallible method no longer looks fallible — generates the same
  code as resolving it early.  Attribution and the inspect dump still use the
  live buffer, where the use site actually is.

- **The containers pass joins the UFCS/chain fixpoint.**  A chain temp's
  declared type only exists in the rewritten text, so the member
  normalization that needs it must run there, not just in the early chain.
  Safe now, because of the snapshot.

- **The chain-links pass accepts a member-generic hop.**  Its recognition
  scan and its emit re-walk both required `.ident(` — the `::[...]` span
  failed the hop grammar, the `!>` fell through to the statement-unwrap form,
  and that form consumed the producer and shredded the rest of the line.  The
  two scans now share the hop grammar; a re-walk that accepts less than its
  recognizer turns "link found" into a silent no-op, which is how this one
  hid.

Pinned by `tests/py_expose_chain_smoke.ccs`, now one chain of three hops and
two monomorphs.

## Fixed — a contiguous buffer crosses into Python without copying

A typed slice marshalled to a Python `list`: one boxed object per element, then
the callee walked that list and rebuilt a contiguous buffer.  Both sides
already had the same layout, and the crossing destroyed and rebuilt it.

`py_buf(x)` passes a `memoryview` over the CC buffer instead
(`PyMemoryView_FromMemory`, a new `CCPyArg` kind restating the length in
bytes).  Measured by `perf/py_numpy_workload.ccs` at n=1000000: summing through
the list marshal took 71.2 ms, through the borrow 0.52 ms — the same 0.52 ms
numpy takes on an array it already owns, so the copy was the entire
difference.

The view borrows, which is the price of not copying: read-only, and valid only
while the CC buffer is.  The `list` arm is unchanged for callees that want a
sequence.  Pinned by `tests/py_buffer_borrow_smoke.ccs`, which checks both arms
on one slice.

## Fixed — the exported-call trampoline no longer allocates per call

Profiling `r += cc.add(a, b)` against `r += a + b` put the boundary at 2.35x a
native Python add.  Three things were paying for generality the path could not
use:

- `METH_VARARGS` made CPython pack an argument tuple for every call.
  `CC__PyMethodDef` is this header's own struct, bound to the runtime by symbol
  rather than compiled against `Py_LIMITED_API`, so `METH_FASTCALL` is
  reachable: arguments arrive as a pointer into CPython's value stack with a
  count.  The arity check became a comparison and an argument read became an
  array index, removing an allocation and three indirect calls per call.  This
  was the win: 131.7 ns -> 88.0 ns.
- `PySequence_GetItem` dispatched through the type and returned a NEW
  reference the trampoline immediately dropped — a refcount round-trip per
  argument, for a borrow.  `PyTuple_GetItem` borrows directly (~3 ns).
- `cc__py_none()` imported `builtins` and read an attribute on EVERY
  void-returning call, to return an immortal singleton.  Cached.

Net: 2.35x a native Python add down to 1.51x.

That ratio then turned out to be the wrong question, and the benchmark now
says so.  `r += a + b` performs no call at all, so comparing a crossing
against it charges the boundary for CPython's ordinary call overhead too.
Adding a Python-level call of the same shape splits the ladder:

    add_native  ~60 ns   inline, no call
    add_pycall  ~82 ns   +22  what calling any Python function costs
    add_bound   ~86 ns   +4   THE BOUNDARY
    add_to_cc   ~90 ns   +4   the module attribute lookup per iteration

Crossing costs about 4 ns over calling a Python function that does the same
work — roughly the two argument unboxings and the module-state fetch, three
indirect calls into libpython.  That is the floor for a design bound to the
runtime by symbol, and it means the decision is not whether the boundary is
cheap but whether the exposed routine does more work than ~4 ns, which
anything past a few operations does.

Getting there needed a methodology fix worth keeping: CPython 3.13 specializes
bytecode adaptively, so whichever loop ran first paid the unspecialized
iterations and the ladder came out in the wrong order — an inline add
measuring SLOWER than a function call, which cannot be true.  Each mode is
warmed on its own before it is timed.

## Fixed — calling out to Python built a string per call

The outbound direction cost ~360 ns/call against ~100 ns for the same Python
function called from Python.  Benchmarking it at three arities showed the cost
was almost all FIXED — 329 ns with no arguments at all — so it was not the
marshalling.

`PyObject_GetAttrString` was ~244 ns of it.  It builds a temporary Python
string for the method name on every call: allocate, hash, look up, free.  The
name is now interned once per interpreter and kept on the handle, keyed by the
caller's string pointer — call sites pass literals, so the pointer is a stable
identity and the hit path needs no `strcmp`.  The cache lives on the handle
rather than in a global because an interned string belongs to the interpreter
that made it, and it then inherits the handle's threading rule instead of
inventing a new one.

The rest was the argument tuple, allocated and freed per call only to hand
CPython a container it immediately unpacks.  Without keywords the arguments go
to `PyObject_Vectorcall` as a plain array.  Keywords still need the tuple and
dict, so that path is unchanged.

    cc_to_py_0   329 -> 122 ns
    cc_to_py_1   356 -> 157 ns
    cc_to_py     360 -> 235 ns   (3 arguments)

The ownership rule inverts between the two paths — `PyTuple_SetItem` STEALS a
reference where vectorcall leaves the caller holding it — so
`tests/py_call_refcount_smoke.ccs` watches `sys.getrefcount` across 2000 calls
on both paths rather than watching the answers, which stay correct either way.
Removing the vectorcall `DecRef` moves an argument's count from 3 to 2003, and
the test fails.

## Fixed — a Python buffer comes back into CC with one memcpy

The return direction had no counterpart to `py_buf`: `as_list::[T]` walked the
sequence protocol, one boxed object per element, even when the source was a
numpy array whose bytes already had the destination's exact layout.  numpy's
sequence protocol is slower than a list's, so the natural payload was the
WORST case: 55.7 ms to pull 1e6 doubles out of an array, versus 19.9 from a
list.

`as_list` now probes the buffer protocol first (`PyObject_GetBuffer`,
contiguous + format flags).  An exporter whose element format and size match
the destination is one `memcpy` into the arena: 55.7 -> 1.33 ms, and the
result keeps arena provenance like every other slice.  Everything else falls
through to the per-element walk unchanged — a `list` is not an exporter, and
a `float32` array asked for as `double` must convert, not be reinterpreted;
only native byte order is accepted for the same reason.

Copy-once rather than a true borrow, deliberately: a borrowed view would
couple a CC slice's lifetime to a Python object's, the inverse of `py_buf`
where the borrow is scoped by the call.  The memcpy is the same order as the
reduction itself; the 42x was the boxing, not the copy.

Pinned by `tests/py_as_list_buffer_smoke.shcc`, which compares the exporter
path against a list of the same values and includes the wrong-dtype fallback.
The disabled-fast-path run doubles as the mutation check: correctness holds on
either path while the timing collapses, which is exactly the shape the test
should have — it pins equivalence, and the benchmark pins the speed.

