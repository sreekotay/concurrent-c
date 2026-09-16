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

## 2. Arena as lifetime

An arena names a lifetime. Everything else in this section is a
consequence of taking that literally: what each construct encodes about
lifetime, where the language currently encodes it twice, where it does
not encode it at all, and the one relation that would let every existing
check be an instance of a single rule.

### 2.1 What the constructs encode

| Construct | Fact | Where it is stated | Who acts on it |
|-----------|------|--------------------|----------------|
| `CCArena a = … @destroy` | a lifetime exists and ends here | the declaration | the ledger |
| `cc_arena_stack` / `heap` / `buf` / `malloc` | how storage for that lifetime is obtained | the constructor | the runtime; never the checker |
| `T[:]` with `id` | which lifetime these bytes belong to, and whether the view is unique, a sub-view, a grower, a C string | minted by the allocator | owners at run time; the own step at compile time, by name |
| arena-last parameter | the product lives on the caller's named lifetime | the signature and the call | the reader; `clone_into` |
| `@detach` | the owner outlives this scope | the declaration | the caller's ledger |
| `create_*(owner, …)` | this object is a part of `owner` and dies with it | the constructor name | the owner's teardown walk |
| `adopt` / `attach` | a part moves into, or is registered with, a whole | the call | the walk; the cycle refusal |
| checkpoint / restore | a mark on a lifetime; a child that restore frees | the binding | the runtime; views above the mark go stale |
| `T[:!]`, `recv`, `send_take` | exactly one name for these bytes | the type or the verb | the own step: copy, return, and use-after-move refused |
| `send_into` | the payload is built in the receiver's lifetime | the verb | the reader; materialization at the boundary |
| `@scratch` | these bytes die with the call statement | the operand | the own step: escape refused |
| capture into a spawn | a task holds this lifetime until join | the closure | the pin: epoch-ending operations refused while live |

Two things are true of this table. Every fact is stated at the position
the instruction guide asks for: the narrowest place it becomes known.
And the enforcement column is split between a static step that reads
names and a runtime token that owners check.

### 2.2 One fact, two encodings

The slice id carries the arena's epoch and generation plus flag bits.
Owners compare tokens: a stale handle cannot grow or destroy, and a reset
advances the epoch so a view minted before it no longer matches. That is
the dynamic encoding, and it is loud only where an owner is asked to act.
A plain view read through a stale pointer is not checked.

The own step is the static encoding. It reads a declaration's type and
initializer, records which named arena a local borrows from, and refuses
a free of a borrow, an epoch end under a pin, a reset with a borrow in
scope, a send of a non-stable view, a copy of a unique, an escape of
scratch. It tracks locals only. Parameters, field paths, index
expressions, and call results pass, by the rule that a wrong refusal
costs more than a missing one.

So the fact "these bytes belong to lifetime L" is encoded twice, and the
gap between the encodings is exactly the set of programs where a view
outlives its lifetime without an owner ever being asked to act on it.
That gap is where the remaining use-after-free lives.

### 2.3 The order

Every holder of a view has a lifetime of its own. A local's holder is its
frame. A field's holder is the object. A closure environment's holder is
whatever holds the closure. A channel's holder is unbounded. A task's
holder is the join set it belongs to.

Lifetimes form a partial order by containment: the frame is inside the
scope that declared its arenas; a child arena is inside its parent; a
task is inside the join set that waits for it; a channel is inside
nothing. Write `L1 <= L2` when `L1` ends no later than `L2`.

One rule: **a view may be held by `H` only if the view's lifetime is
greater than or equal to `H`'s.**

Every existing check is an instance:

| Existing rule | Instance of the order |
|---------------|-----------------------|
| stack slice cannot escape into a closure that outlives the frame | frame < closure's holder |
| arena epoch pinned under spawn; reset refused while pinned | task <= join set, so the arena must be >= the join set until join |
| reset refused with a borrow in scope | the borrow's frame <= the arena's current epoch |
| channel send of a non-unique view refused | channel is unbounded; only a lifetime that moves with the payload qualifies: unique, static, or an arena riding in the message |
| aggregate send checked field-wise | each field is a view with its own holder |
| pointer-alias capture mutation refused | the alias's frame < the task |
| `@scratch` cannot be returned or captured | the call statement < the frame |
| child-free ban | a borrow's holder may not end its lifetime |
| teardown order of parts, newest first | parts registered later are <= parts registered earlier; a waiter attached before its resource inverts the order and is refusable |

The last row is the check the lifetime-parents design leaves to
convention. Under the order it is a comparison, not a walk.

The rule also unifies what the CVE study scores separately. A stack work
struct captured by reference into a nursery declared in an outer scope is
frame < join set. A bare pointer on a channel whose pointee is arena or
frame memory is arena < unbounded. What the order cannot decide is what
no lifetime rule can: untracked heap, foreign memory, bounds, arithmetic.

### 2.4 What the order needs that is not on the page

Three holders have no lifetime story today.

**Parameter views.** A callee that receives `char[:] src` knows only that
the bytes outlive the call. Storing that view into the receiver, a field,
or an arena-held object is the one place the checker is silent and the
instruction guide answers with a convention: materialize at the boundary,
clone into the arena the caller named last. The convention is right and
it is not checkable, because the callee cannot compare a lifetime it
does not know.

The fact that is missing is the callee's: this parameter is kept. The
narrowest place it becomes known is the signature. Stated there, the
comparison moves to the call site, where both lifetimes are locals and
the order decides. That is one attribute on a parameter, no lifetime
variables, no inference. A callee that keeps a view without saying so is
then the refusable shape.

**Closure environments.** A closure's environment has the lifetime of
whatever holds the closure. The closure step already computes this in
the negative: a closure escapes the frame if returned, assigned to a
member, or passed to a call that is not a scoped spawn. Under the order
the environment is a holder like any other, its lifetime is that of its
own holder, and the capture table is derived rather than enumerated.
Channels, nurseries, and closures predate arena-as-lifetime; this is the
re-derivation that makes them one model.

**Runtime handles.** A turnstile, a parallel handle, an exclusive, a pool
are objects with interior pointers and a dead state. The lifetime-parents
design already says what such objects are: parts born into a whole by a
`create_*` constructor, never moved by copy. Today only nurseries and
pools have that constructor; the rest are declared by value and the
program mints an arena, or a heap allocation, to host them. Extending
`create_*` to every hooked runtime type is not a new idea. It is the
existing storage-class rule applied to the types that were built before
it.

### 2.5 The mereology, stated once

Parthood is the relation the tree encodes, and it inherits exactly one
consequence: a whole destroys its parts, newest first, before releasing
its own storage. Nothing else is inherited. A part is not cancelled
because its whole is; a view is not owned because its holder is; a
container is not a parent because it embeds an arena unless it exposes
that arena as a face. The cycle refusal in `adopt` is the statement that
parthood is proper. The `Region` face on a nursery is the statement that
a part may stand in for its whole for one authority, allocation, and no
other.

That discipline is the instruction guide's "relations do not inherit
consequences" applied to the one relation that must inherit one.

### 2.6 Surface that does no work, and what is open

- Three arena constructors that share one engine and differ only in
  where L1 lives are taught as three ideas. They are one lifetime and
  three storage policies; the pages should say so in that order.
- "Provenance" is used for the runtime token and for the static fact.
  They are the same fact and should share a name, or the pages should say
  which one each rule reads.

Open:

- The kept-parameter attribute: its spelling, and whether a keep of a
  parameter without it is an error or a warning during the transition.
- Whether the runtime token should also guard view reads in debug builds,
  so the dynamic encoding covers the gap until the static one does.
- Whether a checkpoint's stale views, which the token already detects,
  should be a compile-time refusal when the view and the mark are both
  locals. That is the same order applied to a mark instead of an arena.

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
