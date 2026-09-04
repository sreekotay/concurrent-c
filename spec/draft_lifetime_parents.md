# Lifetime parents

Status: draft. The arena operations, parent views, viewed faces, and
`cc_arena_create_nursery` / `create_child` are implemented (`cc_arena.cch`,
`cc_nursery.cch`; `tests/arena_lifetime_parent_smoke.ccs`,
`tests/as_viewed_face_smoke.ccs`, `tests/nursery_create_owner_smoke.ccs`).
The lowerer pins `Alloc` / `Parent` / `Region` on `CCArena` so header
lowering does not drop them.

## 1. Notion

An arena is a **lifetime parent**: it holds destroy obligations for other
objects — its **children** — and discharges them when it is destroyed,
before releasing its own memory. Parenthood is a runtime relationship
between objects, held entirely by the parent. The scope annotations
`@destroy` / `@detach` are independent and unchanged; for a live object, a
scope obligation and a parent obligation are never held at once (§3).

`CCArena` is the only parent type. A type becomes a container by embedding
an arena whose destruction sits at the right point of its own teardown and
exposing that field as a face (§6); it does not implement parenthood
itself.

## 2. Storage classes

The operations an arena admits follow from where its handle and oldest
slab (L1) live:

| class | birth | may move | destroyed by |
|---|---|---|---|
| scope value | `CCArena a = cc_arena_heap(n) @destroy;` | yes (heap L1) | scope epilogue |
| embedded child | `owner->create_arena(n)`, `n > 0` — L1 carved from owner | no — storage-bound | owner's walk |
| free child | `owner->create_arena(0)` — heap L1 (default 4096) | yes — may move again | owner's walk |
| sized free child | `owner->create_heap_arena(bytes)` — heap L1 of `bytes` (`0` = default) | yes — may move again | owner's walk |

A `cc_arena_stack` arena is a scope value whose L1 no move accepts; an
embedded child is the same refusal one level up — its L1 is the owner's
storage. `cc__arena_l1_heap_owned` is the discriminator; no flag records
the class.

## 3. Dead state and moves

Every hooked type defines a **dead state**; for `CCArena` it is the zero
value. Destroying a dead object is a no-op. Every operation that takes
ownership away from an object's current location leaves the source dead.
A **move** returns the object at its new location; only the source husk is
dead.

- `cc_arena_detach(&a)` moves the arena **to the caller** — value return.
  The caller places it: return it, store it, or adopt it.
- `parent->adopt(&a)` moves the arena **into `parent`** — the result is
  the arena at its embedded address, and a record (§4) is appended.

Because moves consume their source:

- a scope obligation on the source discharges vacuously — the epilogue
  runs the chain on the husk, a no-op;
- adopting the same object twice is refused at the second call (dead
  source);
- a record left in a previous parent points at a husk and is skipped by
  that parent's walk.

Build in scope, escape by move:

```ccs
CCArena build_index(CCSlice paths) {
    CCArena a = cc_arena_heap(kilobytes(64)) @destroy;  // every error path covered
    for (...) { parse(p, &a) !>; }
    return a.detach() !>;                                 // success path moves out
}
```

The return expression is evaluated before the scope epilogue (§5.1 of the
complete spec), so the epilogue meets the husk.

## 4. Records and the walk

A parent holds a list of records `{object, destroy fn}`. Record nodes are
allocated from the parent itself and are never freed individually; an
unlinked record is a tombstone, compacted opportunistically during later
attaches. List mutation is serialized by the arena's meta lock. A child
stores `lifetime_parent` beside `self_rec` and takes that lock before
writing `obj` (tombstone, re-home). The teardown walk claims each `obj`
under the same lock, then runs destroy unlocked. There is no lock order
beyond the parent list under that parent: teardown never holds two parent
locks; a child's free tombstones after the parent steal has dropped the
lock. Steal does not clear each child's `lifetime_parent` / `self_rec` —
the child's free still tombstones the stolen node while the parent host
is alive. Generic `cc_arena_attach` does not pin `self_rec`; arena hosts
use `cc__arena_attach_host` so detach/free can tombstone.

Destroying a parent first **walks** its records, newest first (LIFO),
before the arena releases overflow or slabs — child handles may live in
either. Each record runs its destroy fn on its object; dead objects no-op.
The walk runs exactly once per parent destruction and clears the list.

`cc_arena_reset` runs the same walk before rewinding: attached children
are contents, and contents die at reset — their handles live in the slabs
being rewound, exactly like every unhooked allocation. The arena stays
live and may attach again afterward.

A checkpoint is itself an attached child (the active one): its record is
walked at reset and free like any other, and restore is the child's own
free. Attach records never refuse a capture or a restore. A checkpoint
child's record lives inside the child's region and is unlinked when the
child dies; a heap-rooted child's record is released back to the parent.

Records are hints; the object's own live/dead state is authoritative.

Attaching to a parent that is mid-walk is refused.

## 5. Operations

### `cc_arena_adopt`

```ccs
CCArena !>(CCError) cc_arena_adopt(CCArena* parent, CCArena* src);
```

Moves `*src` into `parent`: detaches it (all `cc_arena_detach` guards
apply), embeds the handle in `parent`'s storage, appends a record, and
returns the embedded handle. Adopt is atomic: on any refusal the source is
unchanged (a failure after the detach point restores `*src`); on success
the arena lives in `parent` and `*src` is dead.

Refusals, each a distinct error: dead source; stack- or caller-owned L1
(including an L1 carved from another arena); outstanding checkpoint loans;
`parent` dead or mid-walk; `parent`'s handle located inside `src`'s
storage (cycle); handle allocation failure in `parent`.

Adopt accepts only `CCArena`. Other hooked types are not movable by copy
(interior pointers); they enter a parent at birth via `create_*`
constructors and never move.

### `cc_arena_attach`

```ccs
int cc_arena_attach(CCArena* parent, void* obj, void (*destroy)(void*));
```

The primitive for non-arena objects: appends a record. It does not pin
`self_rec`; an arena child uses `cc__arena_attach_host` (via
`create_arena` / adopt) so detach and free can tombstone. The caller
supplies the destroy fn — for user types, the function registered as the
type's destroy hook. Fails, nonzero with a report, on a dead or mid-walk
parent.

### `create_*` constructors

A constructor named `create_<thing>(Owner*, ...)` births an object into an
owner: handle allocated from the owner, initialized, attached, returned as
a pointer. The prefix is load-bearing: parent views admit `create_*` by
glob (§6), and UFCS bare-name dispatch gives every such function the
`owner.create_<thing>(...)` spelling with no further declaration.
Constructors are declared in the product's header:

```ccs
CCArena !>(CCError) create_arena(CCArena owner, size_t n);         // cc_arena.cch
CCArena !>(CCError) create_heap_arena(CCArena owner, size_t bytes); // cc_arena.cch
CCArenaPool* !>(CCError) create_pool(CCArena owner, size_t elem);  // cc_arena.cch
CCNursery !>(CCError) cc_arena_create_nursery(CCArena* a);     // cc_nursery.cch
Session*     create_session(CCArena owner, int fd);                // user code
```

`create_arena(owner, n)` with `n > 0` carves the child's first slab from
the owner: the child is storage-bound and cannot move. With `n == 0` it is
`create_heap_arena(owner, 0)`: a free heap child with the default L1, which
may later be detached or adopted elsewhere. `create_heap_arena(owner, bytes)`
is the sized heap-child spelling — L1 is `bytes` (or the default when
`bytes` is 0). The `create_arena` size argument selects the storage class;
it does not size a heap child's L1.

`create_arena` is Result. OOM and a dead owner are `cc_err`, never a dummy
empty handle.

A `create_*` result carries no scope sigil; the constructor named the
holder. Declaration construction (`name@(args)`) still requires `@destroy`
or `@detach`, unchanged.

## 6. Parent views and container faces

`cc_arena.cch` declares three modes on `CCArena`:

```ccs
@typeview Alloc  on CCArena { r: alloc, remaining; };
@typeview Parent on CCArena { r: adopt, attach, create_*; };
@typeview Region on CCArena { r: alloc, remaining, adopt, attach, create_*; };
```

`Alloc` and `Parent` are for signatures — a function takes
`@typeview(Parent) CCArena*` to prove it constructs and owns but never
resets or destroys. `Region` is for container faces: a type that embeds an
arena exposes it with a viewed face (`as: (Region)field;` — see the type
views spec), and every parent operation composes through the face:
`n.alloc(...)`, `n.create_arena(...)`, `n.adopt(&a)`. Allocation
through a container is itself lifetime attachment — bytes placed in the
arena die with it, with no record.

## 7. Nursery

Lifecycle phases: OPEN → JOINING/LEFT → EMPTY → DEAD (see
`concurrent-c-spec-complete.md` §8.1).

`CCNursery !>(CCError) cc_arena_create_nursery(CCArena* a)` births a nursery into a live owner:
handle in the owner's storage, record attached. A null or dead arena aborts.
The nursery's teardown — one entry serving the record fn, the registered
destroy hook, and manual destruction — is: if dead, return; join all tasks;
release. Release destroys the nursery's embedded arena, running the walk
over everything created through the nursery's face, after the join on both
the waited (JOINING) and LEFT exit paths, and then zeroes the handle rather than
freeing it, so the husk outlives the owner's record.

`cc_nursery_create()` is the self-owned malloc handle, freed at release; no
record ever points at it. `parent.create_child()` is the cancel-tree nest
(parent handle required; empty host aborts).

A leave-capable nursery is self-owned. `n.leave()` consumes the handle
(OPEN → LEFT): the last child to exit reaches EMPTY then DEAD at an
unpredictable time on a worker thread, so no outside owner may hold a
destroy record for it — the record would fire on freed storage, and the
dead-state protocol cannot help when the handle's own storage is gone.
`n.leave(ctx, finish)` registers one leftover that runs at EMPTY on the LEFT
path only, then leaves. `leave` on an owner-attached nursery is refused; a
nursery that must be left is created with `cc_nursery_create()`. Deprecated:
`abandon` (`n.leave()`), `on_last` (leftover registration only; prefer
`n.leave(ctx, finish)`).

`n.close(tx)` arms this nursery's EMPTY to close `tx` on both the waited
and LEFT paths. It is not teardown and not a parent record — channels close
at EMPTY, a join-set signal, not a destroy obligation. Deprecated: `close_on`.

The nursery handle projects Region through the host:
`as: (Region)n;`.

## 8. Teardown order

Within one parent, children are destroyed newest-first. Create what tasks
or later children depend on before the thing that waits on or uses it —
resources before waiters. Creation order is the ordering contract; nothing
enforces it.

## 9. Non-goals

- Moving non-arena objects: `adopt` of other types, synthesized destroy
  chains for record fns.
- A tree-to-tree transfer verb; `adopt` from the current location covers
  it.
- Reparenting state on children: parent pointers, back-references.
- Ownership registries or global tracking; the dead-state protocol is the
  only mechanism.
- Coupling scope sigils to constructor calls in either direction.
