# Design review, 2026-09

Concepts and ideas, argued. Not spec: spec is normative and carries no
reasoning; this file is reasoning. Where they disagree, spec wins.
Evidence from the corpus lives in `DESIGN_REVIEW_2026_09_EVIDENCE.md`.

Sections are ordered by what a reader meets first in a program.

The goal is one path: tutorial, idiomatic, performant, and production
are the same code, and it is safe.

This review is the discovery of a doctrine and its measurements, not
their defense. The position below was found by the counts in the
evidence file; the terms after it are what those counts turned out to
measure.

Memory safety is a consequence of a program stating its facts, not a
proof layer beside them. A lifetime parameter, a reference-counted
cell, a lock wrapper, an unsafe block: each is text that carries no
program logic, and text that carries no logic is where logic hides.
Memory is one of the facts a program states, with outcome, authority,
and join set; ownership is one of three seams a bug crosses, with wire
truth and data model. A stated fact is checked strictly and refused
loudly. An unstated fact is never inferred. The failure mode is the
unstated fact, so safety widens by widening what can be stated.

A stated fact has up to three consumers. The compiler refuses the
shapes that contradict it. The runtime checks it where the check rides
on work already paid. And a relation the compiler cannot prove is
checked by execution: the statement generates the test that exercises
it. The language already does the third once: `seq (cond)` states that
the parallel schedule and the sequential one produce the same result,
and flipping the flag is the differential test. Bend proves its laws;
CC runs them. Either way the law is written once, and here it is the
declaration itself, so it costs nothing to author. A fact with no
consumer is surface that does no work.

The anti-pattern is reconstruction: a language gives a fact no place to
be stated, then adds machinery to recover an approximation of it.

| Reconstruction | Recovers | Stated in CC by |
|----------------|----------|-----------------|
| tracing GC | when this dies | the arena binding; reset or destroy is the program's own event |
| reference counting, as ownership | who may destroy this, recovered by counting claimants | one named owner; claims with stated ends; the tree destroys parts. As a claim counter it is legitimate: each claim is a scope-bound guard, only their order is counted |
| lifetime inference | which storage a reference points into | the named arena, arena-last, the kept attribute |
| escape analysis | whether a value outlives the call | scratch or the caller's lifetime; the closure step checks the statement |
| handler search, unwinding | who handles this failure | the lexical handler, keyed by type |
| RTTI, reflection | what this value is now | the variant arm, the face |
| vtable dispatch | which function this name means | static resolution; one declared dynamic sink |
| deadlock detection, wake heuristics | who waits on whom | spawn, stage, EMPTY-close, deadline; the detector covers the rest |

Reconstruction is legitimate exactly when no party can state the fact:
which of several concurrent claimants finishes last is one such fact,
and counting is the honest way to learn it. Every other reconstruction
remaining in the runtime or the checker is a proposal waiting to be
written, scored on the terms below. The last row
is unfinished and is section 3's subject.

Safe is measured on five terms, not on soundness:

| Term | Measures | Instrument |
|------|----------|------------|
| coverage | how much of a program runs on the checked surface | peels per thousand lines |
| precision | how often a correct program is refused | false refusals per rule, notes before errors |
| reach | which bug shapes are ill-formed, mitigated, or expressible | the CVE study |
| one path | whether the tutorial form is the production form | the specimen ladder |
| cost | what a check adds at run time | measured against the bump, the walk, the verbs |

A proposal that moves none of them is dropped. The terms are provisional
in the same way the doctrine is: a later section may add one or retire
one.

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

**Decided, seeds 412 to 414.** `CCIoError` has no face. An I/O Result
reaches an `@errhandler(CCIoError)` or nothing. A scope that answers
with `CCError` converts where it chooses to, `e.base`, and the other
direction is `cc_io_error(e)`. No error type in the tree widens
implicitly now, so the one type-global piece is gone and widening is a
line at a site. The cost was paid in the teaching code: about
twenty-five handler lines became `@errhandler(CCIoError e)
cc_error_exit(e.base)`, and a scope that meets both kinds writes two
handlers. A handler naming two types on one line remains available if
that pair proves common. Two rules landed with it. A handler no unwrap
in its scope reaches is refused: it says the scope can fail that way,
and it cannot. A handler a nearer one shadows is allowed with a
warning. And an unwrap through a macro that declares no `E` dispatches
to the innermost `CCError` handler, never to the last one declared.

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
- A handler for an error nothing in its scope raises. Refused since
  seed 412.

---

## 2. Arena as lifetime

### 2.1 Status

An arena names a lifetime. Storage policy is a constructor argument. A
slice carries the lifetime its bytes belong to. Arenas form a tree
through lifetime parents. A function never chooses a lifetime for a
product: it borrows scratch for the call, or writes into the arena the
caller passed last.

| Construct | Fact | Stated at | Enforced by |
|-----------|------|-----------|-------------|
| `CCArena a = … @destroy` | a lifetime exists and ends here | the declaration | the ledger |
| `cc_arena_heap` / `stack` / `buf` / `malloc` | how storage is obtained | the constructor | the runtime only |
| `T[:]` with `id` | which lifetime these bytes belong to; unique, sub-view, grower, C-string | the allocator | owners at run time; the own step at compile time |
| arena-last parameter | the product lives on the caller's lifetime | the signature | convention |
| `@scratch` | one stack arena per function; a bound product lives to the frame, an unbound one is reclaimed after its statement | whether the product has a name | escape refused; tip restore after the statement |
| `@detach` | the owner outlives this scope | the declaration | the caller's ledger |
| `create_*(owner, …)` / `adopt` / `attach` | a part of a whole; dies with it | the constructor or call | the teardown walk; cycle refusal |
| checkpoint / restore | a child lifetime, materialized lazily: a mark until it needs a host | the binding | the runtime; views above the mark go stale |
| `T[:!]`, `recv`, `send_take` | one name for these bytes | the type or verb | copy, return, use-after-move refused |
| `send_into` | the payload is built in the receiver's lifetime | the verb | convention |
| capture into a spawn | a task holds this lifetime until join | the closure | epoch-ending ops refused while pinned |

Lifetimes below the caller's named one: an unbound scratch product ends
after its statement; a bound one at the frame; a frame arena at scope
exit. Registration order is release order, so error defers on a bump
arena unwind exactly; the failure path needs no mechanism.

Ownership is the ability to destroy, and exactly one thing has it. A
claim delays that end and always states when the delay stops: a pin
until join, a hold until release, a checkpoint until restore. Access
neither owns nor claims. A refcount as ownership is an anonymous owner
plus counted claims; as a claim counter it is the right tool when
claimants are concurrent, must finish, and must not block the owner, and
then each claim is still stated and only their order is counted.
Otherwise the owner is named and the owner waits. Where a claimant may
lose the race, the token refuses instead of delaying.

The boundary of that rule: claims with stated ends cost either retention
until the epoch ends or head-of-line blocking on the owner, and a claim
with no bound hangs the owner rather than leaking. A refcount pays
neither and charges an anonymous destroyer. Where an owner must make
progress independently of a dynamic set of long readers, the tools here
are a copy per reader, an epoch per generation with the owner parked
until quiescence, or blocking; none is free. The corpus uses no refcount
and was chosen from problems that already have an owner; a shared-graph
problem with churn has not been run. Token refusal covers writers today
and readers only once the verbs check.

Scope carries most ownership in pipeline- and request-shaped programs,
about half in a server, and almost none in a long-lived object model,
where teardown is a hand-written destroy function.

A struct is already a parent of its value fields: the destroy chain
walks them last-declared first, transitively, with dead-state no-ops,
the same order as the arena walk. Declaration order is the dependency
order. What a struct is not, is a part of the tree: its own owner does
not tear it down without a call. That half exists only for arenas,
through attach and `create_*`. Creation, placement, and ending are
separate annotations; `name@(args)` says nothing about ending and must be
followed by `@destroy` or `@detach`. The attach primitive already exists;
the spelling that hands a hooked value's obligation to a named owner
does not.

Parthood inherits one consequence: a whole destroys its parts, newest
first, before releasing its own storage. Nothing else crosses the
relation. The `Region` face lets a part stand in for its whole for
allocation and no other authority.

The lifetime fact is encoded twice. The slice id carries the arena's
epoch and generation; owners compare tokens, and a stale handle cannot
grow or destroy. The own step records which named arena a local borrows
from and refuses by name. Neither checks a plain read through a view.

The own step tracks locals only. It reads C spellings and runs before
UFCS resolution, so `a.reset()` is unchecked and `cc_arena_reset(a)` is
checked. Restore and detach are not in its table. Views through a field,
a call result, an unwrap, a kept parameter, a dest late-admit, or an
arena held in a field are unchecked. A stack buffer captured into a
nursery from an outer scope, or followed by `leave`, is unchecked. A
send of an aggregate carrying an arena does not consume the sender's
binding. A copied handle survives the host's free and dereferences it.
Reset after last use is refused by scope, not liveness.

### 2.2 Proposal

Every holder of a view has a lifetime: a local's frame, a field's
object, a closure's holder, a task's join set, a channel (unbounded).
Lifetimes are ordered by containment. One rule: **a view may be held by
`H` only if the view's lifetime is greater than or equal to `H`'s.**

The checker refuses shapes; it does not prove absence. Aliasing through
a pointer, an array of views, a copied aggregate, or a pointer parameter
is outside it. Every refusal is a real bug. What it misses, the token
catches where an owner acts. What it trades in proof it earns back in
clarity: a refusal names a view, the arena it came from, and the line
that ended the arena, all inside the function the reader is in, with no
inference chain and no annotation on a path that only passes a view
through. The checker has the locality of the handlers, for the same
reason. The order is total within a frame and
across join sets; a view stored into shared state and read by another
fiber after a reset is a temporal question it does not see, and rests on
the token and the hold discipline.

Each existing check is an instance:

| Existing rule | Instance |
|---------------|----------|
| stack slice cannot escape a closure that outlives the frame | frame < closure's holder |
| epoch pinned under spawn; reset refused | task ≤ join set ≤ arena |
| reset refused with a borrow in scope | borrow's frame ≤ epoch |
| channel send of a non-unique view refused | channel is unbounded; only unique, static, or an arena riding with the message qualifies |
| aggregate send checked per field | each field is a view with its holder |
| pointer-alias capture mutation refused | alias's frame < task |
| scratch cannot be returned or captured | frame < any outside holder |
| child-free ban | a borrow's holder may not end its lifetime |
| teardown newest first | later parts ≤ earlier parts; a waiter attached before its resource is refusable |

The rule reaches the holders the step does not see, with these facts:

1. **Arena-last is inferred, as a minimum.** A call whose last argument
   is an arena returns a view that lives no longer than that arena or
   any view argument. Ending any of them while the result is live is
   refused. This is conservative: a callee that did not use the arena
   for its result still pins it. The convention makes that rare.
2. **Field paths of local values are names.** `d.store` is a lifetime
   while `d` is; a view stored into `d.body` is held by `d`. Handles are
   copyable, so names alias: `CCArena b = a` and `m.arena = b` join one
   alias set, and an epoch end through any member counts for all. Paths
   through a pointer parameter stay the caller's to state.
3. **A kept parameter is declared.** One attribute on a parameter or a
   field: this view is kept, by default for the receiver's or object's
   lifetime, else for the named parameter's or for static. The
   comparison moves to the call site, where both lifetimes are locals.
   Lifetime targets are parameter names; there are no lifetime
   parameters, no generics over them, no variance, and nothing on paths
   that only pass a view through.
4. **A dest is a join set.** Late admit pins like spawn.
5. **Scoped spawn compares scopes.** The join set's scope must lie
   inside the captured lifetime's scope; `leave` is an escape.
6. **Crossing into the flow is a move.** A send that carries an owner
   detaches it from the sender's tree; the sender's name is dead
   afterward, and a receive adopts or takes it. Provenance keeps views
   on the graph safe; this keeps parthood from being claimed by two
   trees.
7. **Copying an owner handle without a move is refusable** (compile
   time), and a stale copy refuses at run time (2.3). A storage-bound
   child's host is carved from its parent's slab and dies with it; a
   stale handle to one is outside the runtime half.
8. **The obligation may go to a named owner.** Three facts stay
   separate: how a value comes to be (`name@(args)` or any expression),
   where it is placed (a scope value, an owner's storage, its own heap),
   and who ends it (`@destroy` this scope, `@detach` the caller). The
   third has no spelling for "a named owner" except the arena-only
   `create_*` family, which also fixes placement. One annotation for any
   hooked value, attaching a record whose function is the type's destroy
   chain, gives turnstile, parallel, exclusive, and a document created
   into a workspace a holder with no call on the page, without
   conjoining creation, placement, and ending.
9. **The own step learns the taught spelling.** It keeps reading the
   program as written, so its messages quote what the user wrote, and it
   recognizes a method on a receiver the index types as an arena as the
   same fact as the C name: `reset`, `alloc_slice_bytes`, `restore`,
   `detach`. Restore, try_restore, and detach join the epoch-ending
   table. This costs no clarity; it is a vocabulary hole, not a trade.

10. **An arena-last parameter is the `Alloc` face by default.** A callee
    may add bytes to the caller's lifetime and may not reset, restore,
    detach, or destroy it. Faces are erased; the cost is zero. This
    makes the failure path structural and the inference in item 1 safe
    against the callee ending the lifetime it was handed.

Diagnostics name both ends: the view's lifetime, the holder's, and which
ends first.

Each new refusal lands first as a note that does not fail the build,
runs across the specimens and the compiler's own sources, and becomes an
error when its false-refusal count is known. A wrong refusal costs more
than a missing one; this is the procedure that honors it.

Not covered by any lifetime rule: untracked heap, foreign memory,
bounds, arithmetic.

### 2.3 Cost

Items 1 to 6 and 9 are compile-time only. They change what the own and
closure steps refuse. The emitted C, the runtime, and the ABI are
unchanged, and the steps stay linear in names and statements.

Item 7 has a free half and a paid half. Refusing a copy of an owner
handle without a move is compile-time. Making a stale copy refuse at run
time is not, and there are two ways to pay:

| Option | Runtime | Memory | ABI |
|--------|---------|--------|-----|
| hosts never unmapped, no generation | none on alloc; a stale copy reads a dead host and `is_live` is false | freed hosts are retained and reused, like owner headers; a heap arena's host must be allocated apart from its region, one more malloc per heap arena | none; ABA remains: a stale copy can see a reborn host |
| generation as a tag above the 48-bit pointer | one mask per host peel; one compare in `is_live` | same retention | 8 bytes, but fragile: 57-bit addressing, ARM top-byte-ignore, and pointer authentication all use those bits |
| generation with an index handle: 32-bit index into a host table plus 32-bit generation | one dependent load per host peel on the alloc path | same retention plus the table | 8 bytes on both widths; no pointer in the handle; host C that peels `.a` / `.p` breaks |

Oldest-first reuse of hosts defers ABA to many arenas later without a
generation. Take that now; the index handle is the upgrade if ABA is
ever observed. The static half alone covers every copy the checker can
see; the runtime half covers copies that reach a field or a parameter.

Item 8 is cheaper than today. The handle is a bump from the owner plus
one attach record, and it dies in the owner's walk. Today a turnstile is
hosted by an arena minted for it, and a parallel handle by a calloc.

Open items with a cost:

- Promotion versus spill: promotion mallocs a host when scratch outgrows
  a slab under a mark; spill would malloc per pre-mark regrow instead.
  Which is rarer depends on the workload. Neither is on the fast path.
- The 32-bit grower epoch has no free fix. Widening the id makes every
  slice 32 bytes instead of 24. Moving the generation out of the id
  loses stale-owner detection on grower views. Drawing epochs less often
  weakens restore staleness. This is a trade to choose, not a repair.
- Pinning a mark instead of the arena, last-use liveness, and the kept
  attribute are compile-time only.

**Principle.** A build mode changes speed and diagnostics, never layout
and never which checks run. One ABI, one meaning. The spec passages that
promise a debug-only trap resolve to always or never.

**Baked in.** A runtime check is admitted where it rides on work already
paid at that site:

| Paid operation | Already reads | Also decides, at no new access |
|----------------|---------------|--------------------------------|
| point store | bounds, grower generation | nothing new; the model |
| point load | bounds | the same generation bit |
| walk entry | `.len` and pointer, once | the generation bit, once per loop |
| release, realloc | the slab chain, to find the owner | the epoch of the bytes at that pointer; a stale or aliased release refuses |
| every alloc | the current slab, tested for NULL | a stale handle, once freed hosts are not unmapped |
| arena-last verbs taking a view and an arena | both operands | a stale self-view: the view's epoch against the arena's current one |

The bump and the walk body stay untouched. Hosts come from a pool by a
bump instead of sharing the region's malloc, reused oldest-first, so
every existing liveness test detects a stale handle and ABA is deferred
without a generation.

**Not admitted.** A view finding its host with nothing else in hand
needs a registry lookup per check. By the principle it would be
always-on; most of what it catches is already caught where an arena is
in hand. It is dropped. A runtime kept-parameter check needs the same
lookup; that fact belongs to the compiler.

### 2.4 Open

- The kept-parameter spelling, and whether an undeclared keep is an
  error or a warning during transition.
- Last-use liveness within a block, so a reset after the last use of a
  view is not refused.
- Pinning a mark instead of the whole arena when a task holds a view
  minted above a checkpoint.
- Whether pre-mark regrow under a checkpoint should spill to per-object
  overflow with the root epoch instead of promoting.
- The 32-bit grower epoch drawn from one global counter.
- A germ: a peel that is returned. `.ptr` as a scoped claim with a
  stated end, so an epoch cannot end while a peel is out and a peel that
  outlives its block is the refusable shape. Lexical only; a counted
  peel would be a refcount on views.

### 2.5 The Gap

Safety is three layers, each with one job:

| Layer | Checks | Where |
|-------|--------|-------|
| verbs | bounds, extent snapshot, generation | every access that is not a peel |
| token | a stale handle cannot grow, release, or destroy | every owner operation |
| order | a view is not kept past its lifetime | before the program runs |

`.ptr` is the one exit from the first layer. It is legal C, it is
visible, and it is counted. The lever on the first layer is needing it
less. What forces it today, in order of weight:

1. A helper declared with a pointer and a length, called with a peel
   at every site. The signature becomes a slice; a note on any
   pointer-and-length parameter pair where a slice would do makes the
   rewrite mechanical. This is the largest share and it is user code,
   not the stdlib.
2. Scanning with an index: skip, find, look back, look ahead. Most of it
   is a verb that exists and was not reached for. The rest is a cursor:
   peek, take-while, skip-while, rest, as a view that advances.
3. A null check on a slice, because allocation returns an empty slice on
   failure. Failure must not look like an empty success; allocation is a
   Result, for the slice form and the pointer form. The C twins stay
   pointer-returning. Measured: no cost on the shared or the local bump.
4. Copying into a raw buffer held in a struct. The buffer is a slice
   window; dest-bulk applies.
5. Reinterpreting bytes as a record, or decoding them by hand. A checked
   typed view over bytes, or the grammar.
6. Stdlib signatures that take pointer plus length. Slice-taking twins.

Together these remove about nine in ten peels. What remains is identity
comparison and calls into C libraries, which is the returned peel's
territory.

Numeric and handle code peels nowhere. Text processing peels most. The
Gap is a text-processing gap, and the largest part of it is the arena
API contradicting the rule that failure is never an empty success.

### 2.6 Surface that does no work

- Three constructors taught as three ideas; they are one lifetime and
  three storage policies.
- "Provenance" used for both the runtime token and the static fact.
- `@scratch(N)` taught as a size; it is the root, and larger products
  spill and are still reclaimed.
- The spec's capture example shows a stack slice into a same-frame
  nursery as an error; the closure step accepts it, correctly.
- `CCArc` taught as shared ownership. It is a claim counter: the owner is
  the count's home, each claim a scope-bound guard, the last guard out
  runs the teardown. Its trigger is claimants that are concurrent, must
  finish, and must not block the owner. No specimen has that shape yet.
  The own step's diagnostic for a hand-rolled last-drop should name that
  trigger, and name the parent tree or `acquire_when` otherwise.

## 3. Join and job

A join set is a lifetime for work, the way an arena is a lifetime for
bytes. The fact it states is who waits on whom: which fibers this
statement will not pass until they are done, in what order they hand
state to each other, and what closes when the last one dies. Two things
have grown on the same construct: a wave, which is a join that ends at
its statement, and a job, which spans many waves and ends with the
object that owns it. The surface spells the wave and the program
reconstructs the job beside it.

### 3.1 Status

| Construct | Fact | Stated at | Enforced by |
|-----------|------|-----------|-------------|
| `@parallel { a = f(); b = g(); }` | these arms are independent and all done here; an arm's unhandled unwrap crosses the join as `CCError` | the brace | the join; racing arms undefined |
| `@parallel (pred)` / `seq (cond)` / `#pragma(@parallel) off` | the schedule below this cut is sequential | the predicate or the directive | one body, two lowerings |
| `@parallel for (i in lo..hi)` | iterations are independent | the range | bisection; `break` as a shared stop |
| `@parallel wait (ts) for` + `@stage (gate, name…)` | at most `cap` tickets in flight; this block runs after the name it waits on is passed and before it passes its own | the cap, the gate, the name | a named one-shot cell, two touchers; a failed pass wakes the waiter `err` |
| `cache (name)` | this local is per-ticket scratch | the clause | one instance per runner slot |
| `CCParallel h = @parallel {…}`, `@parallel(h) {…}` | this work belongs to that join set; it may outlive the frame | the bind, the admit | closure capture rules; admit after join aborts |
| `h.close(tx)` / `n.close(tx)` | this stream ends when this join set is empty | the registration | EMPTY, on both paths |
| `h.adopt(h2)` | cancel reaches down here | the call | the cancel walk; not a join |
| `h.fail(e)` | this join set's outcome | the body | first error wins; `wait` returns it |
| `h.cancel()`, `h.pause()` | stop or hold at the next seam | the call | honor at thunk start, half, leaf, enter, ticket, stage |
| `n.spawn(() => …)`, `n.leave(ctx, finish)` | a child of this owner; the owner is gone, run this at EMPTY | the nursery | never denied; leftover on the LEFT path |
| `@with_deadline(...) as dl` | the clock these arms share | the name | spawned arms see no caller clock unless named |

The stage is a name, not an index. Underneath the turnstile is one
exclusive map of gate cells keyed by a 64-bit name: whichever of `wait`
or `pass` touches a name first creates the cell, the second completes
it and frees it, so live cells stay at the in-flight count and the
program never holds a graph. The turnstile is a depth channel plus one
name base per gate, and its only rule is that `pass(n)` touches `n+1`.
`@stage (gate, k, expr)` feeds the same expressions to the wait and the
pass, so the program chooses which name each ticket sits at and which
gate `k` it sits in; the successor is the next name in that gate. Every
stage in the corpus names its ticket by the loop index. That is the
shape of a block chain and a tile scan, not of the construct: a ticket
may sit in several gates, a gate is a chain, and the order stated is
the union of the chains. The cap is a separate tier: `enter` ignores
the ticket and takes a token, `cap` runners take tickets in the order
the caller sent them, and a runner parked in a stage holds its slot.
The gate knows nothing of it. Two facts the gate tier does not state:
the head of each chain is name zero, since nothing passes zero unless
the program passes the head's predecessor by hand; and serial elision
requires the name order to agree with the ticket order, because on the
sequential schedule every pass must precede its wait in program order
or the one fiber parks on its own turn. The name order is the
program's fact and the detector is its floor, by the preamble's rule.
Today the floor holds on one schedule and not the other. On the
parallel schedule a wrong name fires the detector, and the report says
two fibers parked in `exclusive_when`, with no gate, no name, no
ticket, and no line, under a causes list that names channels and
joins. On the sequential schedule the caller parks on the host-thread
path, which the detector does not scan, and the program hangs with
nothing printed. The floor is a floor only where the park is a
fiber's, and it is loud only if it names what the program named.

Since seed 413 an arm's unhandled unwrap crosses an immediate-wait join
as a `CCError` whatever the arm unwrapped, a `CCIoError` through its
base. The join is a rendezvous and not an operation of its own, and
what it reports is that an arm failed; the frame needs a `CCError`
handler for that record to reach, and any other error type is handled
inside the arm.

The nursery is the join set underneath `@parallel`: a wait-for's `h.n`
is one, a dest's bodies are its children, EMPTY is its event. It is
also still a teaching surface. The cc way and the README do not name
it; getting started and the cheatsheet teach it beside `@parallel`, and
three of fourteen recipes use it. The specimens that keep it (pigz,
random access, redis, curl) use a spawn with a capture list, an
EMPTY-close, a leave with a leftover, and `@destroy` as the join. The
dest has each of those but the capture list. `create_child` has no
user. Two spellings of one bag is the older one not yet withdrawn.

The handle states its own lifetime and not the work's. `h.live()` is
planted-and-not-joined; right after a kick the wave can be finished and
`live()` is still true, and the spec says not to read `h.n` or `h.nt`.
So every program that kicks waves keeps a second cell: a `done` latch
beside the handle in four structs, a second `cancel` flag beside
`h.cancelled` in three of them, because the program's cancel must be
readable before the plant and after the join, when there is no handle.
The find in cctext is the specimen: one job (a query), many waves (a
kick per idle frame), and the job's facts live in the struct while the
wave's live in the dest. The same three files poll `h.paused` inside
each `@stage` block; the construct already honors pause between the
stage's `wait` and its block, so those loops are drift.

A join set with no work has no spelling. The idiom is a plant with a
no-op arm, `@parallel spawn { @serial { (void)0; } } !>`, in seven
places, three of them in the teaching recipe. Under `spawn` that arm is
a fiber: an empty join set costs one spawn and one join of nothing.

What the runtime reconstructs, and from what:

| Mechanism | Recovers | From | Could a party state it |
|-----------|----------|------|------------------------|
| adaptive spawn gate | whether this arm is worth a fiber on this machine | measured leaf time per site; 1-in-2^20 resample | no: the cost is the machine's. The cutoff predicate states the shape when the caller knows it |
| deny stack | that a denied join needed concurrency | a park inside a denied arm | the direct case is ill-formed already; through a helper, nothing says the helper parks |
| deadlock detector | who waits on whom | park reasons, a 1000 ms latch | the join and stage edges are stated; the exemptions are stated as dynamic scopes; a park's partner set is not |
| pool growth, wake-skip, sysmon | whether capacity is short | queue depth, idle count | no |

The exemptions are statements at the wrong layer. `cc_deadlock_suppress`
and `cc_external_wait` both mean "this park's progress source is
outside the graph" and the detector treats them identically; a program
picks one. Both are scopes: curl's worker wraps its whole loop in one,
so every park inside it is exempt, including the internal hold on
`done` that the program did not mean to exempt. The fact is a property
of the queue the host fills, stated once at its creation; it is spelled
today as a property of whoever waits, stated at each wait.

The bag is missing four faces that a host queue needs and a join set
does not: retract a queued item, detach without joining in-flight work,
poll for empty, and grow or shrink the runner set after the plant. Curl
stays on a nursery and an exclusive for those.

### 3.2 Proposal

Every wait names its partner set. A join set is the partner of its
wait; a stream's partner is the join set registered to close it; a
wait whose partner is outside the program says so on the object, once.

1. **A join set is a declaration.** `CCParallel h@();` plants a live,
   empty dest: no arm, no fiber. `h.wait()` or `h.leave()` ends it as
   today. The no-op arm goes.
2. **The job is the dest; the wave is a drain.** `h.drain()` joins
   everything admitted so far and leaves the handle live, marks intact;
   `h.wait()` stays the terminal join. A job is then planted once,
   admitted per kick, drained per frame, and waited at close: the find
   in cctext with no struct-side latch and no second flag.
3. **Finished is a read.** `h.settled()` is true when nothing is running
   and nothing is admitted, from the occupancy the growing form already
   keeps. It is not `live()`. A settled dest may receive an admit on the
   next line, and the admitter is the same program; that is its fact to
   sequence.
4. **Marks belong to the job.** `cancelled` and `paused` persist across
   drains. A cancelled job refuses its next admit (`CC_ERR_CANCELLED`),
   which is what the struct-side flag was guarding by hand.
5. **External progress is the object's fact.** A channel or exclusive
   created as host-fed marks every park on it external. The scope forms
   remain for the dynamic case, a `pread` on a caller thread, and stop
   being the idiom for a queue.
6. **Drift, not design.** Delete the in-stage pause polls; read
   `h.paused()` not the field.
7. **The nursery is implementation.** The taught join set is
   `@parallel` and its dest; `CCNursery` is the runtime's type under
   it. What the dest absorbs: the arena-hosted handle, `h@(a)` on the
   `create_*(owner, …)` form of section 2, so the arena is the join
   set's parent and the walk joins it; and the spec line that sends a
   named task lifetime or tile size to `n.spawn`, which becomes a dest
   attach. Cancel reach is `adopt`; the clock is named. Getting
   started, the cheatsheet, and the three recipes move over.

Reconstructions counted: ten today, six in the runtime and four in the
program. Items 1 to 4 close the four in the program. Item 5 moves one
exemption from scope to object; it is a precision change, not a new
statement. Item 7 changes no count: it withdraws a spelling. Five remain in the runtime and are the floor by the rule in
the preamble: the gate, the deny stack, the detector, growth, and
wake-skip each recover a fact no party holds before the run. The deny
stack's helper case is the one of those a party could state, and it is
an open item, not a proposal.

### 3.3 Cost

| Item | Run-time cost | Memory | Notes |
|------|---------------|--------|-------|
| 1 declaration plant | removes one spawn and one join per plant | none | today's plant is a real fiber under `spawn` |
| 2 drain | the same park as `wait` without the terminal store | none | the dest keeps the occupancy already |
| 3 settled | one acquire load | none | replaces a program-side latch that was one store and one load |
| 4 persistent marks | none | none | the flags exist; the bind stops clearing them |
| 5 host-fed object | one flag read on the park path | one bit | the park path already reads the object's state |
| 6 delete polls | removes a yield loop per stage | none | |
| 7 nursery under the dest | none | none | `h.n` is already the nursery; an arena-hosted dest is the handle the nursery already places |

Pause is a yield loop at every seam: a paused dest with k tickets at
seams spends k yields per scheduler pass until resume. A parked honor
would need a wake list per dest and would make `resume()` a wake, which
today it is not. That is a cost the current design chose; it is not
free, and it is not charged to the proposals above.

Two schedules for one body is the one place the language lowers the
same text two ways. It holds to one build, one meaning because the
schedule is in the text: the predicate, the `seq`, the directive. The
adaptive gate is the runtime's choice and changes only timing, never
which arms run or what they see, except through the deny stack abort,
which is loud.

Not admitted: reading `h.n` or `h.nt` as finished; a runtime that
guesses a partner set for a park; a debug build with a different
detector.

Measured, on Bend's `pow2` tree at depth 24 with a cut of 8 (255
spawns), Linux, four cores, seed 414, three repetitions:

| Form | vs sequential C |
|------|----------------|
| `@parallel (0)` at every node, the false path | 1.15x, faster than the C |
| `#pragma(@parallel) off` | 1.07x |
| `@parallel spawn` above the cut | 3.4x, every run |
| `@parallel (pred)` above the cut, C below | 0.90x, every run but two |
| `@parallel (pred)` at every node | 0.75x; 0.20x with four eager workers |

The same 255 arms, the same work. The statement delivers the machine
and the deniable form does not, on this platform; the author's ten-core
Darwin receipt has the deniable form at 3.3x. The gate is not the
cause (the site reads `real`, and turning the gate off does not help),
nor is wake-skip. Where the deniable join loses its time is a runtime
question; what the measurement settles is that its denial, its
inline-on-deny, and its grow-on-demand are reconstructions with a cost
that varies by platform, while `spawn` is a fact with a cost that does
not. Study: `studies/parallel_pow2/`.

The detector's report is part of its cost account, since a floor that
does not name the program's own words is paid for twice, once in the
hang and once in the reading. What it can say for free: the park
reason as the construct (`@stage wait`), the gate, the stage, the
name, and the ticket, which the wait already holds; the file and line,
which the park macro already passes and the V2 path discards; and a
host-thread park on a runtime object counted as an internal wait, so
the sequential schedule is not silent. Each is a store the park path
makes once.

### 3.4 Open

- **A parking helper.** The brace-join rule refuses a direct channel
  operation in a denied join and cannot see one through a call. A
  signature fact, a function that may park, would let the rule reach
  through. The burden is on every such signature, and a missed one is
  the runtime abort again. Whether the fact is worth its spelling is
  not settled.
- **The capture list.** `n.spawn(() => [&x] { … })` states the mode
  of each capture in one token. A dest body states it by type: a value
  copies, a pointer copies the pointer, an owner moves, and `&x` inside
  the body is refused in favor of a frame-side pointer. Both are stated
  facts; one is a list and one is the type. Whether the list said
  anything the type does not is the one question item 7 leaves.
- **Adopt is half a tree.** `adopt` is a cancel edge and not a join
  edge; `h1.wait()` does not wait `h2`. A tree that cancels down but
  does not join down states one direction of the relation.
- **Two clocks in one body.** The first arm sees the caller's deadline
  and spawned arms do not. A construct could refuse a body that reads
  the current deadline in an arm that will not see it, so that naming
  the clock is required rather than remembered.
- **The host queue.** Retract, detach, poll-empty, grow and shrink are
  four faces or a statement that this ABI is the bag. Nothing here
  decides which.
- **Stages outside the loop.** `@stage` is ill-formed outside a
  wait-for body, so a graph of dest bodies joined by names has the
  cell but no spelling.
- **Two touchers.** A cell is one pass and one wait. Fan-in is several
  stages in sequence; fan-out is several passes. A cell with many
  waiters is a different object, and the exclusive layer's broadcast
  is not reachable through the stage.

### 3.5 Surface that does no work

- The no-op arm as a plant.
- Two exemption spellings the detector does not distinguish.
- `live()`, named for the handle, read as the work, with the latch
  beside it as proof.
- `h.paused` the field and `h.paused()` the load, both readable.
- Two spellings of the join set, taught side by side.

## 4. Co-facts

A co-fact is one fact stored in more than one place: two fields, a
field and a runtime object, a value and something computed from it.
Everything between the two stores is reconstruction done by the
reader, every time. The language's best constructs are each a co-fact
made one value: the slice (pointer, extent, lifetime), the Result (tag,
payload), the variant (arm, data), the id (bytes, generation), the
ledger (obligation, exit). The rule they share is single storage, and
this section applies it to the program's own data.

### 4.1 Status

The specimens keep co-facts by hand in four shapes.

| Shape | Instances | Kept by |
|-------|-----------|---------|
| a count shadowing a set | staticd `nworkers` beside the dest's admits; curl `live` beside the nursery's children | CAS to reserve, decrement on two exit paths, decrement again on a failed spawn |
| a record whose fields move together | the find's `scan_off`, `scan_bytes`, `done`, `truncated`; the dest latches of section 3 | interleaved stores in one stage block; the UI can read one field fresh and another stale |
| a derivation with a source | cctext `edit_gen` bumped on every content change, `safe_gen` "last flushed edit_gen", dirty is their inequality; stylo `inv_stamp` and `bloom_stamp` against per-node `inv_gen` and `bloom_gen`; staticd's poll `gen`, `hit_epoch`, `wake_epoch` | a hand-rolled version on the source and a stamp on the derived value, compared on read |
| a tag beside a payload | `disk_valid` beside `disk_mtime`, `disk_sz`, `disk_ino`; `mark_valid` beside `mark_line`, `mark_off`; `seek_valid` beside `seek_rel` | a validity int the reader must test first |

A census of every project in `real_projects/` and the std server
library (`studies/cofact_census/`) finds about 170 such sites. The four
largest groups are a tag beside a payload, a stored derivation, a count
beside a set, and a check that belongs at a seam; together they are
about 90 sites, and each has a zero-cost statement below. Deltas are
rare: outside cctext's history no project keeps an edit journal. The
census also found a data-loss bug of exactly this shape: pigz's
`--rsyncable` path fills a segment array sized for the densest possible
hits, drops the tail segment, never checks that the segments sum to the
block, and exits 0 with a corrupt archive.

Twenty-two fields across the corpus are named `valid`, `dirty`,
`stale`, `gen`, `epoch`, or `version`. Three specimens invented the
version-and-stamp pattern independently. No comment anywhere says
"keep in sync"; the sync is in the reader's head.

`@variant` is the spine of the compiler and the construct with the
most lowering holes. Exhaustiveness is forfeited wherever `default:`
is forced by a hole. There are three tagged-data dialects: variants,
hand `{kind, u}` structs, and grammar-generated unions.

`@typeview` confines writes. A default `r: *` refuses a field store at
an ordinary site; a named mode refuses a store outside its `rw:` list
inside a body of that mode; trust is by first-parameter shape. Nineteen
declarations in the specimens, most of them named modes that carry a
write surface by concern. Trust extends through embedding, and a taken
address is the exit.

### 4.2 Proposal

One storage, the rest derived. A co-fact set has one primary, which is
stored, and any number of derivations, which are reads. The set is
constructed by the derivation naming its sources; the primary knows
nothing, holds no subscriber list, and pushes nothing.

1. **A count is a read of its set.** The set already knows its
   occupancy and serializes its admits, so the condition and the admit
   are one operation on it: `@parallel(h, below: n) { … }` admits or
   says no under the set's own lock, and a worker leaves with
   `n.leave_above(min)`. The count field, the CAS helpers, the
   decrements, and the rollbacks go.
2. **A record is its primary.** The scan record stores the position
   scanned and the hits appended; `done`, `truncated`, `scan_off`, and
   `scan_bytes` are one-line reads against the file length. One word
   stored cannot tear.
3. **A cheap derivation is a read hook.** The precedent is `.len` on a
   walk subject: readable at ordinary sites as a field, not storable. A
   named read hook on a type lowers `d->find.done` to a call and refuses
   the store. Call sites do not change.
4. **An expensive derivation is a cache with a stamp.** A primary that
   has caches carries a version; a store to it bumps the version; the
   cache stores its sources' versions beside its value and a read
   compares. This is `edit_gen` and `safe_gen` with the language holding
   the pattern. Auto-tracking, recovering the sources by watching the
   derivation run, is reconstruction; the derivation names them.
5. **A tag beside a payload is a variant.** `disk_valid` and its three
   fields are one arm.
6. **Validation is at the seams.** Where a primary enters from outside,
   the wire and construction, a check on the store is right, per store,
   because at a seam there is no intermediate state to allow. Serdes
   assigns fields, so wire validation falls out with an error naming
   the field. Construction stays open today, so an unvalidated value can
   be made by designated init; a validated type is sealed to its create
   hook.
7. **The residue is a hold.** A move between two containers is two
   primaries that must agree, because in an intrusive list membership
   is the storage. That is a verb on the pair under a hold, which the
   code already writes. A transaction construct adds nothing the hold
   and the ledger do not say.
8. **A taken address of a non-writable field is refused**, as the walk
   already refuses `&v` on a binder. One condition in the allow-list
   check.

**Authoring.** Three groups in the type's unnamed view, and ordinary
functions for the rest. The view already holds two kinds of group:
faces, which change how a use lowers, and allow-lists, which decide
what a site may do. Bindings are a third kind, of the first sort: they
say what a use of a field means, everywhere, including trusted bodies,
as a face does. The trusted-body exemption is an admission rule and
does not touch them. Named modes narrow admission and may list derived
names; they do not bind, so a version is one fact per type. The view
adds no bytes: the author declares the counter and stamp fields, and
the binding finds them by name.

```c
typedef struct {
    size_t pos;      uint32_t pos_v;      /* the primary and its version */
    size_t len;      uint32_t len_v;
    size_t *offs;
    RtxLineIndex idx; uint32_t idx_v;     /* a cache and its stamp */
} RtxFind;

@typeview on RtxFind {
    r: *;                            /* stores only in RtxFind* bodies */
    v: pos, len;                     /* a store also stamps pos_v from the counter */
    d: done, truncated, scan_off;    /* read like a field; resolve as UFCS; store refused */
    c: idx from pos, len;            /* read compares idx_v to the sources; rtx_find_idx rebuilds */
};
```

Use sites do not change. `d->find.done` reads, `d->find.done = 1` is
refused, `d->find.pos = v` stamps, `d->find.idx` checks and rebuilds.
A derived name is a function of the object alone; one that needs an
argument is a method. A cache whose builder is `!>(E)` makes its read
a Result site, and that is the one place a use site changes. Derived
of derived is a call; cache of cache is a check chain; several sources
are several stamps; an aggregate store re-stamps every versioned field;
zero-init is stale, so designated init and serdes carry no versions; a
cache's storage is a part of the object and dies in its walk.
Hooks stay what they are, lifecycle and library naming.

The lifetime order of section 2 applies unchanged: a cached derivation
holds views of its sources, so a source outlives every derivation of
it, and the primary has the set's longest lifetime. That is why the
per-wave dest of section 3 grew a second `done` beside it, and why the
job-lifetime dest is the fix.

Not proposed: a transaction scope, since single storage removes the
intermediate states it would allow and the hold covers the residue; a
per-instance validator, since a fact stated per construction is
invisible at the type; a subscriber graph.

There are three places to do the work of a derivation, and the signal
libraries have tried all three.

| Model | Writer pays | Reader pays | Needs |
|-------|-------------|-------------|-------|
| update on write | every derivation, every store | nothing | the writer must know its derivations: a graph, or the hand-kept co-fact |
| update on read | one increment | a staleness check, then the recompute if stale | versions on sources, stamps on caches |
| mark on write, update on read | a walk of the reachable subscribers, stopping at ones already marked | a walk of the marked path only | a graph in both directions, with a link per edge |

The corpus today is the first row done by hand: the stage block writes
`done`, `scan_off`, and `scan_bytes` beside the position, which is
update on write with the reader keeping the graph. Preact's core is the
second row: a computed that nobody watches never subscribes, checks a
global version for the quiet case, and walks its sources' versions
only when that moved. alien-signals, which Vue 3.6 adopted, is the
third: a store marks its transitive subscribers pending and touches no
values; a read walks only pending sources, recomputes those that are
dirty, and an unchanged result stops the walk. Its graph is a doubly
linked list in both directions with one link per edge, reused in order
across recomputes so a recompute does not churn allocation. It pays the
write path to make reads precise, and wins when many unrelated sources
change between reads, because a global version spoils the quiet-case
skip on every unrelated write and a per-source walk on a deep chain is
the whole tree.

What decides the row is the depth of the derivation graph. Every
derivation in the specimens is depth one: a stamp against its source,
a dirty bit against an edit generation, a node's generation against
the engine's. At depth one a per-source compare is constant, the
writer pays one increment, and no graph is needed. The third row earns
its graph at depth, in an incremental compiler or a UI tree, and that
is a library of a few hundred lines with ports in Rust and Go already,
whose links are churned per recompute and belong in a scratch or a
pool. The language's part is the vocabulary both rows share: a
versioned primary, a stamped derivation, a named source.

Update on write keeps one legitimate case: a derived scalar published
to another fiber. The writer computes it and stores one word, so the
reader loads one word instead of taking a seqlock over the sources.
That is the section 3 `settled` flag, and it is right exactly when the
derivation is cheap and the reader is elsewhere.

Effects are the same in all three rows and are not the graph's job
here: an effect is a fiber parked on a channel or a signal, woken by
the writer's own statement. Pull with versions is glitch-free by
construction; the push rows need a flush at the end of the write to
avoid running an effect on a half-updated diamond, and both libraries
have one.

**Derivations with stated edges.** A derivation is a declaration
suffix, the way `@destroy` is: it says how the binding behaves. The
initializer or the block is a closure, and like every CC closure it
captures by free name. So its free names are its edges, stated once,
inline, where the logic is.

```c
/* expression: the initializer is the derivation */
size_t nlines = rtx_count_lines(&doc->tree) @derive;

/* block: the body updates the declared name in place, as @destroy { } names it */
LineIndex idx @derive {
    idx.clear();
    @for (p in doc->tree.pieces) idx.add(p, width);
};

/* optional list: narrow the edges, or compare a value instead of a version */
Markup hl @derive [&idx, window] { ... };
```

```c
if (idx.tree_v != doc->tree.v || idx.width != width) { rebuild_idx(); idx.v = ++cc_gen; }
if (hl.idx_v != idx.v || hl.win != window)            { rebuild_hl();  hl.v  = ++cc_gen; }
```

JS also knows a closure's free names; what it cannot know is which
reactive cells the body reaches, because cells are runtime values. Here
the edges are names of versioned storage, so the lexical free-name set
is the edge set, and the finest edge is the field path the body reads:
reading `doc->tree` makes the tree's version the edge, not `doc`'s. A
free name that is a function, a constant, or a comptime value is not an
edge; only mutable storage is. The dependency graph therefore lives in
the compiler, and bringing derived state up to date lowers to the
straight-line compares above with no graph at run time.

A read brings a derivation up to date, and a read of `hl` checks `idx`
first, so the order comes from the edges. Reading the value is where the
rebuild cost lands. A captured scalar is its own version: the stamp
stores the value. A rebuild that produces the same value does not bump,
so early cut-off is free. The block form runs against the existing
value, so the body can reuse its storage or patch it. A named function
is not required and not excluded: `LineIndex idx =
rtx_line_index(&doc->tree, width) @derive;` is the expression form.

What it removes, in the specimens:

| Hand-kept today | Where | With a stated edge |
|-----------------|-------|--------------------|
| one snapshot of 20 fields written three times: copied in `rtx_ws_safe_note`, compared in `rtx_ws_safe_stale`, hashed in `rtx_ws_safe_sig` | cctext `workspace.ccs` | one capture list; the copy and the compare are generated; a body reading an uncaptured field is refused |
| push invalidation: every edit path must clear `hl_full`, zero `hl_win_stamp`, or call `analysis_reset` | cctext `document.ccs`, four sites | the edit bumps the tree's version; the writer no longer needs to know its readers |
| `edit_gen` beside `safe_gen`, dirty as their inequality | cctext | the pattern named once |

Four boundaries. A call inside the body can hide an edge: the free names
are lexical, and a callee that reads state it was not handed is not. An
attribute stating that a function reads only its parameters closes that
at compile time where it is written; it is an option, not the price of
the construct. This is make's missing
header, and `gcc -MD`, which records what the compiler opened, is the
legitimate reconstruction for it. Over-capture is safe and costs only a
recompute. A facts-you-do-not-own source, such as a file checked by
`stat`, has an observed version rather than a bumped one, so its capture
takes a probe in place of a counter. And a deep tree with sparse change
favors marking: stylo sets `dirty` at eight sites and propagates it by
hand, and per-node stamps checked from the root would compare every node
on every restyle. That case stays a library with an explicit graph.
And data-dependent reach is coarse: a body that walks a collection has
one edge, the collection's version, which any element store bumps when
element stores go through the collection's verbs.

The relation `cache == body(captures)` is stated by the declaration and
checked by execution. A harness rebuilds each derivation from scratch
after each settle and compares it with the cached value. A mismatch at
equal stamps means an edge is missing from the capture list, which is
exactly the failure the compiler cannot see through a call. The check is
the net under the one hole in the idea, it is a test and not a build
mode, and the law costs the author nothing to write. Comparing needs an
equality on the derived type: structural where comptime can generate it,
a declared hook where the value holds views.

Pure languages remove co-facts by not mutating and then rebuild
incrementality by hand: the cache comes back as a field of the state,
with its stamps, and Lean's language server and rust-analyzer's Salsa
are the machinery that results. Salsa is the nearest precedent here,
with versioned inputs, memoized queries, and early cut-off, but it
records dependencies at run time. Stated edges, imperative bodies, and
no run-time tracking together are, as far as this review found, new.

### 4.3 Cost

| Item | Run-time cost | Memory |
|------|---------------|--------|
| 1 count as a read | removes a CAS and two atomics per spawn | one field fewer |
| 2, 3 read hooks | the derivation's compares on each read; nothing stored | fields removed |
| 4 versioned primary | one increment per store; one compare per cached read | one version per primary, one stamp per source per cache |
| 5 variant | none | often less: one tag replaces several ints |
| 6 seam validation | one call per annotated store at a seam; nothing elsewhere | none |
| 8 address refusal | none | none |
| derivations | one compare per captured source at each settle or read; nothing per store beyond the version increment | one stamp per captured source per derivation |
| the execution check | a from-scratch rebuild and compare per settle, in the test harness only | none in the program |

A versioned primary read across fibers is a seqlock, version then value
then version, or a hold, unless value and version pack into one word as
the id does. The language says so rather than hiding it. ABA on a
per-primary counter is the grower-epoch trade of 2.4: one global
counter and a width question, or per-primary counters and rare reuse.

### 4.4 Open

- Whether the version is per primary or drawn from one counter.
- The projection protocol for the three tagged-data dialects, and
  comptime naming arms, so exhaustiveness is not forfeited to a hole.
- Whether trust through embedding should stop at the field.
- Sealed construction's spelling, and whether serdes-filled types are
  sealed by default.
- Five places where one storage costs more, from the census: scalars
  published to other threads that are meant to be stale; external
  sources with no version to bump, the kernel's interest set, the
  filesystem, and the clock; hot loops where any added store shows;
  pinned snapshots that must not resync; and a deliberate sparse second
  storage. The model needs a spelling for two of them: frozen after
  construction, which also lets a compiler hoist through aliasing, and a
  pinned version.
- `seq (cond)` is never compared in pigz, and parallel_storm keeps four
  hand copies of one recursion. Making the comparison automatic is the
  first place to apply a stated relation checked by execution.
- A version says whether a source changed, not what changed. Patching a
  cache in place after an edit needs the edit, a delta. cctext keeps an
  edit journal, so the shape is a source that carries a version and a
  log since a given version; its spelling is open.
- The cctext chain of line index, highlight, find, and layout is the
  specimen to write this against first, to measure how many hand-kept
  `valid`, `gen`, and invalidation sites disappear and how often a body
  calls into a function that reads state it was not handed.
- Which other stated relations get a generated execution check: `cache
  (name)` states that deleting the clause leaves the serial program, and
  a grammar schema that reads and writes states a round trip.

### 4.5 Surface that does no work

- A `valid` int beside the fields it guards.
- A version and a stamp with no name for the pattern, in three
  specimens.
- A count kept beside a set that already counts.

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
