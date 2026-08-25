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
| free child | `owner->create_arena(0)` — heap L1 | yes — may move again | owner's walk |

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
attaches. List mutation is serialized by the arena's meta lock.

Destroying a parent first **walks** its records, newest first (LIFO),
before the arena releases overflow or slabs — child handles may live in
either. Each record runs its destroy fn on its object; dead objects no-op.
The walk runs exactly once per parent destruction and clears the list.

`cc_arena_reset` runs the same walk before rewinding: attached children
are contents, and contents die at reset — their handles live in the slabs
being rewound, exactly like every unhooked allocation. The arena stays
live and may attach again afterward.

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

The primitive under adopt and the `create_*` constructors: appends a
record. The caller supplies the destroy fn — for user types, the function
registered as the type's destroy hook. Fails, nonzero with a report, on a
dead or mid-walk parent.

### `create_*` constructors

A constructor named `create_<thing>(Owner*, ...)` births an object into an
owner: handle allocated from the owner, initialized, attached, returned as
a pointer. The prefix is load-bearing: parent views admit `create_*` by
glob (§6), and UFCS bare-name dispatch gives every such function the
`owner.create_<thing>(...)` spelling with no further declaration.
Constructors are declared in the product's header:

```ccs
CCArena !>(CCError) create_arena(CCArena owner, size_t n);         // cc_arena.cch
CCArenaPool* create_pool(CCArena owner, size_t elem);              // cc_arena.cch
CCNursery !>(CCError) cc_arena_create_nursery(CCArena* a);     // cc_nursery.cch
Session*     create_session(CCArena owner, int fd);                // user code
```

`create_arena(owner, n)` with `n > 0` carves the child's first slab from
the owner: the child is storage-bound and cannot move. With `n == 0` the
child's slabs are heap-owned (ordinary `cc_arena_heap` growth): the child
is free and may later be detached or adopted elsewhere. The size argument
selects the storage class.

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

`CCNursery !>(CCError) cc_arena_create_nursery(CCArena* a)` births a nursery into a live owner:
handle in the owner's storage, record attached. A null or dead arena aborts.
The nursery's teardown — one entry serving the record fn, the registered
destroy hook, and manual destruction — is: if dead, return; join all tasks;
release. Release destroys the nursery's embedded arena, running the walk
over everything created through the nursery's face, after the join on both
the waited and abandoned exit paths, and then zeroes the handle rather than
freeing it, so the husk outlives the owner's record.

`cc_nursery_create()` is the self-owned malloc handle, freed at release; no
record ever points at it. `parent.create_child()` is the cancel-tree nest
(parent handle required; empty host aborts).

An abandon-capable nursery is self-owned. `abandon` hands the handle to the
children: the last one to exit frees it at an unpredictable time on a worker
thread, so no outside owner may hold a destroy record for it — the record
would fire on freed storage, and the dead-state protocol cannot help when
the handle's own storage is gone. `abandon` on an owner-attached nursery
aborts; a nursery that must be abandoned is created with
`cc_nursery_create()`.

The `closing(ch)` list is not a parent record: channels close **after
wait** — a join signal, not a destroy obligation. It is unchanged.

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
