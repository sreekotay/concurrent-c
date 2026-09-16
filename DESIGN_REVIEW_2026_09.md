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

1. **Arena-last is inferred.** A call whose last argument is an arena
   returns a view of that arena. Over-approximation can only miss,
   never refuse wrongly.
2. **Field paths are names.** `d.store` is a lifetime while `d` is; a
   view stored into `d.body` is held by `d`.
3. **A kept parameter is declared.** One attribute on a parameter or a
   field: this view is kept for the receiver's or object's lifetime. The
   comparison moves to the call site, where both lifetimes are locals.
   The receiver is the only lifetime variable needed, because arena-last
   and receiver-first already fix which lifetime is meant.
4. **A dest is a join set.** Late admit pins like spawn.
5. **Scoped spawn compares scopes.** The join set's scope must lie
   inside the captured lifetime's scope; `leave` is an escape.
6. **Send of an aggregate with an arena moves the binding.** The
   sender's name is dead afterward, as after adopt or detach.
7. **Copying an owner handle without a move is refusable** (compile
   time), and a stale copy refuses at run time (see 2.3 for the two ways
   to pay for that).
8. **Runtime handles are born into an owner.** Turnstile, parallel, and
   exclusive get `create_*` constructors like nursery and pool, per the
   storage classes the lifetime-parents design already defines.
9. **The own step runs after UFCS, or composes names by the universal
   rule.** Restore, try_restore, and detach join the epoch-ending table.

Diagnostics name both ends: the view's lifetime, the holder's, and which
ends first.

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
| generation in the handle | one mask on every host peel; one compare in `is_live` | same retention | the handle stays 8 bytes (tag above the 48-bit pointer on 64-bit; 32-bit pointer plus 32-bit generation on ILP32) but host C that peels `.a` / `.p` directly no longer holds a raw pointer |

The second closes ABA. Both cost one allocation per heap arena, which
is on the per-request path. The static half alone covers every copy the
checker can see; the runtime half covers copies that reach a field or a
parameter.

Item 8 is cheaper than today. The handle is a bump from the owner plus
one attach record, and it dies in the owner's walk. Today a turnstile is
hosted by an arena minted for it, and a parallel handle by a calloc.

Open items with a cost:

- The debug read-check costs one registry lookup and one compare per
  `at`, `set`, and walk entry, in debug builds only. The registry holds
  one entry per live epoch block.
- Promotion versus spill: promotion mallocs a host when scratch outgrows
  a slab under a mark; spill would malloc per pre-mark regrow instead.
  Which is rarer depends on the workload. Neither is on the fast path.
- The 32-bit grower epoch has no free fix. Widening the id makes every
  slice 32 bytes instead of 24. Moving the generation out of the id
  loses stale-owner detection on grower views. Drawing epochs less often
  weakens restore staleness. This is a trade to choose, not a repair.
- Pinning a mark instead of the arena, last-use liveness, and the kept
  attribute are compile-time only.

### 2.4 Open

- The kept-parameter spelling, and whether an undeclared keep is an
  error or a warning during transition.
- A debug read-check through the view verbs; hosts draw epochs in
  256-aligned blocks, so a block-to-host registry is the lookup.
- Last-use liveness within a block, so a reset after the last use of a
  view is not refused.
- Pinning a mark instead of the whole arena when a task holds a view
  minted above a checkpoint.
- Whether pre-mark regrow under a checkpoint should spill to per-object
  overflow with the root epoch instead of promoting.
- The 32-bit grower epoch drawn from one global counter.

### 2.5 Surface that does no work

- Three constructors taught as three ideas; they are one lifetime and
  three storage policies.
- "Provenance" used for both the runtime token and the static fact.
- `@scratch(N)` taught as a size; it is the root, and larger products
  spill and are still reclaimed.
- The spec's capture example shows a stack slice into a same-frame
  nursery as an error; the closure step accepts it, correctly.

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
