# Design review, 2026-09

Concepts and ideas, argued. Not spec: spec is normative and carries no
reasoning; this file is reasoning. Where they disagree, spec wins.
Evidence from the corpus lives in `DESIGN_REVIEW_2026_09_EVIDENCE.md`.

Sections are ordered by what a reader meets first in a program.

---

## 1. The local system

Errors, cleanup, and resumption are one mechanism. Every line that can
fail says so. Every obligation is registered where it arises. Every exit
discharges the same ledger. A failure handled in scope resumes at the next
statement. Nothing is resolved at run time.

### 1.1 One form at the site

```c
T x = CALL !>;                            // route E to the scope handler
T x = CALL !>(e) { ...; return 1; }       // handle here; diverge
T x = CALL !>(e) { log(e); @ok(v); }      // handle here; resume with v
T x = CALL !>(e) { local(e); @err(e); }   // local work, then the scope policy
T x = CALL ?> v;                          // sugar for !> { @ok(v); }
```

A body has three exits: diverge, re-route with `@err(e)`, or resume with
`@ok(v)`. A body that can reach its end without one is ill-formed at
expression position. `?> v` is the body with nothing before `@ok`. There
is one operator.

Absence opts in: a pointer-returning call may sit on the left of `!>`;
its error type is the ambient one. There is no option type.

### 1.2 Scope handlers

`@errhandler(E e) BODY` at the top of a scope is the policy for bare `!>`
on error type `E` in that scope. Several coexist, one per type. Dispatch
is the exact type, else a unique `as:` face path. Resolution is lexical
and never crosses into a task.

At statement position a bare `!>;` runs the handler and control falls
through to the next statement. At expression position the handler must
diverge: the site has no value to continue with. A bare `!>;` on the same
`E` inside the handler's own body is ill-formed.

The handler is a body that runs at the site, in the site's frame, before
any cleanup, and returns to the site's next statement. The lowering uses
labels and jumps; the semantics are the lexical ones. The handler sees the
names visible at its declaration. A `return` inside it is a soft return
through the site's ledger.

This is a resumable handler with no continuation. Exceptions unwind; `?`
and `try` return; effect systems capture a continuation to resume.
Common Lisp's `handler-bind` and Smalltalk's resumable exceptions are the
precedent, and both are dynamic. Here the continuation is the label after
the site, the installation is lexical scope, and the dispatch is the
Result's error type. All three are decided before the program runs.

**Proposed.** `@ok(v)` in a scope handler. The handler is a template
inlined per site, so `@ok(v)` is typed per site, as an assignment would
be, and discarded at a statement site. An ill-formed conversion is
diagnosed at the site with a note at the handler. The scope handler exists
to reduce typing; a value exit is the one thing it cannot yet save the
author from restating.

**Proposed.** A site-position value inside an inlined handler, so a
record-and-continue handler can attach the site's span.

### 1.3 Required handling by type

A domain error type without an `as:` face onto the common error cannot be
absorbed by a common-error handler. A scope that uses bare `!>` on it must
bind a handler for that type or it does not compile. That is a checked
effect annotation: one line per scope, no signature pollution unless the
function chooses to pipe the type.

The convention that keeps this cheap: the fact lives on the object, the
handler is a typed unwind, and its presence at the top of a function is
the signal that the function is fault-aware.

The criterion for a face: a specialized error gets a face when its default
consequence is "the operation failed"; it gets no face when it signals a
state change a failure handler would misread.

Two costs. A discarding handler on the common type is the CC spelling of
an empty catch, and a face extends its reach; discarding handlers belong
on narrow types. And widening is the one type-global piece of an
otherwise scope-local model.

**Open.** Whether widening may also be admitted at a handler, so one scope
accepts a narrow type through its face while another refuses.

### 1.4 The ledger

| Construct | Registers | Discharged on |
|-----------|-----------|---------------|
| `@defer stmt` | an obligation | every exit |
| `@defer name: stmt` | an obligation with a handle | every exit unless `@cancel_defer name` ran |
| `@defer(ok)` / `@defer(err)` | an obligation conditioned on the outcome | that kind of exit |
| `@destroy` / `@destroy { body }` | the type's destroy chain for this declaration | every exit after the declaration |
| `@detach` | nothing here; the owner outlives this scope | the caller's ledger |

`@destroy` binds to the declaration and nothing else. It does not know
whether construction succeeded. Safe teardown on failure is two
independent facts composing: a `!>` that leaves never completes the
declaration, so nothing is registered; and destroying a dead object is a
no-op, because every hooked type has a dead state. A declaration whose
value came back dead from plain C, checked with an ordinary `if`,
registers a destroy that does nothing.

The outcome is a fact the ledger sees. Destructors are blind to it. Go's
defer inspects a return. Zig has `errdefer` and nothing for success. Here
ok and err are exits the ledger distinguishes, and a named cancel is the
commit point at the line where the commit happens.

Registration and exit share one lowering. The jump that makes a `return`
inside an inlined handler safe is the jump that runs the ledger on every
soft return, and a resume label is one more exit it reaches. Error
operators and cleanup forms are one mechanism; each line names which of
its edges it takes.

### 1.5 The cuts

| | Site visible | Policy visible | Resolution | Resume | Outcome-aware cleanup |
|--|--|--|--|--|--|
| Exceptions | no | far away | dynamic | no | blind |
| Go | yes | at every site | none | manual | inspect return |
| Rust `?` | yes | implicit propagate; `From` widens | static | closure only | blind |
| Zig `try` | yes | implicit propagate; sets merge | static | labeled block | err only |
| CC | yes | per scope, per type | static, lexical, task-bounded | at the site, void or valued | ok / err / cancel / destroy |

No implicit propagation: propagate is one policy among exit, log, fail,
and resume, stated once per scope. No implicit widening: it happens only
where a face is declared. In a server, propagate is not the dominant
policy; privileging it is a library bias.

### 1.6 Surface that does no work

- Teaching two operators. There is one form and one sugar.
- "Cleanup on successful construction" as a primitive. The primitive is
  "on the declaration"; the product follows from `!>` and dead state.
- `@noblock` and `@nonblocking` outside `@async` bodies.
- Warning against a custom `E` as if it were a mistake. A custom `E`
  without a face is how handling is required; with a face it is a payload.

---

## 2. Arena as lifetime, and the partial order

An arena names a lifetime. Storage policy is separate. A slice carries the
lifetime it belongs to. Lifetime parents make lifetimes a tree; adopt
moves a part between wholes, and the cycle refusal is proper parthood.

Three checks exist as separate passes: a stack slice escaping into a
closure, an arena epoch pinned under a spawn, a pointer alias captured by
reference. They are edges of one order: frame < arena < join set <
channel. One rule covers them: a view's lifetime is an ancestor-or-equal
of its holder's. The same rule gives the teardown-order check that
lifetime parents currently leave to convention.

Channels, nurseries, and closures predate arena-as-lifetime. Re-deriving
them from it is the same work as the checker: the closure environment is
the one holder with no lifetime story.

Runtime handles are not on the storage classes. A turnstile, a parallel
handle, an exclusive should be born into an owner the way a child arena
is, not hosted by an arena minted for the purpose.

## 3. Join and job

`@parallel` is lexical fork-join and has also become the server pool and
the background scan. Those want done distinct from live, pause honored at
a stage, a placeable handle, and a public cancel. The axis is one job
each: join, range, stream, bag. The surface has not split along it.

## 4. Tagged data

`@variant` is the spine of the compiler and the construct with the most
lowering holes. Exhaustiveness is forfeited wherever `default:` is forced
by a hole. There are three tagged-data dialects: variants, hand `{kind, u}`
structs, and grammar-generated unions. One projection protocol should
serve all three, and comptime should be able to name arms.

## 5. Instance placement

Monomorphs are emitted text spliced once, with no defined hoist point. A
module member cannot typedef an instance; a vec of pointers needs a
typedef; an instance cannot appear in a prototype. Instance declarations
should be index facts with a defined placement.

## 6. Comptime is semantics

Generics and UFCS registration run compile-time C, so the evaluator is
part of the language definition. The observation surface must be pinned.
Determinism is a language requirement. The static index and a running
hook can disagree; the hook language should be narrowed to what the index
can read, leaving the sink as the one dynamic tier.

## 7. The runtime as guest

When the progress source is outside the scheduler, programs carry a
sequential twin or a suppress call. A declared host-fed wait belongs in
the same who-waits-on-whom relation that underlies deny, spawn, stage, and
EMPTY-close. `@async` is unearned by the corpus; the removal rule applies
unless a specimen re-earns it.
