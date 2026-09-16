# Design review, 2026-09

What this is: a review of Concurrent-C's concepts as the corpus exercises
them, written to argue about what works, what is theater, what is open, and
what is interesting. It is not spec. Spec is present-tense normative and
carries no reasoning; this file is all reasoning. Where the two disagree,
spec wins and this file is wrong.

Method: the specimens under `real_projects/`, the three out-of-tree
specimens (cctext, rlsw-cc, stylo-cc), the CVE locality study, the lowerer
read as a program written in CC, and the bootstrap chain. Claims cite files.
Counts are grep counts and are approximate.

Every finding carries one tag:

| Tag | Meaning |
|-----|---------|
| **design** | The model would produce this in fresh code written by someone who knows the idiom. A question for the language. |
| **drift** | The code predates the concept it now contradicts. Chronology, not disagreement. |
| **unimplemented** | Spec or a draft already says what should happen; the lowerer does not do it yet. |
| **defect** | The lowerer does something wrong and the specimen worked around it. |
| **doc** | The pages describe a product of primitives as if it were a primitive, or describe something the code no longer does. |

Only **design** rows are arguments about the language. The others are work.

Sections are ordered by what a reader meets first in a program, not by
what was designed first. A twenty-line script uses the local system on
line one and an arena it never named.

---

## 1. The local system

Errors, cleanup, and resumption are one mechanism. Every line that can
fail says so; every obligation is registered where it arises; every exit
discharges the same ledger; and a failure handled in scope resumes at the
next statement. Nothing in it is resolved at run time.

### 1.1 One form at the site

A fallible site is `CALL !>`, optionally with a binder and a body:

```c
T x = CALL !>;                       // route E to the scope handler
T x = CALL !>(e) { ...; return 1; }  // handle here; body diverges
T x = CALL !>(e) { log(e); @ok(v); } // handle here; body resumes with v
T x = CALL !>(e) { local(e); @err(e); } // local work, then the scope policy
T x = CALL ?> v;                     // sugar: !> { @ok(v); }
```

The body has exactly three exits: it diverges (`return`, `break`, `goto`,
`exit`), it re-routes with `@err(e)`, or it resumes with `@ok(v)`. A body
that reaches its end on any path without one of those is ill-formed at
expression position. `?> v` is the body with nothing before `@ok`. There
is one operator; the recipe `examples/recipe_unwrap_destroy_forms.ccs`
enumerates its products and says so in its header.

Absence opts in: a pointer-returning call may sit on the left of `!>`, and
the error type is the ambient one. There is no option type.

**Verdict: works.** `examples/serdes/json/tools/minify.shcc` is the
argument. Two `!>` on the two calls that touch the world; the Python twin
has zero and fails on a line you cannot identify by reading it. The cost
of visibility is two tokens.

### 1.2 Scope handlers are inlined templates keyed by type

`@errhandler(E e) BODY` at the top of a scope is the policy for bare `!>`
on error type `E` in that scope. Several may coexist, one per type.
Dispatch is exact type first, then a unique `as:` face path (spec, Results
invariant 4). Resolution is lexical and never crosses into a task.

At statement position a bare `!>;` runs the handler body and control falls
through to the next statement (spec, invariant 6). At expression position
the handler must diverge, because the site has no value to continue with.
A bare `!>;` on the same `E` inside the handler's own body is ill-formed.

The handler is not a target that is jumped to and abandoned. It is a body
that runs at the site, in the site's frame, before any cleanup, and
returns to the site's next statement. The lowering realizes that with
labels and gotos rather than by copying text, but the semantics are the
lexical ones: the handler sees the names visible at its declaration, and a
`return` inside it is a soft return through the site's ledger.

That makes it a resumable handler with no continuation. Mainstream
languages do not have one: exceptions unwind, `?` and `try` return,
effect systems capture a continuation to `resume`. Common Lisp's
`handler-bind` and Smalltalk's resumable exceptions are the precedent, and
both are dynamic. Here the continuation is the label after the site, the
installation is lexical scope, and the dispatch is the Result's error type.
All three are decided before the program runs.

**Verdict: works, and different.** It is a bit contrary. A reader from any
unwinding language will assume a handler that does not `return` is a bug.
That assumption should be met on the page (see 1.7).

**Proposed, design:** allow `@ok(v)` in a scope handler. Because the
handler is a template inlined at each site, `@ok(v)` is typed per site, as
an assignment would be, and discarded at a statement site. A site where
the conversion is ill-formed is diagnosed at the site with a note at the
handler. The scope handler exists to reduce typing; a value exit is the
one thing it cannot currently save the author from restating.

**Proposed, design:** a site-position pseudo-value available inside an
inlined handler, so a record-and-continue handler can attach the site's
span. The lowerer's own idiom (`ix->sym(nm) ?> NULL` plus "the caller
attaches the use site", `cc/lower/index.cch`) is exactly this handler with
the position added by hand at every call.

### 1.3 Required handling by type

A domain error type that has no `as:` face onto the common error cannot be
absorbed by a common-error handler. Any scope that uses bare `!>` on it
must bind a handler for that type or it does not compile. That is a
checked effect annotation: one line per scope, no signature pollution
unless the function chooses to pipe the type.

cctext is the specimen. `RtxIndexErr` (`core/piece_tree.cch`) has no face,
by design: "@errhandler must not merge with save/edit." The frontends bind
151 common-error handlers and 67 index-fault handlers, and functions bind
both at once with different policies. Nearly every index handler discards
the error and returns, with the comment that the fault is already latched
on the object. That is the convention that keeps required handling from
becoming Java's boilerplate: the information lives on the object, the
handler is a typed unwind, and its presence at the top of a draw function
is the signal that the function is fault-aware.

The criterion for a face, stated once: a specialized error gets a face when
its default consequence is "the operation failed"; it gets no face when it
signals a state change a failure handler would misread. By that rule the
seven stdlib faces (IO, print, Python, JS, QuickJS, the slice family) are
correct, and cctext's omission is correct. The faces are also load-bearing
for scripts: the script kind injects a common-error handler, and every bare
`!>` in `pydemo.shcc` / `jsdemo.shcc` / `qjsdemo.shcc` reaches it only
through a face.

**Verdict: works.** Two costs, neither an argument for fewer faces:

- A discarding handler on the common type is the CC spelling of an empty
  catch, and a face extends its reach to every foreign exception. cctext's
  discarding handlers are all on the narrow type. **doc:** a rule for
  the-cc-way.
- Redis (`real_projects/redis/redis_idiomatic.ccs`) declared the IO face
  and then projected `.base` in every binder. That is value-position
  widening, which `spec/draft_as.md` lists as arg-position autocast, not
  yet implemented. **unimplemented**, and partly **drift**.

### 1.4 The ledger

| Construct | Registers | Discharged on |
|-----------|-----------|---------------|
| `@defer stmt` | an obligation | every exit |
| `@defer name: stmt` | an obligation with a handle | every exit unless `@cancel_defer name` ran |
| `@defer(ok)` / `@defer(err)` | an obligation conditioned on the outcome | that kind of exit |
| `@destroy` / `@destroy { body }` | the type's destroy chain for this declaration | every exit after the declaration |
| `@detach` | nothing here; the owner outlives this scope | the caller's ledger |

`@destroy` binds to the declaration and to nothing else. It does not know
whether construction succeeded. The safe-teardown-on-failure behavior the
pages describe is two independent facts composing: a `!>` that leaves
never completes the declaration, so nothing was registered; and destroying
a dead object is a no-op, because every hooked type has a dead state. A
declaration whose value came back dead from plain C, checked with an
ordinary `if`, registers a destroy that does nothing:

```c
CCArena arena = make() @destroy { g_destroyed += 1; };
if (!cc_arena_is_live(arena)) return 1;
```

The outcome is a fact the ledger can see. C++ destructors do not know the
scope is failing. Go's defer inspects a named return. Zig has `errdefer`
and nothing for success. Rust's drop knows nothing. Here ok and err are
exits the ledger distinguishes, and a named cancel is the commit point at
the line where the commit happens (`examples/recipe_prepare_commit.ccs`).

Registration and exit share one lowering. The goto rewrite that makes a
`return` inside an inlined handler safe is the rewrite that runs the ledger
on every soft return, and a resume label is one more exit it already
reaches. Error operators and cleanup forms are not two features that
cooperate. They are one mechanism, and each line on the page names which
of its edges it takes.

**Verdict: works.** **doc:** "cleanup on successful declaration
construction" (getting-started, cheatsheet, language-concepts) names a
product as if it were the primitive. The primitive is "on the
declaration"; the product follows from `!>` and dead state.

### 1.5 What is different, precisely

| | Site visible | Policy visible | Resolution | Resume | Outcome-aware cleanup |
|--|--|--|--|--|--|
| Exceptions | no | far away | dynamic | no | destructors, blind |
| Go | yes | at every site | none | manual | defer, inspect return |
| Rust `?` | yes | implicit propagate; `From` widens | static | closure only | drop, blind |
| Zig `try` | yes | implicit propagate; sets merge | static | labeled block | `errdefer` only |
| CC | yes | per scope, per type | static, lexical, task-bounded | at the site, void or valued | ok / err / cancel / destroy |

The two cuts against the state of the art are: no implicit propagation and
no implicit widening. Propagate is one policy among exit, log, fail, and
resume, stated once per scope. Widening happens only where a face is
declared. In a server, propagate is not the dominant policy, so
privileging it is a CLI and library bias. Redis and staticd vary their
policy per scope; the one uniform file in the corpus is the redis database
module, where every function pipes, and eighteen one-line handlers is the
price of that module having no hidden default.

### 1.6 Evidence from the corpus

| Observation | Tag | Reading |
|-------------|-----|---------|
| pigz product: 22 handlers, 6 distinct policies | design | the policy varies by scope; the count is not restatement |
| redis database module: 18 handlers, 1 policy (propagate) | design | uniform pipe, stated locally; the honest price |
| staticd page: 17 handlers, 2 policies | design | works |
| cctext: 151 common + 67 index handlers, functions bind both | design | required handling by type, in production |
| redis `.base` in every binder | unimplemented | arg-position autocast through faces |
| pigz decompress uses a global atomic as the error channel | drift | predates `h.fail`; the local option was missing and the error became ambient state |
| cctext wait-for body swallows a read error into a cancel flag | design | a `return` in a ticket body is a ticket return; no spelling for "this ticket failed" |
| pigz error reporter needs its own handler to print | design | correct refusal of same-E re-entry; costs a scope for the commonest helper |
| staticd polls `.failed()` on templates at 12 sites | doc | growth failure poisons and never truncates; if the poison trips the next consumer the polling is redundant, and that guarantee should be pinned |
| lowerer: 0 handlers, `?> NULL` x12, bool + out-param walks | domain | diagnostics are record-and-continue; expressible as a template handler with `@ok` (1.2) |
| `@noblock` on idiomatic redis helpers outside any `@async` | theater | no-op per spec §8.2.1; vestigial from the owner variant |

### 1.7 What is theater, what is open

Theater:

- Teaching "two operators" (`?>` and `!>`). There is one form and one
  sugar. The recipe header already says so; the cheatsheet and
  language-concepts still lead with two.
- "Cleanup on successful construction" as a primitive (1.4).
- `@noblock` and `@nonblocking` outside `@async` bodies.
- The anti-pattern note about custom `E` to force handling is right, but it
  reads as a warning against the mechanism 1.3 depends on. It should say:
  a custom `E` without a face is how handling is required; a custom `E`
  with a face is only a payload.

Open, all **design**:

- `@ok(v)` in a scope handler (1.2).
- A site-position value inside an inlined handler (1.2).
- A spelling for "this ticket failed" inside a wait-for body.
- Whether widening should also be admissible at a handler, so a scope can
  accept a narrow type through its face while another scope refuses. The
  face is the one type-global piece of an otherwise scope-local model.
  cctext never needed it; redis wanted it.

Work, not debate:

- **unimplemented:** arg-position autocast through `as:` faces
  (`spec/draft_as.md`); `@defer(ok|err)` at block scope
  (`cc/lower/lower_cleanup.cch`); `'!> @destroy'` in the clean lowerer
  (`cc/lower/lower_results.cch` reports it twice).
- **test:** re-entry through a face, not only the exact type; one handler
  inlined at three sites with `int`, pointer, and struct destinations once
  `@ok` is admitted; a handler reached from a nested block whose ledger
  entries armed after the handler line.
- **doc:** the-cc-way should state that handlers resume, that resume means
  the next statement, that a discarding handler belongs on a narrow type,
  and the face criterion from 1.3 in one sentence.

---

## 2. Arena as lifetime, and the partial order

Not yet written in this file. Claims to carry in:

- An arena names a lifetime; storage policy is separate; slices carry
  which lifetime they belong to; lifetime parents make the lifetimes a
  tree. The parts/wholes reading is literal: adopt moves a part between
  wholes, the cycle refusal is proper parthood.
- Three bespoke passes (stack-slice escape, arena-epoch pin under spawn,
  pointer-alias capture) are edges of one order: frame < arena <
  nursery-or-dest < channel. The rule "a view's lifetime is an
  ancestor-or-equal of its holder's" unifies them, flips CVE-2023-54235
  from mitigated to prevented, shrinks SHAPE-T7, and gives the
  teardown-order check `spec/draft_lifetime_parents.md` says nothing
  enforces.
- Channels, nurseries, and closures predate arena-as-lifetime. The
  channel-family arena-per-block idiom is **drift**. The closure
  environment is the one holder with no lifetime story; it is the same
  work as the checker.
- Runtime handles (turnstile, parallel, exclusive) are not on the storage
  classes; specimens mint arenas and callocs to host them. **design.**

## 3. Join and job

Not yet written. `@parallel` is lexical fork-join and has become the
server pool and the background scan. cctext writes the same hundred lines
three times around a dest. The tickets plan's "one job each" is the axis;
the surface has not split along it. **design.**

## 4. Tagged data

Not yet written. `@variant` is the compiler's spine (510 switches) and the
construct with the most lowering holes; 503 of 510 switches carry
`default:` because of three of them. **defect.** Redis has three tagged
dialects. **design.** Comptime cannot name arms. **design.**

## 5. Instance placement

Not yet written. Monomorphs are emitted text spliced "once" with no defined
hoist point; a module member cannot typedef an instance; the lowerer
cannot spell a vec of pointers without a typedef. One fact, four
specimens. **design or unimplemented**, pending a model.

## 6. Comptime is semantics

Not yet written. Generics and UFCS registration run compile-time C, so the
evaluator is part of the language definition. The observation surface must
be pinned; determinism is a language requirement; the static index and a
running hook can disagree, and no specimen needed the computed-name path.
The sink is the one legitimately dynamic tier. **design.**

## 7. The runtime as guest

Not yet written. Curl, stylo, rlsw, and cctext each carry a sequential
twin or a suppress call because the progress source is outside the
scheduler. A declared host-fed wait belongs in the same who-waits-on-whom
relation that underlies deny, spawn, stage, and EMPTY-close. **design.**
`@async` is one specimen file and no `@await`; DESIGN.md's rule applies.

## 8. Receipts

Not yet written. The ladder is the freeze veto and its rungs drift: redis
wins only at deep pipelines; pigz is parity and went bimodal after a
runtime change; the levenshtein headline is the upstream number moving;
two pigz variants emit zero bytes and remain in `make test`. None of it
changes a concept. All of it weakens the veto.
