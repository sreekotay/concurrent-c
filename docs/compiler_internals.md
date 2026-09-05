# Compiler internals

How a `.ccs` file becomes a binary, what code does each part of that, where
the compiler carries knowledge that belongs to user space, and where it is
fragile. This is the audit that precedes the cleanup; the cleanup plan is
the last section. Line numbers are as of the audit and drift; names do not.

Companion documents: [`cc/docs/ARCHITECTURE.md`](../cc/docs/ARCHITECTURE.md)
(constraints and ADRs for the native front), [`cc/shadow/README.md`](../cc/shadow/README.md)
(file layout of the lowerer), [`cc/bootstrap/shadow_lower/README.md`](../cc/bootstrap/shadow_lower/README.md)
(seeds), [`build-when.md`](build-when.md) (which command after which edit).
`cc/docs/COMPILER_CLEANUP_STATUS.md` and `cc/docs/PASS_CLEANUP_PLAN.md`
describe the removed multipass driver and are archaeology.

---

## 1. The pipeline at a glance

```text
cc/bin/ccc (sh)          stale-header check → lower_headers → exec .ccc-bin
   │
cc/src/cc_main.c (C)     paths, mode, build.cc, cache keys, runtime .o,
   │                     flag folding → fork/exec shadow_lower
   │
out/cc/bin/shadow_lower  (Concurrent-C, bootstrapped from a committed seed)
   │
   ├─ comptime prepare   cc/src/preprocess/comptime_prepare.c   raw-text rewrites
   ├─ stage 1  tape      cc/shadow/pp_tape.cch                  pp-tokens + line index
   ├─ stage 2  stitch    cc/shadow/pp_stage2.cch                #include / #define / guards
   ├─ type-sugar pass    shadow_rewrite_restricted_type_sugar   token array, in place
   ├─ parse              cc/shadow/pp_ast_parse_*.cch           whitelist AST (AstNode)
   ├─ safety             cc/shadow/pp_ast_safety.cch            move / channel / unwrap / scratch
   ├─ emit               cc/shadow/pp_emit_*.cch                one walk → emit.c (+ #line)
   ├─ comptime splice    cc/shadow/shadow_comptime.c + libshadow_comptime.a
   ├─ product scan       shadow_product_host_c_ok               leftover CC tokens → error
   ├─ host cc -c         cc/shadow/shadow_build.cch             emit.c → tu.o (cached)
   └─ host cc link       tu.o + runtime.o + @link libs → binary
```

Two things the picture in `ARCHITECTURE.md` does not show:

- **A text-rewrite stage runs before the parser.** `shadow_comptime_exec_file`
  calls `cc_comptime_prepare_source` (`cc/src/preprocess/comptime_prepare.c:20`,
  from `cc/shadow/shadow_comptime.c:703`) on the raw bytes, and that runs
  ten byte-offset scanners: template dedent, `@grammar` splice, module export
  directives, `Type.fn(...)` scoping, `CC_GENERIC_FACTORY`, `static_map` calls,
  `@comptime if`, value-position `@comptime(expr)`, template receiver chains,
  and string templates. Positions the parser later reports are positions in
  the rewritten buffer.
- **The host compile runs inside `shadow_lower`, not in the driver.** So the
  driver's post-processing of the emitted C (`cc_emit_polish_c`, `--no-line`)
  happens after the host compiler has already spoken.

Header lowering (`.cch` → `out/include/**/*.h`) is a separate tool,
`out/cc/bin/lower_headers`, run by the shell wrapper when headers are stale
and by `make -C cc lower-headers`. It is a fifteen-pass text pipeline, not
the native lowerer (section 6).

---

## 2. Translation units and artifacts

One program produces:

| Artifact | Where | Built by |
|---|---|---|
| User TU | `out/.cc-build/native/<emit_key>/emit.c` → `tu.o` | `shadow_build_host` (`cc/shadow/shadow_build.cch:1276`) |
| Runtime unity TU | `cc/runtime/concurrent_c.c` includes ~35 sibling `.c` files (`:27-64`); rewritten to `out/runtime/concurrent_c.c`; compiled per flag variant into `out/.cc-build/host/<host-fp>/runtime-<variant>.o` with a `.recipe` | `cc__ensure_runtime_obj` (`cc/src/cc_main.c:5333-5342`) |
| Lowered headers | `out/include/**/*.h`, stamp `out/include/.headers_lowered.stamp` | `lower_headers` |
| Comptime hook dylibs | `$HOME/.cache/concurrent-c/comptime-hooks/<fnv>.dylib` (or `$TMPDIR`, `/tmp`), 256 MB budget | `cc__build_compile_and_load` (`cc/src/comptime/hook_compile.c:847`) |
| Lowerer seed | `cc/bootstrap/shadow_lower/<VERSION>-N/` with pre-lowered `shadow_lower.c` and flattened headers; pointer `last-good` | `scripts/iterate_shadow_lower.sh --ship` |

**Body-only-in-the-runtime.** The decision that an arena function has an
out-of-line body only in the runtime TU is made in the headers, not the
driver: `cc/runtime/concurrent_c.c:15` defines `CC_ARENA_IMPL`, and
`cc/include/ccc/cc_arena.cch:42-48` chooses `CC__ARENA_SYS` (external
definition) versus `static inline`. `cc/runtime/arena_state.c:1` sets the
same macro so tool binaries get the bodies, which is why `arena_state.o` is
filtered out of `libshadow_comptime.a` (`cc/Makefile:212-217`) to avoid an
ODR clash with `concurrent_c.o`.

**Cache keys.** Six caches, every one keyed by content: the bytes of the
inputs and the bytes of the toolchain, never an mtime. One helper,
`cc_toolchain_content_fp()` in `preprocess.c`, folds the running binary,
`.ccc-bin`, `shadow_lower` and `lower_headers` (found from the repository
root) and the toolchain version, memoized per process; every key below
folds it. Same-second, same-size edits are the test
(`tests/cache_key_same_second_smoke.ccs`).

| Cache | Key |
|---|---|
| Lowered local headers (`out/include/**/*.h`, `$TMPDIR/cc-lowered-<uid>/`) | `<h>.key` sidecar: header bytes, owner `.ccs` bytes, every quoted `.cch` it includes transitively, toolchain |
| Include expansion (`~/.cache/concurrent-c/incexp`) | input bytes, host cc, `.ccc-bin`, repo root, toolchain; deps sidecar holds a content hash per expanded file |
| Driver TU emit (`out/.cc-build/<build>__<target>__<unit>.meta`) | source bytes, `cc_depends`, transitive `.cch` bytes, `.ccc-bin` and `shadow_lower` bytes, version, flags, env, consts |
| Driver object (`.obj` meta) | emit key (raw C: source bytes), target, flags, env, host fingerprint, plus the `.d` check |
| `shadow_lower` emit / object / link (`out/.cc-build/native/<key>/`) | emit: source bytes + toolchain, deps validated by content (`emit.deps.key`); object: emit key + host cc bytes (resolved through PATH) + flags + the emitted C's bytes; link (`bin.key`, `bin.sum`): object bytes, runtime object bytes, link flags, and the binary's own bytes |
| Comptime hook dylibs (`~/.cache/concurrent-c/comptime-hooks`) | TU bytes, argv, host cc, toolchain |

`--no-cache` still writes `emit.c` and `tu.o`; it only suppresses the keys.
A dependency that cannot be read folds a sentinel with its path, so a file
that appears later changes the key.

**The seed bootstrap.** `shadow_lower` is written in Concurrent-C, so a cold
clone builds it from the committed seed: the Makefile sed-patches the seed
for API drift (`cc/Makefile:461-478`, rules like
`cc_arena_reset(&` → `cc_arena_reset(`) and host-compiles it. A rule that
stops matching fails as a compile error deep in a multi-megabyte generated
`.c`. `out/runtime/.seed.stamp` (`cc/Makefile:280-292`) exists only to break
the cycle on a cold tree by seeding a crude host-C `out/include` first.

---

## 3. The driver (`cc/src/cc_main.c`, 8.5k lines)

Responsibilities, in order of execution: layout resolution
(`cc_init_paths`, `:570`: `$CC_HOME`, then a prefix install, then a dev
checkout found by string-cutting `/cc/bin/ccc` out of argv[0], then walking
up from cwd, then cwd), mode dispatch, `build.cc` parsing
(`cc/src/build/build.c`), unit kind and version-pin resolution, `.shcc` and
`#!ccc` rewriting into content-keyed copies under `out/.cc-build/`, runtime
object management, flag folding, and the exec:

```text
shadow_lower [--no-cache] [--verbose] [--release] [--debug] [--no-runtime]
             [--cc-flags=…] [--ld-flags=…] <in> -o <out>
env: CCC_VERSION, SHADOW_QUOTE_DIR, SHADOW_RUNTIME_O
```

(`cc_main.c:4389-4404`). Lowering stderr is captured to `<out>.diag` and
replayed on warm cache hits. The host compile line, built inside
`shadow_lower`, is:

```text
<CC> -std=c11 -D_DEFAULT_SOURCE [-I<CC_INCLUDE_PATH>] -I<srcdir> -I out/include
     -I cc/include -I cc -I. -I examples -ffunction-sections -fdata-sections
     -Werror=implicit-function-declaration $CFLAGS <cc_flags> [-DNDEBUG] [-g]
     -c emit.c -o tu.o
<CC> tu.o <runtime.o> -o <bin> -lpthread -lm [-Wl,--gc-sections] <@link libs>
```

The host profile (`cc/src/build/host_cc_profile.c`) probes the compiler once
per fingerprint with three snippets and records whether `-std=c11`, `-B`,
`-Uarm` or `-DCC_NO_LIBLFDS` are needed. The parse-session C-version drift
noted in `CLAUDE.md` is fixed by the single constant `CC_HOST_C_STD_OPTION`
(`host_cc_profile.h:24`) applied at every session, with one residual copy
of the literal in `cc/shadow/shadow_tcc_compile.c:108`.

What the driver hard-codes (section 8 has the full catalogue): the runtime
source list used for staleness (fifteen names plus `vendor/zmij.c`,
`cc_main.c:5203-5218`; a new runtime file never marks the object stale),
`-DCC_ENABLE_ASYNC` always on, `-lpthread -lm`, the `shadow_lower` search
order, `third_party/tcc` paths, and forty-odd environment variables.

Fragility in the driver: the `shadow_lower` argv is `char *argv[28]`;
flag buffers are 2048 to 4096 bytes built with `strncat` and no overflow
check (`cc__compile_c_to_obj`, `:5014-5061`, can drop the output path);
`system()` results are compared to zero so a missing shell is a compiler
error; `@link` scanning runs `cc -E … 2>/dev/null` through `popen` with the
status unchecked and `cc` hard-coded (`:3079-3106`).

---

## 4. The C-side text engine (`cc/src/preprocess`, `cc/src/visitor`)

`ARCHITECTURE.md` says the multipass text-rewrite front is removed. That is
true of the driver (`--frontend=legacy` is a hard error, `cc_main.c:3733`)
and misleading about the code: `cc/Makefile:115-122` re-adds seven visitor
files by name, every `preprocess/*.c` except `grammar_stub.c` is in
`LOWER_HEADERS_SRCS` (`:172-210`), and the same objects are archived into
`libshadow_comptime.a` (`:217`), which `shadow_lower` links. The native
lowerer carries the entire 87k-line text engine as a static library and
enters it on three seams.

### 4.1 Live orchestrators

- **`cc_comptime_prepare_source`** (`comptime_prepare.c:20`), the pre-parse
  rewrite described in section 1, on every unit.
- **`cc_lower_header_string`** (`cc/src/header/lower_header.c:785`), the
  `.cch` → `.h` pipeline: dedent → strip `@comptime` → strip generic
  factories → `.cch` includes to `.h` → `@as` to comment → strip `@typeview`,
  `@typehooks`, `@destroy`/`@detach` → shared type-syntax lowering (itself a
  six-pass sub-chain, `preprocess.c:20004-20015`) → `@variant` → splice
  header Vec decls → `T!>(E)` to `CCResult_T_E` → inferred Result ctors →
  Result field sugar → `!>`/`?>` unwrap → `@err` syntax → per-use splice of
  Result declarations.
- **`cc__apply_phase1_canonical_passes` / `phase3`** (`preprocess.c:24730`,
  `:24822`), the full legacy chain of about forty passes, with exactly one
  live caller: `cc_preprocess_to_string_ex` from `hook_compile.c:940`, and
  only when a comptime batch contains a UFCS type hook. `CC_DEBUG_CANON`
  prints each pass name; that is the closest thing to a manifest.

### 4.2 Dead code still compiled

`cc_preprocess_file`, `cc_preprocess_to_string`, `cc_preprocess_canonicalize`,
`cc_preprocess_emit_splice` and the reparse-coordinate accounting
(`preprocess.c:11963-12170`) have no callers. Two of the three entry points
of `pass_channel_syntax.c` (2270 lines) are uncalled; `preprocess.c:2900`
carries its own copy of the third. `visitor/ufcs.h` declares four
`cc_ufcs_rewrite_*` functions that are defined nowhere. `pass_type_syntax.c`'s
slice and Result rewrites are shadowed by copies in `preprocess.c`.

### 4.3 How this engine reports errors

There is no diagnostic sink. `cc/src/diag/diag.h` exists and is never used
here; every message is `fprintf(stderr, …)`, through `cc_pp_error_cat`
(`preprocess.c:111`, 71 sites) or `cc_pass_error*` (`pass_common.h:250-291`),
plus 87 position-free `fprintf`s in `preprocess.c` alone. Positions are
line and column in the buffer the pass was handed, counted from byte zero
and ignoring `#line`. By phase 3 that buffer has been through about
twenty-five line-changing rewrites. Columns from this engine are
structurally always 1: the scanner advances `col` only inside
`cc_scanner_skip_non_code_ex` and undoes the increment when it reaches
code (`preprocess.c:313`, also `:290`, `:307`).

### 4.4 Fragility specific to this engine

- `cc_pass_chain_apply` tracks only the first 32 allocations of a chain but
  installs every result (`preprocess.c:374-383`); phase 1 plus phase 3 on
  one chain issue about forty. Nothing is reported.
- Every rewriter returns `NULL` for "nothing to do", a buffer for success,
  and `(char*)-1` for a diagnosed error, and many failure paths return
  `NULL` (`preprocess.c:10084`, `:11919-11922`, `grammar_seam.c:228`). A
  mid-construct bail-out passes the CC surface through, and the failure
  appears later as a host C error on generated text.
- Hand-rolled string/comment skippers: `grammar_seam.c:231-245` does not
  know about backtick templates; `preprocess.c:11745-11762` is another
  copy. `make lint-scanners` only prevents growth.
- Truncation: 250 `snprintf` calls in `preprocess.c`, about 97 checked.
  `preprocess.c:9080-9082` builds `Map_%s_%s` into `char mangled[128]`
  unchecked, so two long type parameters truncate into a different,
  valid-looking name that then fails to match its own declaration.
- Process globals with documented reentrancy hazards:
  `g_cc_pp_splice_last_anchor` (`preprocess.c:12136-12138`),
  `g_rewrite_root_path`, `g_ufcs_header_path`, `g_script_prelude_off`,
  `g_va_hits_s`, `cc__schema_reg`.
- Any user file whose path is under `/tmp` skips the async-channel-await and
  block-on validation checks (`preprocess.c:11946`, `:12028`), because those
  paths are how the engine recognises its own temp files.

---

## 5. Inside `shadow_lower` (`cc/shadow`, 65k lines)

### 5.1 Tape and stitch

Stage 1 lexes bytes into a `FileTape` of pp-tokens with comment spans and a
per-line index that understands both `#line N "path"` (`pp_tape.cch:189`)
and the comment-encoded `/*CC_LN N "path" */` marker (`:220`) used where a
real `#line` would be illegal C. Stage 2 splices `#include`, object-like
`#define` and simple guards; directive policy is an exhaustive comptime
`static_map` (`pp_stage2.cch:44-66`) whose fallthrough is a hard error.
Before parsing, `shadow_rewrite_restricted_type_sugar` mutates the token
array in place (`shadow_lower.ccs:607`).

### 5.2 The AST

`AstNode` (`pp_ast_core.cch:326-395`) is a fixed-shape record with eight
positional, untyped text slots `a` through `h`, whose meaning is documented
per kind in the enum comments (`AST_CHAN_VAR` uses `a=name b=capacity
c=direction d=elem e=ordered`; `AST_PARALLEL_FOR` uses all eight). There are
68 kinds. Children attach in three roles that must not be mixed: `kids`
(open parent lists on a bump table capped at `SHADOW_KIDS_CAP = 131072`),
`body` (statement lists) and `dbody` (destroy bodies and attachments).
Trivia is sticky: `file_id`, lead comment span, `tok_off`, and a
`char indent[64]`.

Anything the parser does not model becomes `AST_RAW_LINE`: with `e == "tape"`
the slots hold byte offsets and emit copies the span verbatim
(`pp_emit_stmt.cch:10022-10044`). Local enums, labels, file-scope macro
invocations, `#pragma(@parallel)` and switch case labels all take this
path, as does any statement `parse_stmt` rewinds from (`pp_ast_parse_stmt.cch:4276-4281`).

### 5.3 Emit is a walk for statements and a text pipeline for expressions

`shadow_emit_stmt_ctx` (`pp_emit_stmt.cch:6913`) is a ~3100-line `switch`
over node kinds; `AST_IF` is a real recursive walk with bind-state merging
across arms. Inside an expression, though, lowering is a fixed sequence of
in-place character scanners over a `char cur[8192]`:
`shadow_emit_expr_text` (`pp_emit_ufcs.cch:5710-5875`) runs print-and-string
→ string-as-slice → `@slice` → exact UFCS → UFCS peel loop → closure calls →
leftover `?>` → variant → Result ctors → generic types → `type_of` →
nursery host calls → array `len` fields → `as:` field reads. Each is a
`strncmp`/`strstr` pass; `shadow_rewrite_result_ctors`
(`pp_emit_core.cch:157-232`) balances parentheses by hand to split
`cc_err(a, b)`. `AST_SWITCH` shows the seam: a structured path when case
bodies parsed, otherwise the body is copied into a buffer and the text
passes run over it.

ADR-S3 forbids a general post-parse mangler. The expression pipeline is
one, for exactly the grammar the whitelist does not model.

### 5.4 UFCS resolution (`pp_emit_ufcs.cch`, 5953 lines)

`shadow_ufcs_lower_parts` (`:1796-3508`, 1713 lines, 58 local buffers)
resolves `x.foo(a)` in this order: strip casts and find the receiver's bind
and base type (with a global hint `g_shadow_ufcs_hint_vty` and a literal
special case for `cc_nursery_arena(`); `@typeview`; member-generic
factories; `.map` dyn-sink; nursery `spawn`/`send_task`/`leave`; **the
`.ufcs` hooks from `@typehooks`**, verified against a declaration before use
(`:2231-2256`), which is the intended extension point; closure-field sugar;
built-in tables for arena, IO and len/cap; a 107-arm `strcmp(meth, …)`
ladder; and finally compose-then-verify invention of `Type_meth` or the
`cc_<snake>_<meth>` twin, refused when no declaration exists. Five separate
copies of the snake-case generator exist (`:1000`, `:1164`, `:1481`, `:3823`,
plus the `_len` variant).

### 5.5 Global state

125 distinct `g_shadow_*` globals, 88 of them in `pp_emit_core.cch`, are the
real interface between phases: the current site for diagnostics
(`g_shadow_ufcs_site`, `g_shadow_expr_site`), the sink destination type, the
receiver hint, the scratch-wrap marker, and six sticky failure flags checked
only at the end of `shadow_emit` (`pp_emit_tu.cch:3695-3706`). Reset is
partial (`:1311-1329`); the process is one-TU by construction.

### 5.6 Fixed buffers

1735 `char name[N]` declarations across the lowerer, 41 distinct sizes.
The busiest functions: `shadow_emit_stmt_ctx` 140, `shadow_emit` 71,
`shadow_ufcs_lower_parts` 58, `parse_static_fn` 33. Outliers:
`char out[65536]` (`pp_emit_ufcs.cch:4990`), `char recv[4608]`,
`static char csv[1100]` returned by pointer (`pp_emit_core.cch:5807`).

The archetype of the failure mode is `shadow_rewrite_print_and_string`
(`pp_emit_stmt.cch:1483-1561`): it builds into `char out[8192]`, `break`s on
any overflow, and its tail is

```c
if (strlen(out) + 1 <= cap) snprintf(expr, cap, "%s", out);
```

so when the rewritten text does not fit, the caller's buffer is left
untouched with `println(` and `@string(` still in it. `@string(` is caught
by the post-emit product scan; a bare `println(` reaches the host compiler
as an implicit declaration. About thirty bail-outs of this shape exist
(`pp_emit_core.cch:172` … `:9828`, `pp_emit_stmt.cch:1337` … `:7297`);
`pp_emit_core.cch:196` says so in its comment: "ambiguous — leave
incomplete; host". Opaque static-function bodies go through a 2048-byte
staging buffer (`pp_emit_tu.cch:3074`, `pp_emit_ufcs.cch:5941`) and are
truncated mid-token past that.

### 5.7 Caps that stop quietly

The UFCS peel handles at most 128 sites per text (`pp_emit_ufcs.cch:5376`);
site 129 is left as written. `guard++ < 8` loops bound several rewriters
(`pp_emit_core.cch:511, 4072, 6098, 6177, 7290, 7812`), `< 16` for `@defer`,
`< 32` for autoblock, and `@as` retries at 8. None reports. Statement
recursion has no depth counter and each frame carries about 140 fixed
buffers. Table overflows, by contrast, are loud (`shadow_table_full`,
`pp_ast_core.cch:19`, "refusing silent drop").

### 5.8 Failure that looks like success

`shadow_ufcs_peel_left` returning `-1` does not fail the emit; the loop
skips past the site (`pp_emit_ufcs.cch:5378-5386`) and `x.foo(1)` stays
verbatim. The backstop only fires when it can bind the receiver's type
(`:3740`). For an unbound receiver `x.foo(1)` is valid C, the product scan
passes it, and the user gets the host compiler's "has no member named
'foo'". The same applies to any CC sugar inside an opaque span that
happens not to contain a banned `@token`, `!>`, `[:]` or `::`.

### 5.9 Shapes an AST needs that the engine refuses

Writing a tree of `@variant` nodes with `Vec` children and a template
printer (`stress/break/break_ast_cc_way_smoke.ccs`) trips thirteen distinct
defects, each pinned by a sibling `break_*` test with an `.xfail` marker:

| Shape | What happens today | Test |
|-------|--------------------|------|
| `Vec::[T*]` | no instance is emitted for a pointer element; with a typedef, `@for` types the element as an opaque value and refuses `p->v` | `break_vec_of_pointers` |
| `Vec::[Ref]` field in a struct declared before `Ref`'s pointee | the instance is spliced after the pointee's definition, after the struct that needs it | `break_vec_field_before_elem_def` |
| `n->call.args.push(x)` | receiver type is taken from the first identifier of the path (`Node`) | `break_ufcs_through_arm_field` |
| `@for (x in c->xs)` with `c` a `case .arm(c)` binding | the `@for` is left in the C (`stray '@'`) | `break_for_in_case_binding` |
| a pointer local named `xs` in one function | `c.xs.push` in another function is typed as a pointer: local types are keyed by bare name per file | `break_local_scope_leak` |
| `@string(\`${n}(\`, a)` | the argument scanner counts parentheses inside the backtick text | `break_template_lone_paren` |
| `return cc_ok(@string(\`(${l} ${r})\`, a))` | the emitted expression is clipped at about 330 characters | `break_template_in_return_ok` |
| `"@errhandler"` inside a `@switch` case body | the switch lowering re-scans its bodies for `@`-tokens and splits the string literal | `break_at_word_in_switch_literal` |
| `uint32_t lo = 0, hi = v.len();` | only the first declarator's initializer is lowered | `break_ufcs_second_declarator` |
| `const T* p = xs.get_ptr(xs.len() - 1);` | the nested `.len()` stays un-lowered when the destination is `const T*` | `break_nested_ufcs_const_dest` |
| `n->k = { .shared = a }` with `shared` an arm of two variants | the braced assignment is left un-lowered | `break_variant_shared_arm_assign` |
| `Vec::[int] xs` declared inside a `@switch` case body | the generic spelling is not lowered | `break_generic_in_switch_body` |
| `static bool f(Vec::[int]* xs);` prototype | the generic parameter in a prototype is never lowered | `break_generic_ptr_param_proto` |

Every one is a text-engine property (name-keyed type tracking, splice
points chosen by textual position, paren-counting through literals, fixed
expression buffers); none is a language rule. The AST probe passes with a
local binding or an index loop in place of each shape.

Two more came from moving the same types into a quoted `.cch`, and both
are fixed in place in `pp_emit_typehooks.cch` because the clean lowerer's
own faces need them (`tests/quote_cch_one_line_struct_smoke.ccs`): the
header field scan read one field per source line, so a struct written on
one line registered no fields and `c.items.push(x)` had no receiver type;
and a `Map::[K,V]` field in a header struct never registered the instance
the TU must emit, so the host compiler saw an unknown `Map_K_V`.

---

## 6. Comptime, type hooks, generics, header lowering

**`@comptime` blocks** are recognised by one function
(`cc_match_comptime_block`, `cc/src/util/text.h`) and executed in-process
with libtcc from `cc_emit_plan_exec_comptime_blocks` (`emit_plan.c:2214`).
Whether a block runs at all is a textual allow-list
(`cc__block_needs_executor`, `emit_plan.c:2061-2083`): the block must call a
registered `@comptime` fn or mention `cc_emit_tpl_`, `cc_emit_error`,
`cc_emit_warning`, `cc_emit_raw`, `cc_canonical_name`, or one of
`type_of`/`for`/`while`/`do`/`switch`. A block matching none is skipped,
indistinguishably from a block with nothing to do. Errors from this path
are good: libtcc diagnostics are captured and wrapped with the block's
`#line`-resolved origin.

**Type hooks.** `@typehooks on T { … }` is rewritten to a `cc_type_register`
call and parsed textually (`symbols.c:1032`); the recognised fields are a
fixed list (`create`, `destroy`, `pre_destroy`, `cast`, `len`, `access`,
`ufcs`, `ufcs_sink`, `ufcs_dynamic`, `ufcs_dynamic2`, `niche`). Only `.ufcs`
compiles code, through a hook dylib (`hook_compile.c:847`): a "slim" TU of
`prelude.cch` plus `cc_ufcs.cch` plus the handler, keyed by content, argv
and a toolchain fingerprint that includes the lowering binary itself. Two
backends exist (in-process libtcc for isolated factories, host compiler
plus `dlopen` otherwise); on libtcc failure the error is erased and the
host compiler is tried (`hook_compile.c:1083`), so a real error in a factory
body is reported, if at all, as the second compiler's message. `$CC=tcc` is
silently replaced by `cc` (`:541`). A second, unreferenced cache
implementation lives in `dylib_cache.c` with a different root and opt-out.

**Generics.** `Vec::[T]` and `Map::[K,V]` are detected and mangled in
`preprocess.c` (`cc_ct_canonical_name`, `:23554`; recipe in
`cc/docs/GENERIC_MANGLING.md`) and instantiated through the factory registry
in `emit_plan.c` (`CC_GENERIC_FACTORY`, `:597`). Diagnostics here are
specific and name the missing include.

**Header lowering** is section 4.1's second orchestrator. Its remaining
silent-degradation paths, in the sense `CLAUDE.md` uses:

1. `cc__strip_generic_factory_blocks_header` (`lower_header.c:641-644`)
   returns `NULL` on an unbalanced brace, which the caller reads as "no
   factory blocks"; every factory body leaks into the `.h`. No diagnostic.
2. `cc__strip_comptime_blocks_header` now diagnoses the unbalanced block
   (`:730-742`) but still returns `NULL`, and `cc_lower_header` (`:1106`)
   propagates only the write error, so the tool prints the message and
   exits 0 with the `@comptime` blocks intact.
3. `cc_lower_header` (`:1136`) writes the raw `.cch` bytes as the `.h`
   whenever lowering produced nothing, conflating "nothing to rewrite"
   with "every pass bailed".
4. At most 64 Vec instances per header; the rest are dropped
   (`lower_header.c:289-291`).

The outside-repo-root lowering no-op from `CLAUDE.md` is fixed
(`preprocess.c:15154-15180`, with a temp-directory fallback and a failure
flag checked by three callers).

---

## 7. Diagnostics

### 7.1 Three sinks and a dead fourth

| Stage | Sink | Format |
|---|---|---|
| Text engine | `cc_pp_error_cat`, `cc_pass_error*`, raw `fprintf` | `<repo-relative>:L:1: error: …` (column always 1) |
| Lowerer | `diag_emit` / `diag_at` / `diag_err_loc` / `diag_product` (`pp_tape.cch:400-508`) | `<path as given>:L:C: error: …`; `path:L:` when no column; bare `error:` when no line |
| Host compiler | captured and rewritten by `shadow_host_diag_replay` (`shadow_build.cch:650`) | `<basename>:L:C: error: …` with the snippet re-read from the original file |

`cc/src/diag/diag.c` (`CCDiag`, `CCSourceSpan`, `cc_diag_translate_tcc_error`,
`CCSourceMap`) is a designed diagnostic system whose only caller is
`cc_diag_init` at `cc_main.c:7690`. `cc/src/diag/mangle.c` exists for
demangling and is unused.

### 7.2 Position machinery

`shadow_emit_line` (`pp_emit_core.cch:9688`) writes `#line N "path"` before
a statement only when the line changed. The path comes from the tape's
logical position (honouring `#line` and `CC_LN`) and is then reduced by
`shadow_fmt_site_path` (`:8869`) to a `tests/…` suffix if one is present,
otherwise the basename. Multi-line synthetic expansions must call
`shadow_resync_line` (re-emit once) or `shadow_pin_line` /
`shadow_emit_pinned_block` (re-pin every physical line); where an emitter
forgets, drift is immediate and unbounded.

The driver's `--no-line` is consumed by the driver (`cc_main.c:875-876`) and
never forwarded to `shadow_lower`, whose own `--no-line` flag sets
`g_shadow_no_line`. Since the host compile happens inside `shadow_lower`,
the driver flag has no effect on diagnostics.

`shadow_host_diag_replay` remaps `out/include/REL.h:L:C` to `REL.cch:L`,
rewrites `unknown type name 'CCVec_int'` to `Vec::[int]`, replaces the
compiler's snippet with the original line, and redraws the caret at the
column of the first quoted identifier found in the original text. The
numeric `file:line:col` header is left as the compiler wrote it, so the
caret and the printed column routinely disagree.

### 7.3 Ten probes

Run as `cc/bin/ccc run <file> --no-cache`; sources in the audit scratch
directory, worth turning into `stress/break` oracles.

| Case | Line | Column | Names the user's construct | Note |
|---|---|---|---|---|
| Missing `;` after CC syntax | yes | yes | yes | plain C, works |
| `int n = s;` with `CCString s` | yes | yes | yes | works |
| `s.nosuch()` on `CCString` | yes | the dot | no, "has no member named" | C vocabulary for a UFCS miss |
| Unclosed `${` in `@string` | yes | always 1 | no | says "requires a trailing arena argument"; the arena is present; hides every later error |
| `!>` on a non-Result | 2 of 3 past EOF | no | no, `__r`, `.ok`, `.u` | the scaffold is not pinned |
| `println` under a `CCError` handler | yes | yes | yes | the best message in the system; absolute path spelling; hides host errors |
| Error inside a `@parallel` spawn body | yes | yes | yes | works |
| Missing prelude include | yes | emit.c column | no, `cc_string_len` for `s.len()` | leaks lowered names and suggests `cc_slice_len` |
| Undeclared variable in a UFCS chain | yes | yes | yes | works because the text was preserved verbatim |
| Error after a four-line `@string` | yes | yes | yes | no drift |

Stage masking: a text-engine fault plus two C errors prints only the
text-engine line; a lowerer error plus a C error prints only the lowerer
error; the parser is sticky and keeps the first diagnostic
(`pp_ast_core.cch:466-471`).

### 7.4 Where positions are lost

1. Text-engine columns are always 1 (section 4.3).
2. The `!>` scaffold emits `#line 8` once, then five generated lines, then
   one `shadow_resync_line` (`pp_emit_unwrap.cch:33`); cpp counts on from
   there, so a ten-line file reports lines 11 and 12.
3. Columns are emit-buffer columns and are never remapped; they are right
   only when the statement text survived verbatim.
4. `#line` paths are basenames, so from any other directory the host
   compiler cannot open the file and prints no snippet or caret, and the
   replay's original-line substitution never fires. The `tests/` special
   case is a golden-file workaround.
5. Three spellings of one path across the three sinks.
6. `shadow_emit_err_loc` positions a lowerer diagnostic by scanning up to
   1024 bytes forward from the node for a needle string
   (`pp_emit_core.cch:4560-4583`); an earlier occurrence inside a string or
   comment wins.
7. `diag_at` takes the column from the physical tape and the line from the
   `#line`-adjusted logical position (`pp_tape.cch:434-445`).

### 7.5 What the tests pin

`.compile_err` is substring-per-line with no ordering or count
(`tools/cc_test.c:375-403`). Of 322 such goldens, 70 pin a `file:line` and
21 pin a column. The ten `diag_oracle_*` tests cover line fidelity and say
in their comments that columns are omitted so clang and TCC both match.
Column fidelity is not tested, which is why the cases above can be this
wrong without CI noticing. Three `.expect_error` sidecars exist and nothing
reads them.

---

## 8. Hard-coded knowledge that belongs in user space

Scale first. String-literal comparisons against identifier names:
`preprocess.c` 451, `cc_main.c` 159, `emit_plan.c` 76, `type_registry.c` 60;
`pp_emit_ufcs.cch` 382, `pp_emit_stmt.cch` 266, `pp_ast_safety.cch` 230,
`pp_emit_core.cch` 228, `pp_emit_tu.cch` 180. Runtime symbol names
(`cc_*`, `__cc_*`, `CC*`) emitted or matched verbatim: `preprocess.c` 279,
`result_spec.c` 87, `pp_emit_ufcs.cch` 227, `pp_emit_core.cch` 135.

### 8.1 Builtin function names

- **The print family**, in three places that do not share a table:
  `preprocess.c:7331` (six names, rewritten to `cc_<name>`),
  `pp_ast_core.cch:1518-1541` (`shadow_is_optional_print_fn`, the same six
  plus `cc_` twins plus a suffix rule), and the tokenizer, which reserves
  `println`/`eprintln` as keywords (`pp_ast_core.cch:107, 134`).
  `println(` → `cc_println` by `strncmp` in `pp_emit_stmt.cch:1498, 7626`,
  `pp_emit_tu.cch:3025`, `pp_emit_autoblock.cch:137`, `pp_ast_safety.cch:3660`,
  `pp_emit_core.cch:1566`. Home: an attribute on the `cc_print*`
  declarations in `stdio.cch`.
- **`@scratch` and `__cc_str_scratch`**: the C spelling is a literal in
  about ten lowerer sites (`pp_emit_stmt.cch:1541, 2648, 3581, 4533, 7236,
  8729`, `pp_emit_spawn.cch:785` with the magic `1024`, `pp_ast_safety.cch:1781,
  2007`) and in `preprocess.c:1643, 2009, 2061, 2091-2094`. The token test
  `shadow_arena_is_scratch` is duplicated. Home: one definition of the
  scratch arena's name and size, consulted by both.
- **Arena allocation call shapes**: `pp_ast_safety.cch:629-632` keys
  provenance on `cc_arena_alloc_slice_bytes(`, `cc_arena_alloc_T_count(`,
  `cc_arena_alloc_T(`, `cc_arena_alloc(` with the arena's argument position
  per key; epoch tracking hard-codes `cc_arena_reset`/`free`/`destroy`
  (`:2121-2126, 3377-3381`); `preprocess.c:811-812` and
  `pass_unwrap_destroy.c:855-856` carry `cc_arena_alloc_T` with hand-written
  lengths 16 and 22. Home: an `as:` style attribute on the declarations.
- **Destroy-callee inference**, duplicated: `preprocess.c:493-512` and
  `pass_unwrap_destroy.c:640-675` each pick `cc_arena_destroy`,
  `cc_arena_checkpoint_destroy`, `cc_slice_destroy`, `cc_nursery_destroy`,
  `cc_channel_free` from a declared type. The lowerer's own version is
  `g_shadow_ufcs_life` (`pp_emit_ufcs.cch:1134-1143`). Home: `@typehooks`
  `destroy`, which exists.
- **Map key hash and equality** (`preprocess.c:14530-14540`): a seven-row
  table with substring rules, so a key type merely containing `64` gets
  `cc_map_hash_u64`. The declared-symbol probe (`cc_map_key_hash_<mangled>`,
  `:14548-14556`) already exists; the table is the fallback that never went
  away.
- **Family call composition** (`preprocess.c:6855-6880`) prefixes
  `__cc_map_generic_`, `cc_command_`, `cc_file_`, `cc_arena_`, `cc_string_`,
  `cc_slice_`; `:6870` silently renames `append` to `push` for strings.
- **No-return functions** (`pass_result_unwrap.c:1402-1407`): ten names, of
  which only `cc_error_exit` is stdlib. Home: `_Noreturn` on the declaration.
- **Container constructors** `cc_vec_new`, `cc_vec_from`, `cc_map_new` and
  the `Vec::[` → `CCVec` alias table (`preprocess.c:7405-7420, 8710, 8954`).
- **Python extraction verbs** `as_list`/`as_map` (`preprocess.c:9259`).
- **Grammar engines**: `rules`/`schema` compiled in, `cli` deliberately in
  `<ccc/std/cli.cch>` (`grammar_rules.c:5182`), and three JSON grammar names
  mapped to `.rules` paths (`:660-662`). The `cli` move is the template.
- **spawn family**: `spawn`, `spawnhybrid`, `send_task`, `send_task_hybrid`,
  `spawn_unsafe` by `strcmp` across five files; `cc_nursery_create`,
  `create_child`, `spawn_child_closure0` (`pp_emit_spawn.cch:149-151`).
- **The capture vocabulary** (`pp_ast_parse_spawn.cch:757-771`): a sixty-entry
  list mixing C keywords, libc (`malloc`, `printf`, `pthread_create`, `FILE`),
  runtime names (`CCSlice`, `cc_atomic_load`) and one project-specific
  `cleanup_fn`, used to decide whether a free identifier in a closure is a
  capture. Anything lowercase and not listed becomes an inferred capture.

### 8.2 Type names

- The family-base predicate `cc/include/ccc/cc_ufcs_families.h:24-43`
  (19 names) and its header routing (`:47-55`). Its comment already says not
  to grow it.
- `result_spec.c`: four short spellings, seven core Result types, and a
  21-row table of stdlib-predeclared `CCResult_*` specs (`:49-72`) whose
  comment says "keep in sync with the `CC_DECL_RESULT_SPEC` invocations in
  the ccc/std headers". Home: generate from those headers at build time.
- Result suffix list (`lower_header.c:350-352`), Result error faces
  (`pp_emit_core.cch:9478`), header-owned Result specs (`pp_emit_tu.cch:1061+`),
  prelude Result oks (`:1021`).
- Prebaked `CCVec_char`, `CCVec_size_t` (`lower_header.c:191-195`) and
  scalar `CCSlice_*` pre-instances (`type_registry.c:318-320`).
- Two differently spelled primitive lists (`preprocess.c:20527`, `:23385`),
  the `type_of` kind enum mirrored from `cc_type.cch` "keep in sync"
  (`:20691`), and three C-keyword lists in the text engine plus three in the
  lowerer.
- The lowerer's own type names seeded into its parser
  (`pp_ast_core.cch:761-768`: `AstNode`, `CEmit`, `FileTape`, …), a
  self-hosting hack.

### 8.3 UFCS method tables

- Map methods, thirteen names, duplicated thirty lines apart
  (`symbols.c:977-980`, `:1010-1013`) and installed again in
  `pp_emit_core.cch:6740`. Home: emit from `CC_MAP_DECL_UFCS`.
- Channel dispatch (`ufcs.h:242-281`, dead) and the lowerer's IO table
  (`pp_emit_ufcs.cch:1341-1350`).
- Arena `avail` → `cc_arena_remaining` (one-row table, `:1302-1305`); len/cap
  over six types (`:1469-1478`); `.release()` arms for four types
  (`:1405-1440`); string `sub`/`trim*` (`:561-562`); grammar entry methods
  (`:1935-1938`, `pp_emit_core.cch:6816`); a 16-spelling scalar destination
  whitelist (`:373-383`); 18 stdlib callees with slice-argument indices
  (`shadow_slc_known_arg`, `:4605-4625`, e.g. `cc_path_join` at 1 and 2).
- Ambient receivers (`cc_ufcs_families.h:66-72`) already live in user space.

### 8.4 Paths, headers, symbols, environment

Force-included headers (`<ccc/script/prelude.cch>` in `script_entry.c:1295`,
`<ccc/std/task.cch>` in `visitor_fileutil.c:342, 450`, four in the comptime
template prelude, `prelude.cch` plus `cc_ufcs.cch` in the hook slim TU);
header names in diagnostics (`emit_plan.c:3416, 3425`, `pp_emit_tu.cch:2468-2481`);
host include search paths and macOS SDK paths (`preprocess.c:17634-17662`);
include-root probes by literal relative path through four `../` levels
(`shadow_build.cch:399-409`, `shadow_lower.ccs:470-486`); the `/include/ccc/`
substring tests; `CCC_VERSION_BASE "0.3.4"` in `unit_header.c:9` and
scraped from the Makefile by the promote script.

Sixty-nine environment variables at 162 sites, none declared anywhere
machine-readable: compiler (`CC_INCLUDE_PATH`, `CC_TCC_LIB_PATH`, `CC_SYSROOT`,
`CC_STRICT_RESULT_UNWRAP`, `CC_HOME`, `CC_OUT_DIR`, …), toolchain passthrough,
eight cache switches with two spellings for the comptime cache
(`CC_COMPTIME_NO_CACHE`, `CCC_NO_COMPTIME_CACHE`), sixteen `CC_DEBUG_*`,
five `SHADOW_*`, eleven `CC_TEST_*`.

### 8.5 Numeric limits

Named limits are scattered over a dozen headers (`emit_limits.h`,
`CC_PASS_CHAIN_MAX 32`, `CC_STR_SCRATCH_MAX_SITES 256`, `CC_MAX_ASYNC_FNS 256`,
`CC_EMIT_PLAN_MAX_GENERICS 128` undiagnosed, `CC_COMPTIME_FN_MAX 32`,
`SHADOW_SCRATCH_CP_MAX 16`, `SHADOW_EH_STACK_CAP 8`, per-function caps of
16/32 defers, destroys, spawns and parallels, and so on). About 1020 fixed
`char[N]` buffers in `cc/src` and 1735 in `cc/shadow`; the dominant sizes
(256, 128, 1024, 64, 512) are conventions without names. There is no
`CC_TYPE_NAME_MAX`, `CC_PATH_MAX` or `CC_CALLEE_MAX`.

### 8.6 Existing homes

Five data-driven mechanisms already exist and are the targets for the
moves above: `.rules` files consumed by the `@grammar(rules)` engine;
`@typehooks` / `cc_type_register` (`docs/typehooks-typeviews.md` says the
point is "without the compiler special-casing type names"); the UFCS symbol
registry (`comptime/symbols.h:97-133`, `cc_ufcs.cch`, whose `CC_UFCS_PASS_TAG`
comment names "the compiler's hardcoded channel / slice dispatchers" as the
remaining debt); the type registry; and `CC_DECL_RESULT_SPEC` in the stdlib
headers. `@grammar(cli)` moving out of the compiler into `<ccc/std/cli.cch>`
is the worked example.

---

## 9. Emit quality sample

Four examples, lowered with `--emit-c-only --no-cache`:

| Example | Source lines | Emitted lines | `#line` markers | Longest line | Lines over 200 chars | `({` statement expressions |
|---|---|---|---|---|---|---|
| recipe_result_error_handling | 69 | 249 | 6 | 1583 | 11 | 7 |
| recipe_ordered_parallel | 88 | 350 | 15 | 1730 | 14 | 6 |
| recipe_ufcs_forms | 140 | 525 | 33 | 1583 | 17 | 11 |
| recipe_async_await | 55 | 544 | 161 | 1730 | 13 | 7 |

The 1500-character lines are the `_Generic` ladders behind `__cc_uw_is_err`
and the `@string` templates lowered as one-line statement expressions. The
`README` goal, "product C should read like hand-lowered code", holds for
statement structure and not for these.

---

## 10. Test harness

`tools/cc_test.c` walks `tests/` recursively (skipping `cparse/` and dot
entries), takes `.c`, `.ccs` and `.shcc`, and requires stems to be unique
across the tree. All expectation files are substring-per-line: `.stdout`
(680 files, `---` splits runs), `.stderr`, `.compile_err` (324; presence
means compile-fail, as does a `_fail` suffix), `.build_stderr` (needles
required in a successful build), `.args`, `.exit`, `.stdin`, `.ldflags`,
`.env`. A stem ending in `_smoke` additionally fails on any `warning:` in
the build stderr. `--quick` is the default and skips stems containing
`stress`, `lostwake`, `_race`, or a path containing `/stress`. `--filter`
is a substring over stem or path and, as a side effect, enables the
`c_pp_*` shadow smokes. Per-test run timeouts are a hard-coded ladder of
about 45 stems (`:532-608`).

`examples/` and `stress/` are not run by `cc_test`. `make examples-check`
compile-checks 31 examples; `make stress-check` runs `tools/run_all.ccs`,
which globs `stress/*.ccs` non-recursively with an exit-code-only policy
table. Subdirectories of `stress/` never run today.

### 10.1 Where `stress/break` plugs in

The directory name the request uses is `stress/break/`. Two hosts:

- **`cc_test`** (preferred: `.stdout` oracles, parallel jobs, the warning
  rule, the failure summary). It needs one line in `test_is_heavy`
  (`tools/cc_test.c:320-326`) so that `/stress/break/` is not treated as
  heavy, plus a directory walk root for `stress/break` next to `tests/`, or
  a symlink `tests/break` → `../stress/break`. Stems must be globally unique
  (prefix `break_`) and should end in `_smoke` with a `.stdout` needle so an
  empty run cannot pass.
- **`run_all.ccs`**: add a `run_files("stress/break/*.ccs", …)` call next
  to `:386` and a `break-check` target. Exit-code only, no output oracle.

The first population of `stress/break/` should be the ten diagnostic probes
of section 7.3 as `_fail` tests with column-pinned `.compile_err`, and a set
of "must compile and run" programs that hit the caps and buffers in
sections 5.6 and 5.7: a 3 KiB opaque static function body, 129 UFCS sites
in one expression, deeply nested `!>` inside `@parallel` inside `@scratch`,
a 200-byte type name in `Map::[K,V]`, forty-plus `@string` sites in one
function, a `@grammar` inside a template literal, a comment containing
`.foo(` inside an opaque switch body, a user file under `/tmp`.

### 10.2 Failing tests on this tree

22 of 1470, all also failing on pristine `main`: five UFCS lowering tests
(`ufcs_free_call*`, `ufcs_fnptr_field_header`, `ufcs_chain_handler_template`,
`ufcs_cstr_unsigned`), five JS/Python interop smokes, three header/unit
lowering tests, `string_at_scratch_multiline_smoke` and
`errhandler_same_e_reenter_fail` (both from the print-Result change),
`nursery_child_handle_smoke`, `raytracer_weekend_smoke`, `cparse_if_full_smoke`,
and the two `mem_*_fail` oracles.

---

## 11. Cleanup plan

Ordered by what each step unblocks. Each is a separate commit with its
own `stress/break` coverage.

1. **One diagnostic sink, user coordinates everywhere.** Route the text
   engine's `cc_pp_error_cat` / `cc_pass_error*` and the lowerer's
   `diag_*` through `cc/src/diag` (which exists and is unused), with one
   path spelling, `#line` paths the host compiler can open, columns from
   `cc__line_col_at` in the text engine, and `shadow_emit_pinned_block` on
   every multi-line scaffold starting with `!>`. Add column-pinned
   `diag_oracle` tests. This is the single change the user sees most.
2. **Diagnose in the lowerer what the host compiler currently reports.**
   Non-Result `!>`, unknown UFCS method on a bound receiver, and missing
   prelude are all knowable from the AST and the bind tables. Demangle in
   the replay for the rest (`cc_string_len` → `.len()` on `CCString`), and
   suppress "did you mean" suggestions of names the user cannot write.
3. **Stop silent bail-outs in the text rewriters.** Every `>= rem` /
   `>= sizeof` `break` in section 5.6 and every `NULL`-means-unchanged
   return in section 4.4 becomes a diagnosed failure. Fold the fixed
   buffers of the expression pipeline into one growable emit buffer; the
   1735 declarations are the symptom, the pipeline shape is the cause.
4. **Move the tables out.** In this order: the 21-row Result spec table
   (generate from `CC_DECL_RESULT_SPEC`), the map hash/eq fallback (delete;
   the probe exists), the duplicated map method lists (emit from
   `CC_MAP_DECL_UFCS`), destroy inference (already `@typehooks destroy`),
   the print family (attribute on `cc_print*`), no-return (`_Noreturn`),
   the arena call-shape table (attribute on the declarations), the
   `@scratch` name and size (one definition), the channel and IO tables
   (`@typehooks ufcs`). Each move deletes a compiler table and adds a test
   that a user-defined type with the same declarations gets the same
   treatment.
5. **Delete dead code and duplicate copies.** `cc_preprocess_file` and its
   kin, `pass_channel_syntax.c`'s uncalled entries, `visitor/ufcs.h`'s
   undefined declarations, the second comptime cache, the five snake-case
   generators (keep one), the six C-keyword lists (keep one), the two
   `.expect_error` conventions (keep none). Retire `COMPILER_CLEANUP_STATUS.md`
   and `PASS_CLEANUP_PLAN.md`.
6. **Name the limits and make caps fail.** One `cc_limits.h` for the
   numeric caps that remain, each with a diagnosed overflow; grow the
   rest. `cc_pass_chain_apply` past 32, `CC_EMIT_PLAN_MAX_GENERICS`,
   the 64-Vec header cap, the 128-site peel cap and the `guard < 8` loops
   are the first five.
7. **Fix the remaining silent degradations in header lowering**
   (section 6, items 1 to 3) and the driver (unreadable dependencies out
   of the cache key, `--no-line` not forwarded, read-only cache directory).
8. **Reduce the text engine's footprint in the native path.** The
   pre-parse rewrite (`cc_comptime_prepare_source`) is the one seam that
   runs on every unit; each of its ten scanners is a candidate for the
   tape or the parser, `@string` templates and `Type.fn(...)` first, so
   that positions the parser reports are positions in the user's file.

Not in scope: an owned C parser (`docs/c-parser.md`), a compiler IR, or
merging the legacy phase counts into the native metric, per
`ARCHITECTURE.md` section 5.
