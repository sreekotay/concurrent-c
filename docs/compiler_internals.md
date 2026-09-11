# Compiler internals

How a `.ccs` file becomes a binary, what code does each part of that, where
the compiler carries knowledge that belongs to user space, and where it is
fragile. This is the audit that precedes the cleanup; the cleanup plan is
the last section. Line numbers are as of the audit and drift; names do not.

Companion documents: [`cc/docs/ARCHITECTURE.md`](../cc/docs/ARCHITECTURE.md)
(constraints and ADRs), [`cc/lower/LOWERING.md`](../cc/lower/LOWERING.md)
(the C each construct lowers to),
[`cc/bootstrap/lowerer/README.md`](../cc/bootstrap/lowerer/README.md)
(seeds), [`build-when.md`](build-when.md) (which command after which edit).
`cc/docs/COMPILER_CLEANUP_STATUS.md` and `cc/docs/PASS_CLEANUP_PLAN.md`
describe the removed multipass driver and are archaeology.

---

## 1. The pipeline at a glance

```text
cc/bin/ccc (sh)          stale-header check → lower_headers → exec .ccc-bin
   │
cc/src/cc_main.c (C)     paths, mode, build.cc, cache keys, runtime .o, flags
   │
   ├─ comptime harvest   cc/src/preprocess/emit_plan.c         fns, factories, blocks
   ├─ comptime prepare   cc/src/preprocess/comptime_prepare.c  byte-offset rewrites
   ├─ comptime execute   cc/src/comptime/executor.c + libtcc   blocks run, fragments spliced
   ├─ module splice      cc/src/preprocess/preprocess.c        members into the root unit
   └─ stage              out/.cc-build/clean_comptime/         `#line 1 "<user file>"` on top
   │
out/cc/bin/cclower_cc --lower   (Concurrent-C, built from a committed seed)
   │
   ├─ lex                cc/lower/lex.cch          tokens with file, offset, line, column
   ├─ parse              cc/lower/parse_*.cch      AST, a span on every node
   ├─ index              cc/lower/index_impl.cch   declarations of the unit and its faces
   ├─ lower              cc/lower/lower_*.cch      AST to AST, one step per construct family
   ├─ headers            quoted `.cch` → `<rel.h>`, lowered in header mode, transitively
   └─ print              cc/lower/print_impl.cch   C with `#line`, plus `<out>.map`
   │
cc/src/cc_main.c (C)     host cc -c, then link tu.o + runtime.o + module objects + @link libs
```

Two things the picture in `ARCHITECTURE.md` does not show:

- **A text stage runs before the parser.** What the lowerer reads is not the
  user's file: the driver runs the comptime pipeline over it and writes the
  result under `out/.cc-build/clean_comptime/`.
  `cc_comptime_prepare_source` (`cc/src/preprocess/comptime_prepare.c:20`)
  does the byte-offset rewrites that stage needs: template dedent, `@grammar`
  splice, module export directives, `CC_GENERIC_FACTORY`, `static_map` calls,
  `@comptime if` and `@comptime for`. The passes that would rewrite surface
  the lowerer owns are left off: `Tweet.parse(...)`, `@string` templates and
  value-position `@comptime(expr)` reach the lowerer as the language, for its
  own steps to lower from the index and the AST. Every pass keeps the line
  count, and the stage carries `#line 1 "<user file>"`, so positions name the
  user's lines. The stage is a compromise: the comptime seam fed AST spans
  (`docs/plans/clean_lowerer.md` §2.6) replaces it.
- **The host compile runs in the driver.** `cclower_cc` prints C and a source
  map and stops there, so `--no-line`, the driver's post-processing of the
  emitted C (`cc_emit_polish_c`) and the module objects all apply before the
  host compiler speaks.

Header lowering has two paths. The stdlib and runtime faces
(`cc/include/**/*.cch` → `out/include/**/*.h`, stamp
`out/include/.headers_lowered.stamp`) are lowered by `out/cc/bin/lower_headers`,
run by the shell wrapper when headers are stale and by
`make -C cc lower-headers`; that is a fifteen-pass text pipeline, not the
lowerer (section 6). A unit's own quoted `.cch` faces are lowered by
`cclower_cc` in header mode into `out/.cc-build/clean/`.

---

## 2. Translation units and artifacts

One program produces:

| Artifact | Where | Built by |
|---|---|---|
| User TU | `out/.cc-build/clean/<stem>.<content-hash>.c` → `tu.o` | `cc__run_clean_lowerer` then `cc__compile_c_to_obj` (`cc/src/cc_main.c`) |
| Lowered faces of the unit | `out/.cc-build/clean/<rel>.h`, and `<face>_cch.c` for a module face | `cclower_cc` in header mode; the module C through a child run of the lowerer |
| Runtime unity TU | `cc/runtime/concurrent_c.c` includes ~35 sibling `.c` files (`:27-64`); rewritten to `out/runtime/concurrent_c.c`; compiled per flag variant into `out/.cc-build/host/<host-fp>/runtime-<variant>.o` with a `.recipe` | `cc__ensure_runtime_obj` (`cc/src/cc_main.c`) |
| Lowered stdlib headers | `out/include/**/*.h`, stamp `out/include/.headers_lowered.stamp` | `lower_headers` |
| Comptime hook dylibs | `$HOME/.cache/concurrent-c/comptime-hooks/<fnv>.dylib` (or `$TMPDIR`, `/tmp`), 256 MB budget | `cc__build_compile_and_load` (`cc/src/comptime/hook_compile.c:847`) |
| Lowerer seed | `cc/bootstrap/lowerer/<MAJOR.MINOR.PATCH-SEED>/`: the lowered C of the four tools, the C of module `lower` (`lower_cch.c`), and the faces that C includes; pointer `last-good` | `scripts/ship_seed.sh --promote` |

**Body-only-in-the-runtime.** The decision that an arena function has an
out-of-line body only in the runtime TU is made in the headers, not the
driver: `cc/runtime/concurrent_c.c:15` defines `CC_ARENA_IMPL`, and
`cc/include/ccc/cc_arena.cch:42-48` chooses `CC__ARENA_SYS` (external
definition) versus `static inline`. `cc/runtime/arena_state.c:1` sets the
same macro so tool binaries get the bodies, which is why `arena_state.o` is
filtered out of `libshadow_comptime.a` (`cc/Makefile:212-217`) to avoid an
ODR clash with `concurrent_c.o`.

**Cache keys.** Every cache is keyed by content: the bytes of the inputs and
the bytes of the toolchain, never an mtime. One helper,
`cc_toolchain_content_fp()` in `preprocess.c`, folds the running binary,
`.ccc-bin`, the lowering binaries found from the repository root and the
toolchain version, memoized per process; every key below folds it. The C of
a module face folds the lowering tool's own bytes on top of that
(`cc_face_module_key`, `preprocess.c`). Same-second, same-size edits are the
test (`tests/cache_key_same_second_smoke.ccs`).

| Cache | Key |
|---|---|
| Lowered local headers (`out/include/**/*.h`, `$TMPDIR/cc-lowered-<uid>/`) and module units (`<face>_cch.c` beside the `.h`) | `<h>.key` sidecar: face bytes, every quoted `.cch` it includes transitively (its members among them), toolchain; the `<face>_cch.c.key` adds the lowerer that produced it |
| Include expansion (`~/.cache/concurrent-c/incexp`) | input bytes, host cc, `.ccc-bin`, repo root, toolchain; deps sidecar holds a content hash per expanded file |
| Driver TU emit (`out/.cc-build/<build>__<target>__<unit>.meta`) | source bytes, `cc_depends`, transitive `.cch` bytes, `.ccc-bin` and lowering tool bytes, version, flags, env, consts |
| Driver object (`.obj` meta) | emit key, target, flags, env, host fingerprint, plus the `.d` check |
| Comptime hook dylibs (`~/.cache/concurrent-c/comptime-hooks`) | TU bytes, argv, host cc, toolchain |

`--no-cache` still writes the lowered C and `tu.o`; it only suppresses the
keys. A dependency that cannot be read folds a sentinel with its path, so a
file that appears later changes the key.

**The seed bootstrap.** The tools are written in Concurrent-C, so a cold
clone builds them from the committed seed: stage zero of `make -C cc`
host-compiles `cc/bootstrap/lowerer/<last-good>/` with no lowerer in hand.
`make -C cc lower-cc` then rebuilds the four tools from their sources with
the seeded ones, and `./scripts/lowerer_selfhost.sh` is the fixed point that
says the two agree: the tools lower their own sources, the host C compiler
builds that, and the second binary must lower the same sources to the same
bytes. `./scripts/ship_seed.sh --promote` freezes a new pin and flips
`last-good`. `out/runtime/.seed.stamp` (`cc/Makefile:280-292`) exists only to
break the cycle on a cold tree by seeding a crude host-C `out/include` first.

A unit whose `#!ccc … version=` pin names the 0.3 line is lowered by that
line's frozen seed instead: `ccc` host-compiles it on demand
(`make -C cc shadow_lower-pin PIN_VER=… PIN_OUT=…`, run quietly by the
driver) and caches the binary under `out/.cc-build/lowerers/<pin>/`. Those
two pins have no sources in the tree.

---

## 3. The driver (`cc/src/cc_main.c`)

Responsibilities, in order of execution: layout resolution
(`cc_init_paths`: `$CC_HOME`, then a prefix install, then a dev checkout
found by string-cutting `/cc/bin/ccc` out of argv[0], then walking up from
cwd, then cwd), mode dispatch, `build.cc` parsing (`cc/src/build/build.c`),
unit kind and version-pin resolution, `.shcc` and `#!ccc` rewriting into
content-keyed copies under `out/.cc-build/`, the comptime pipeline and the
stage it writes, runtime object management, flag folding, the lowerer run,
and then the host compile and link:

```text
cclower_cc --lower <staged unit> -I <cc/include> --root <repo> --h-root <out/.cc-build/clean>
           --known-types <file> [--no-line] [--quote-dir DIR] [--modules F]
           [--schema-variants F] [--instantiate F] [--factories F] -o <out.c>
```

The driver writes `--schema-variants`, `--instantiate` and `--factories` from
what the comptime pipeline learned, because the lowerer is a separate process
and those facts are not in the text it reads. Lowering stderr is captured to
`<out>.diag` and replayed on warm cache hits. The host compile line is:

```text
<CC> -std=c11 -D_DEFAULT_SOURCE [-I<CC_INCLUDE_PATH>] -I<srcdir> -I out/include
     -I cc/include -I cc -I. -I examples -ffunction-sections -fdata-sections
     -Werror=implicit-function-declaration $CFLAGS <cc_flags> [-DNDEBUG] [-g]
     -c <lowered>.c -o tu.o
<CC> tu.o <runtime.o> <module objects> -o <bin> -lpthread -lm [-Wl,--gc-sections] <@link libs>
```

The host profile (`cc/src/build/host_cc_profile.c`) probes the compiler once
per fingerprint with three snippets and records whether `-std=c11`, `-B`,
`-Uarm` or `-DCC_NO_LIBLFDS` are needed. The single constant
`CC_HOST_C_STD_OPTION` (`host_cc_profile.h:24`) is applied at every session,
with one residual copy of the literal in
`cc/src/comptime/shadow_tcc_compile.c:108`.

What the driver hard-codes (section 8 has the full catalogue): the runtime
source list used for staleness (fifteen names plus `vendor/zmij.c`; a new
runtime file never marks the object stale), `-DCC_ENABLE_ASYNC` always on,
`-lpthread -lm`, the tool search order, `third_party/tcc` paths, and
forty-odd environment variables.

Fragility in the driver: the child argv is `char *argv[28]`; flag buffers are
2048 to 4096 bytes built with `strncat` and no overflow check
(`cc__compile_c_to_obj` can drop the output path); `system()` results are
compared to zero so a missing shell is a compiler error; `@link` scanning
runs `cc -E … 2>/dev/null` through `popen` with the status unchecked and `cc`
hard-coded.

---

## 4. The C-side text engine (`cc/src/preprocess`, `cc/src/visitor`)

The multipass text-rewrite front is gone as a product path
(`--frontend=legacy` is a hard error), and the code is not: `cc/Makefile`
re-adds seven visitor files by name, every `preprocess/*.c` except
`grammar_stub.c` is in `LOWER_HEADERS_SRCS`, and the same objects are
archived into `libshadow_comptime.a`, which both the driver and `cclower_cc`
link. Running compile-time code needs a C compiler, so that archive plus
libtcc is on the lowerer's link line too: it evaluates `@comptime(expr)`
through the same executor the driver uses.

### 4.1 Live orchestrators

- **`cc_comptime_prepare_source`** (`comptime_prepare.c:20`), the stage
  rewrite described in section 1, on every unit.
- **`cc_lower_header_string`** (`cc/src/header/lower_header.c:785`), the
  `.cch` → `.h` pipeline behind `lower_headers`: dedent → strip `@comptime` →
  strip generic factories → `.cch` includes to `.h` → `@as` to comment →
  strip `@typeview`, `@typehooks`, `@destroy`/`@detach` → shared type-syntax
  lowering (itself a six-pass sub-chain) → `@variant` → splice header Vec
  decls → `T!>(E)` to `CCResult_T_E` → inferred Result ctors → Result field
  sugar → `!>`/`?>` unwrap → `@err` syntax → per-use splice of Result
  declarations.
- **`cc__apply_phase1_canonical_passes` / `phase3`** (`preprocess.c`), the
  full legacy chain of about forty passes, with exactly one live caller:
  `cc_preprocess_to_string_ex` from `hook_compile.c:940`, and only when a
  comptime batch contains a UFCS type hook. `CC_DEBUG_CANON` prints each pass
  name; that is the closest thing to a manifest.

### 4.2 Dead code still compiled

`cc_preprocess_file`, `cc_preprocess_to_string`, `cc_preprocess_canonicalize`,
`cc_preprocess_emit_splice` and the reparse-coordinate accounting have no
callers. Two of the three entry points of `pass_channel_syntax.c` (2270
lines) are uncalled; `preprocess.c` carries its own copy of the third.
`visitor/ufcs.h` declares four `cc_ufcs_rewrite_*` functions that are defined
nowhere. `pass_type_syntax.c`'s slice and Result rewrites are shadowed by
copies in `preprocess.c`.

### 4.3 How this engine reports errors

There is no diagnostic sink. `cc/src/diag/diag.h` exists and is never used
here; every message is `fprintf(stderr, …)`, through `cc_pp_error_cat`
(`preprocess.c:111`, 71 sites) or `cc_pass_error*` (`pass_common.h:250-291`),
plus 87 position-free `fprintf`s in `preprocess.c` alone. Positions are line
and column in the buffer the pass was handed, counted from byte zero and
ignoring `#line`. Columns from this engine are structurally always 1: the
scanner advances `col` only inside `cc_scanner_skip_non_code_ex` and undoes
the increment when it reaches code.

### 4.4 Fragility specific to this engine

- `cc_pass_chain_apply` tracks only the first 32 allocations of a chain but
  installs every result; phase 1 plus phase 3 on one chain issue about forty.
  Nothing is reported.
- Every rewriter returns `NULL` for "nothing to do", a buffer for success,
  and `(char*)-1` for a diagnosed error, and many failure paths return
  `NULL`. A mid-construct bail-out passes the CC surface through, and the
  failure appears later as a host C error on generated text.
- Hand-rolled string/comment skippers: `grammar_seam.c:231-245` does not know
  about backtick templates; `preprocess.c:11745-11762` is another copy.
  `make lint-scanners` only prevents growth.
- Truncation: 250 `snprintf` calls in `preprocess.c`, about 97 checked. One
  builds `Map_%s_%s` into `char mangled[128]` unchecked, so two long type
  parameters truncate into a different, valid-looking name that then fails to
  match its own declaration.
- Process globals with documented reentrancy hazards:
  `g_cc_pp_splice_last_anchor`, `g_rewrite_root_path`, `g_ufcs_header_path`,
  `g_script_prelude_off`, `g_va_hits_s`, `cc__schema_reg`.
- Any user file whose path is under `/tmp` skips the async-channel-await and
  block-on validation checks, because those paths are how the engine
  recognises its own temp files.

---

## 5. Inside the lowerer (`cc/lower`, about 46k lines)

### 5.1 Tokens

`cc_lex` turns bytes into a tape of tokens. Every token carries `file_id`,
offset, line and column; comments, blank lines and `#line` are tokens, so a
`#line` in the input rebases positions and the printer can replay trivia.
One grammar covers C and the CC tokens: `!>`, `?>`, `=>`, `::[`, `[:`,
`@word`, and backtick templates with `${` nesting.

### 5.2 The AST and the index

The parser is recursive descent over that tape, producing a `@variant` node
per kind with a span on every node. Parse errors carry spans and recovery is
at statement boundaries, so one file reports every error it has.

The index (`index.cch`, `index_impl.cch`) reads the declarations of the unit
and of every `.cch` it includes, with the same parser. Every fact a step
needs about a name is answered from a declaration and verified against one:
whether `f` returns a Result, whether `T` has a `destroy`, which method set
`x.f()` resolves against, whether a call may be discarded. `--dump`,
`--resolve`, `--sites` and `--gaps` on `ccindex_cc` ask those questions one
at a time (`cc/lower/INDEX_GAPS.md`).

### 5.3 The steps

`CcLowerer_lower_unit` runs one step per construct family, in the order the
step list at the top of `cc/lower/lower.cch` gives: results, cleanup,
includes, own, generics, slices, slice arguments, strings, `as:` arguments,
string switch, closures, create, for-in, deadline, parallel, async, channels,
variants, UFCS. Each step rewrites nodes in place, and every node it creates
copies the span of the node it replaces, so the printer pins the generated
lines to the user's line. A construct a step cannot lower is a diagnostic at
its position, never a node left for the host compiler. Steps after an
erroring step still run, and the driver refuses to print C when the sink has
errors. The C each family produces is written down in `cc/lower/LOWERING.md`.

### 5.4 Print and the source map

The printer emits `#line` at every file or line change and records, for each
emitted line, `(user file, user line, column offset)` in `<out>.map` beside
the C. `cclower --lower` and `--print` check the text against that map before
writing: every emitted line has an entry, and the host compiler's attribution
through the `#line` directives agrees with it.

### 5.5 What the lowerer still carries in code

The names it carries are a work list, not a design: the ambient receivers
(`cc_std_out.write` / `std_out.write`), the scalar type spellings
`type_of(T).kind` folds, the C library exits that count as divergence,
`println` / `eprintln` renamed by spelling, `allocT` and `block_on` as the
members that bind a type formal, `send_task_hybrid` as a spawn-family method,
the `Vec` family's `CCVec_` instance prefix, and a `_t` suffix standing in
for the C standard typedefs. Each has a declaration form that replaces it;
`cc/lower/INDEX_GAPS.md` is the list and the regeneration command.

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
2. `cc__strip_comptime_blocks_header` (`lower_header.c:657`) diagnoses an
   unbalanced block, or a `@comptime for` / `@comptime if` it cannot
   harvest, at its line and the lowering fails; this one is closed.
3. `cc_lower_header` (`:1136`) writes the raw `.cch` bytes as the `.h`
   whenever lowering produced nothing, conflating "nothing to rewrite"
   with "every pass bailed".
4. At most 64 Vec instances per header; the rest are dropped
   (`lower_header.c:289-291`).

The outside-repo-root lowering no-op from `CLAUDE.md` is fixed
(`preprocess.c:15154-15180`, with a temp-directory fallback and a failure
flag checked by three callers).

**Modules** (spec §1.7, §1.8). A face with bodies is a module, and the
driver owns the mechanism. `cc_file_start_pragmas`
(`cccportable.c:369`) reads `#pragma(@module) "name"` and refuses
`#pragma(@per_tu)` at its line. `cc__module_ctx_of_root`
(`preprocess.c:15860`) sets the module context from the rewrite root: a
`.cch` is the face of the module its stem names, a `.ccs` with the pragma
is a program module unless `name.cch` sits beside it, in which case it is a
member and is refused as a unit. `cc__member_include_class`
(`preprocess.c:15991`) classifies every quoted include as a member (spliced in
place, declared members only), a face (its lowered `.h`), or a refusal at
the site. The grade of a face — interface, library (all-static with unit-only
forms: spliced into each includer, no `.h`), or module — is the grade of
its module unit, face plus members and library includes (`cc__local_cch_grade`, `preprocess.c:15636`); `@comptime`
blocks and functions and generic factories are compile-time items that do
not grade (`cc__cch_comptime_only_item_end`, `preprocess.c:15407`). The module
`.h` is the extract of that unit: bodies stripped to prototypes, `static`
functions and data omitted, exported data `extern`, member includes placed
ahead of the members, one link marker on line 1
(`cc__lower_local_cch_header_in`, `preprocess.c:18553`, and
`lower_header_with`, `cclower.ccs:593`, over the stages `--modules` names).
At link time the driver collects markers from every emitted C and every
`.h` it reaches (`cc__collect_link_markers_text`, `cc_main.c:4633`), stages
the unit (`cc_module_stage_text`, `preprocess.c:19470`: the includes of the
unit as prologue, a `#line` to the face, the text), lowers it in a
child `ccc --emit-c-only` run of the lowerer to `<face>_cch.c`
beside the `.h` (`cc__ensure_module_c`, `cc_main.c:4765`), compiles it per
host-flag variant (`cc__ensure_module_obj`, `cc_main.c:4850`) and puts the
object on the link line (`cc__module_objects`, `cc_main.c:4909`). A marker
the driver cannot satisfy is an error.

---

## 7. Diagnostics

### 7.1 Two sinks and a dead third

| Stage | Sink | Format |
|---|---|---|
| Driver text engine | `cc_pp_error_cat`, `cc_pass_error*`, raw `fprintf` | `<repo-relative>:L:1: error: …` (column always 1) |
| Lowerer | one sink: `CcDiag` / `CcDiags` (`cc/lower/diag.cch`), sorted by position, with a caret snippet | `<path the user can open>:L:C: error: …` plus notes, each with its own location |
| Host compiler | as the compiler wrote it, in user coordinates through the `#line` directives in the lowered C | `<user file>:L:C: error: …` |

`CcDiag` is also the error arm of every `T!>(CcDiag)` inside the lowerer, so
a step that cannot continue returns the diagnostic that says why, at the
position that caused it.

`cc/src/diag/diag.c` (`CCDiag`, `CCSourceSpan`, `cc_diag_translate_tcc_error`,
`CCSourceMap`) is a designed diagnostic system whose only caller is
`cc_diag_init` in `cc_main.c`. `cc/src/diag/mangle.c` exists for demangling
and is unused.

### 7.2 Position machinery

Every token carries file, line and column; every node carries the span of the
tokens it came from; every node a lowering step creates copies the span of
the node it replaced. The printer writes `#line N "path"` at each file or
line change and records `(user file, user line, column offset)` per emitted
line in `<out>.map`. `cclower_cc` verifies the text against that map before
writing it: an emitted line with no entry, or an attribution the `#line`
directives disagree with, fails the run rather than shipping a wrong
position.

`--no-line` is forwarded to the lowerer, which then omits the directives; the
map still records the attribution.

What the map is not yet used for: the driver does not read it back to remap
host compiler diagnostics. Host messages are right because the `#line`
directives point at the user's file, so they carry the user's coordinates but
the compiler's vocabulary, and a lowered name (`cc_string_len` for `.len()`)
reaches the user unless the lowerer diagnosed the site first.

### 7.3 The diagnostic probes

`stress/break/break_diag_*_fail` holds the probes, each with a
`.compile_err` oracle; four carry an `.xfail` naming what the compiler does
today.

| Probe | Pins |
|---|---|
| `break_diag_syntax_fail` | a missing `;` after CC syntax |
| `break_diag_type_fail` | a type error on a plain C statement |
| `break_diag_ufcs_miss_fail` | `s.nosuch()` on a bound receiver (`.xfail`) |
| `break_diag_undecl_ufcs_fail` | an undeclared variable in a UFCS chain |
| `break_diag_template_unclosed_fail` | an unclosed `${` in `@string` (`.xfail`) |
| `break_diag_after_template_fail` | an error on the line after a multi-line template |
| `break_diag_unwrap_nonresult_fail` | `!>` on a non-Result |
| `break_diag_errhandler_mismatch_fail` | a handler whose error type does not reach `E` |
| `break_diag_missing_include_fail` | a missing prelude include |
| `break_diag_parallel_syntax_fail`, `break_diag_parallel_undecl_fail` | errors inside a `@parallel` arm |
| `break_diag_two_host_errors_fail` | two host errors in one unit |
| `break_diag_two_stages_fail`, `break_diag_lowerer_then_host_fail` | a lowerer error and a host error together (`.xfail`) |

Stage masking is what the last two pin: a lowerer error stops the run before
the host compiler speaks, so the two never print together.

### 7.4 Where positions are lost

1. Text-engine columns are always 1 (section 4.3), and that engine still runs
   on every unit as the comptime stage and on every `.cch` the header
   pipeline lowers.
2. A message about a construct the comptime stage rewrote names the line the
   stage kept, not the column, because the stage counts lines and not
   columns.
3. The source map is written and verified but not read back, so a host
   diagnostic keeps the host's vocabulary (section 7.2).
4. Two spellings of one path across the two sinks: the text engine prints
   repository-relative, the lowerer prints the path it was given.

### 7.5 What the tests pin

`.compile_err` is substring-per-line with no ordering or count
(`tools/cc_test.c:375-403`). Of 322 such goldens, 70 pin a `file:line` and
21 pin a column. The ten `diag_oracle_*` tests cover line fidelity and say
in their comments that columns are omitted so clang and TCC both match.
Column fidelity is barely tested, which is why a wrong column can ship
without CI noticing. Three `.expect_error` sidecars exist and nothing
reads them.

---

## 8. Hard-coded knowledge that belongs in user space

Scale first, on the C side. String-literal comparisons against identifier
names: `preprocess.c` 451, `cc_main.c` 159, `emit_plan.c` 76,
`type_registry.c` 60. Runtime symbol names (`cc_*`, `__cc_*`, `CC*`) emitted
or matched verbatim: `preprocess.c` 279, `result_spec.c` 87. On the lowering
side the rule is that every fact comes from a declaration and is verified
against one; what is left is the work list in `cc/lower/INDEX_GAPS.md`
(section 5.5), and each entry there names the declaration form that retires
it.

### 8.1 Builtin function names

- **The print family**: `preprocess.c:7331` rewrites six names to `cc_<name>`,
  and the lowerer renames `println` / `eprintln` by spelling. Home: an
  attribute on the `cc_print*` declarations in `stdio.cch`, which also
  decides the optional-Result discard.
- **`@scratch` and `__cc_str_scratch`**: the C spelling is a literal in
  `preprocess.c:1643, 2009, 2061, 2091-2094` and in the lowerer's string
  step. Home: one definition of the scratch arena's name and size, consulted
  by both.
- **Arena allocation call shapes**: `preprocess.c:811-812` and
  `pass_unwrap_destroy.c:855-856` carry `cc_arena_alloc_T` with hand-written
  lengths 16 and 22; epoch tracking hard-codes `cc_arena_reset` / `free` /
  `destroy`. Home: an `as:` style attribute on the declarations.
- **Destroy-callee inference**, duplicated: `preprocess.c:493-512` and
  `pass_unwrap_destroy.c:640-675` each pick `cc_arena_destroy`,
  `cc_arena_checkpoint_destroy`, `cc_slice_destroy`, `cc_nursery_destroy`,
  `cc_channel_free` from a declared type. Home: `@typehooks destroy`, which
  exists and is what the lowerer reads.
- **Map key hash and equality** (`preprocess.c:14530-14540`): a seven-row
  table with substring rules, so a key type merely containing `64` gets
  `cc_map_hash_u64`. The declared-symbol probe (`cc_map_key_hash_<mangled>`)
  already exists; the table is the fallback that never went away.
- **Family call composition** (`preprocess.c:6855-6880`) prefixes
  `__cc_map_generic_`, `cc_command_`, `cc_file_`, `cc_arena_`, `cc_string_`,
  `cc_slice_`; `:6870` silently renames `append` to `push` for strings.
- **No-return functions** (`pass_result_unwrap.c:1402-1407`): ten names, of
  which only `cc_error_exit` is stdlib. Home: `_Noreturn` on the
  declaration, which is what the lowerer's divergence check reads.
- **Container constructors** `cc_vec_new`, `cc_vec_from`, `cc_map_new` and
  the `Vec::[` → `CCVec` alias table (`preprocess.c:7405-7420, 8710, 8954`).
- **Python extraction verbs** `as_list` / `as_map` (`preprocess.c:9259`).
- **Grammar engines**: `rules` / `schema` compiled in, `cli` deliberately in
  `<ccc/std/cli.cch>` (`grammar_rules.c:5182`), and three JSON grammar names
  mapped to `.rules` paths (`:660-662`). The `cli` move is the template.

### 8.2 Type names

- The family-base predicate `cc/include/ccc/cc_ufcs_families.h:24-43`
  (19 names) and its header routing (`:47-55`). Its comment already says not
  to grow it.
- `result_spec.c`: four short spellings, seven core Result types, and a
  21-row table of stdlib-predeclared `CCResult_*` specs (`:49-72`) whose
  comment says "keep in sync with the `CC_DECL_RESULT_SPEC` invocations in
  the ccc/std headers". Home: generate from those headers at build time.
- Result suffix list (`lower_header.c:350-352`) and prebaked `CCVec_char`,
  `CCVec_size_t` (`:191-195`); scalar `CCSlice_*` pre-instances
  (`type_registry.c:318-320`).
- Two differently spelled primitive lists (`preprocess.c:20527`, `:23385`),
  the `type_of` kind enum mirrored from `cc_type.cch` "keep in sync"
  (`:20691`), and three C-keyword lists in the text engine.

### 8.3 UFCS method tables

- Map methods, thirteen names, duplicated thirty lines apart
  (`symbols.c:977-980`, `:1010-1013`). Home: emit from `CC_MAP_DECL_UFCS`.
- Channel dispatch (`ufcs.h:242-281`), declared and dead.
- Ambient receivers (`cc_ufcs_families.h:66-72`) already live in user space,
  and are the one table the lowerer reads rather than carries.

### 8.4 Paths, headers, symbols, environment

Force-included headers (`<ccc/script/prelude.cch>` in `script_entry.c:1295`,
`<ccc/std/task.cch>` in `visitor_fileutil.c:342, 450`, four in the comptime
template prelude, `prelude.cch` plus `cc_ufcs.cch` in the hook slim TU);
header names in diagnostics (`emit_plan.c:3416, 3425`); host include search
paths and macOS SDK paths (`preprocess.c:17634-17662`); the `/include/ccc/`
substring tests; the include roots the driver hands the lowerer (`cc/include`,
`out/.cc-build/clean`); `CCC_VERSION_BASE` defaulted in `unit_header.c:9`,
set from `cc/Makefile` at compile time and scraped from that same variable by
`scripts/ship_seed.sh`.

Sixty-nine environment variables at 162 sites, none declared anywhere
machine-readable: compiler (`CC_INCLUDE_PATH`, `CC_TCC_LIB_PATH`, `CC_SYSROOT`,
`CC_STRICT_RESULT_UNWRAP`, `CC_HOME`, `CC_OUT_DIR`, …), toolchain passthrough,
eight cache switches with two spellings for the comptime cache
(`CC_COMPTIME_NO_CACHE`, `CCC_NO_COMPTIME_CACHE`), sixteen `CC_DEBUG_*`,
five `SHADOW_*` (the quote directory and the runtime object among them), and
eleven `CC_TEST_*`.

### 8.5 Numeric limits

Named limits are scattered over a dozen headers (`emit_limits.h`,
`CC_PASS_CHAIN_MAX 32`, `CC_STR_SCRATCH_MAX_SITES 256`, `CC_MAX_ASYNC_FNS 256`,
`CC_EMIT_PLAN_MAX_GENERICS 128` undiagnosed, `CC_COMPTIME_FN_MAX 32`, and so
on). About 1020 fixed `char[N]` buffers remain in `cc/src`; the dominant
sizes (256, 128, 1024, 64, 512) are conventions without names. There is no
`CC_TYPE_NAME_MAX`, `CC_PATH_MAX` or `CC_CALLEE_MAX`. The lowering side has
no fixed buffers for user-sized text: names, spans and emitted text are
arena-backed builders, and a table that fills is a diagnostic.

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

Take one with the lowerer itself, over the recipes the audit used:

```sh
for f in recipe_result_error_handling recipe_ordered_parallel \
         recipe_ufcs_forms recipe_async_await; do
  ./cc/bin/ccc build --emit-c-only --no-cache "examples/$f.ccs" -o "/tmp/$f.c"
done
awk '{ if (length($0) > 200) n++ } END { print FILENAME, NR, n+0 }' /tmp/*.c
```

What to expect. `#line` on every statement whose line changed, and an entry
in `<out>.map` for every emitted line. Scaffolds print as statements, one per
line: an unwrap is a block with the temp, the test and the read; a `@string`
template is one push per literal or slot, each on its own line; a handler is
a label at the end of the function with its cell declared at the top. The
long lines that remain are the `_Generic` projections that read an error
through its `as:` faces, which are expressions by construction and print as
one. The `README` goal, "product C should read like hand-lowered code", is
what these numbers measure.

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
is a substring over stem or path. Per-test run timeouts are a hard-coded
ladder of about 45 stems (`:532-608`).

`examples/` and the top level of `stress/` are not run by `cc_test`.
`make examples-check` compile-checks 31 examples; `make stress-check` runs
`tools/run_all.ccs`, which globs `stress/*.ccs` non-recursively with an
exit-code-only policy table. Of the subdirectories of `stress/`, only
`stress/break/` runs: `cc_test` walks it as a second root beside `tests/`
(`tools/cc_test.c:1273-1275`), and `test_is_heavy` exempts it from
`--quick` (`:362`). `stress/bridge`, `stress/c`, `stress/go` and
`stress/zig` are run by neither runner (`stress/bridge` has its own fuzz
and publish scripts).

### 10.1 Where `stress/break` plugs in

`stress/break/` has one host, `cc_test` (`.stdout` oracles, parallel
jobs, the warning rule, the failure summary): `test_is_heavy`
(`tools/cc_test.c:357-364`) exempts `/stress/break/` from the heavy rule
and the directory walk pushes `stress/break` as a root next to `tests/`
(`:1273-1275`), with the same sidecars as `tests/`. Stems are globally
unique (prefix `break_`) and a "must run" program ends in `_smoke` with a
`.stdout` needle so an empty run cannot pass. `run_all.ccs` does not run
it; a `run_files("stress/break/*.ccs", …)` call next to `:386` and a
`break-check` target would add an exit-code-only host.

The directory holds the diagnostic probes of section 7.3 as
`break_diag_*_fail` tests with `.compile_err` oracles, and "must compile
and run" programs aimed at the shapes a lowering pass is easiest to get
wrong: a switch body of a few KiB whose comments and strings spell
`.bar(`, 130 UFCS sites in one expression, a 150-character type name
through `Vec::[T]`, forty call-local `@scratch` templates in one function,
template literals holding `@grammar`, `!>`, `::[` and `[:]` as text, and
`!>` inside a `@parallel` arm consuming a `@scratch` template. A shape the
compiler still refuses carries `<stem>.xfail`, whose first line says what
it does today; that is the only marker `cc_test` reads, and an XPASS
counts as a failure until the marker goes.

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
   engine's `cc_pp_error_cat` / `cc_pass_error*` through the same shape the
   lowerer already has: a message with a span, one path spelling, and a
   column from `cc__line_col_at`. Add column-pinned `diag_oracle` tests.
   This is the single change the user sees most.
2. **Read the source map back.** The driver writes the lowered C and
   compiles it, so it can map a host diagnostic to the user's line and
   demangle the name in it (`cc_string_len` → `.len()` on `CCString`) from
   the declaration index, and drop "did you mean" suggestions naming
   identifiers the user cannot write.
3. **Diagnose in the lowerer what the host compiler still reports.**
   Non-Result `!>`, an unknown UFCS method on a bound receiver and a missing
   prelude are knowable from the index; the `break_diag_*` markers say which
   of them still reach the host.
4. **Move the tables out.** In this order: the 21-row Result spec table
   (generate from `CC_DECL_RESULT_SPEC`), the map hash/eq fallback (delete;
   the probe exists), the duplicated map method lists (emit from
   `CC_MAP_DECL_UFCS`), destroy inference (already `@typehooks destroy`),
   the print family (attribute on `cc_print*`), no-return (`_Noreturn`), the
   arena call-shape table (attribute on the declarations), and the
   `@scratch` name and size (one definition). Each move deletes a table and
   adds a test that a user-defined type with the same declarations gets the
   same treatment. `cc/lower/INDEX_GAPS.md` is the same list from the
   lowering side.
5. **Delete dead code and duplicate copies.** `cc_preprocess_file` and its
   kin, `pass_channel_syntax.c`'s uncalled entries, `visitor/ufcs.h`'s
   undefined declarations, the second comptime cache, the C-keyword lists
   (keep one), the `.expect_error` sidecars nothing reads. Retire
   `COMPILER_CLEANUP_STATUS.md` and `PASS_CLEANUP_PLAN.md`.
6. **Name the limits and make caps fail.** One `cc_limits.h` for the numeric
   caps that remain in `cc/src`, each with a diagnosed overflow; grow the
   rest. `cc_pass_chain_apply` past 32, `CC_EMIT_PLAN_MAX_GENERICS` and the
   64-Vec header cap are the first three.
7. **Fix the remaining silent degradations in header lowering**
   (section 6, items 1 to 3) and the driver (unreadable dependencies out of
   the cache key, a read-only cache directory).
8. **Shrink the text stage.** `cc_comptime_prepare_source` runs on every
   unit, and each of its scanners is a candidate for the lowerer's own
   parser: `@grammar` bodies and `static_map` first, so that a position the
   lowerer reports is a position in the user's file. The end of that road is
   the comptime seam on AST spans (`docs/plans/clean_lowerer.md` §2.6), after
   which the stage goes and the lowerer reads the user's bytes.
9. **Retire the 0.3 seeds.** They exist so a unit pinned below 0.4 still
   lowers. When the pins in the corpus are gone, the two frozen seeds, the
   `shadow_lower-pin` rule and the driver's pin route go with them.

Not in scope: an owned C preprocessor for system headers
(`docs/c-parser.md`) or a compiler IR, per `ARCHITECTURE.md` section 5.
