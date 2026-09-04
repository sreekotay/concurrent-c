# `CCArena` Allocation Strategy

Status: implemented

## Scope

The [main language specification](concurrent-c-spec-complete.md#5-arenas)
defines arena creation, growth policy, lifetime, slice provenance, and
checkpoint language semantics. This document defines the shipped individual
release and explicit heap-overflow strategy. Its checkpoint section states the
runtime behavior required by the main specification.

## Ownership handle

An arena names a lifetime. Storage is three tiers: **L1** (root slab),
**L2** (grown heap extents), **Main** (overflow). The constructor is an
implementation policy for those tiers. `CCArena` is the handle for that
lifetime and for arena-backed containers. Existing constructors and
container APIs take `CCArena*`; allocation does not require a separate
general allocator object. `cc_arena_live` counts live objects on all three
tiers.

## Constructors and root sizing

Teach three constructors:

```c
CCArena h = cc_arena_heap(N) @destroy;   /* request/window scratch */
cc_arena_stack(s, N);                    /* same policy; stack L1; @destroy at scope exit */
cc_arena_buf(s, ptr, N);                 /* same sugar; caller L1 (no VLA) */
CCArena m = cc_arena_malloc(N) @destroy; /* durable: fixed root + overflow */
```

`cc_arena_heap` / `cc_arena_stack` use L1 capacity exactly `N`,
`block_max = CC_ARENA_DEFAULT_BLOCK_MAX` (4), and Main overflow on: up to four
slabs with 1.5× L2 growth, then Main malloc overflow. Size `N` for the typical
request live set (about 16MiB L1 covers roughly 100MiB-class live under the
default budget). A tiny L1 still allocates, but most traffic becomes Main
(higher alloc cost and reset drain). Main allocations remain arena-owned and
are freed by `cc_arena_reset` / `cc_arena_free`.

`cc_arena_malloc` is fixed-root (`block_max = 1`) with overflow on and no extent
growth. Prefer it when live entries are released individually from a durable
store. Do not use it as general scratch for large keep-alive alloc storms —
prefer `cc_arena_heap` / `cc_arena_stack` with an appropriately sized root.

Caller-backed expert forms use an explicit block policy:

```c
CCArena fixed = cc_arena_create_buffer(buf, sizeof buf, CC_ARENA_FIXED);
CCArena growable =
    cc_arena_create_buffer(buf, sizeof buf, CC_ARENA_GROWABLE);
```

`CC_ARENA_FIXED` limits allocation to the root block.
`CC_ARENA_GROWABLE` permits an unbounded chain of heap-owned extents. A value
greater than one bounds the total slab count; when heap overflow is enabled,
allocation spills to malloc after that budget.

When the current block cannot satisfy an allocation and both ordinary growth
and heap overflow are unavailable, allocation returns `NULL`. In particular,
exhaustion of a fixed arena with heap overflow disabled returns `NULL`.

## Heap overflow

Heap overflow is an explicit fallback beyond the arena's block policy:

```c
bool cc_arena_set_heap_overflow(CCArena arena, bool enabled);
```

The operation fails for an invalid arena. Disabling overflow after the arena has
used it also fails. When allocation exhausts the current block, `cc_arena_alloc`
first follows the ordinary growth policy. If growth fails or reaches its block
budget and overflow is enabled, the allocation uses `malloc` and is accounted
in the arena's outstanding overflow bytes.

Overflow allocation stamps the current arena provenance epoch on the overflow
header and does not itself make the epoch non-rewindable. Overflow pointers
remain individually releasable and reallocatable through the arena via
`cc_arena_release` / `cc_arena_realloc`. Realloc preserves the object's
mint epoch. `cc_arena_reset` and `cc_arena_free` / `cc_arena_destroy` also
free every outstanding overflow allocation (Main). Using a pre-reset
overflow pointer afterward is undefined behavior, same as using a pre-reset
slab pointer. Ownership is fail-closed: each overflow payload carries a
header (`CC_ARENA_OVF_MAGIC` / `CC_ARENA_OVF_MAGIC_CHUNK`); a foreign,
stale, or already-released pointer is refused (`false` / `NULL`) rather
than passed to `free`/`realloc`. Per-object release unlinks and frees
immediately; after `free` the header is gone, so a later double-release is
undefined if the bytes have been reused. Chunk release punches a `DEAD`
hole and leaves the chunk allocated until reset/restore.

## Individual release

```c
bool cc_arena_release(CCArena arena, void *ptr);
```

```c
bool cc_arena_release_sized(CCArena arena, void *ptr, size_t size);
bool cc_arena_set_reuse(CCArena arena, bool enabled);
```

Release is a signal: the caller says the object is over, the strategy decides
what the bytes become. The caller passes a live pointer owned through that
arena (or any active child of it), with the size when it knows it. A pointer
in an arena slab decrements that slab's live-allocation count. A sized
release of the active tip pops the tip. On a reuse host, a sized release of
a block that fits a size class (16 bytes to 32 KiB, doubling) lists it for a
later request of that class; a listed block stays counted live, and a later
request of the class at 16-byte alignment or less takes it instead of
bumping. Any other slab release is a hole until reset. Releasing the last
live allocation on the root slab rewinds its bump offset to zero. Enabling
reuse allocates the class table from the host; disabling drops the lists.

A pointer the arena does not own is refused (`false`) and nothing changes:
a foreign pointer, a pointer at or past the tip (already popped or from a
restored child), a size that cannot fit below the tip, or a slab with no
live allocations. The bump tier carries no per-object header, so a second
hole release of the same block is indistinguishable from the first; owners
carry a token for that case.

When heap overflow is enabled, a pointer outside the arena's slab chain takes
the overflow-header path. The caller must pass a live overflow allocation
obtained through the same arena.

A hole never makes anything non-rewindable: checkpoints, children, and
detach are unaffected by releases. Whole-arena `cc_arena_reset`,
`cc_arena_free`, and `cc_arena_destroy` remain distinct lifecycle operations.

`cc_arena_realloc` preserves the shared prefix of the old and new sizes. When
a slab allocation sits at the active bump tip (`ptr + old_size` equals the
current offset) and the new size still fits the active block, realloc extends
or shrinks the tip in place with no copy. Otherwise slab realloc allocates,
copies, and releases. An overflow allocation may use `realloc`. A cross-arena
reallocation allocates from the destination arena, copies, then releases
through the source arena.

## Checkpoint and restore

A checkpoint is a mark on the innermost active host:

```c
CCArenaCheckpoint checkpoint = arena.try_checkpoint() !>;
checkpoint.try_restore() !>; /* or @destroy on the handle */
```

Capture records the slab word, takes a fresh epoch that every allocation
above the mark is stamped with, and remembers the head of the host's record
list. Allocation keeps bumping the host's own slab. Marks nest per host up to
`CC_ARENA_MARK_DEPTH`; a deeper one promotes the outer marks. An arena its
caller owns outright uses `cc_arena_checkpoint_local` / `cc_arena_restore_local`
(the `alloc_local` contract): no lock, a plain store on restore.

Restore refuses while an inner mark is still named by a live handle, runs the
records attached since the mark, then returns the slab word to the mark in one
CAS. Views minted above the mark fail the epoch check afterwards; pre-mark
views keep theirs. The tip never drops below a mark: a pre-mark object at the
tip released during scratch becomes a hole, and the live count returns to the
mark's count (a pre-mark release during scratch leaves it one high, which can
only defer a rewind). Abandon disarms the mark and keeps its scratch until the
enclosing restore or the host's reset or free. `@destroy` restores, else
abandons. A dropped handle leaves the mark armed.

A mark becomes a child arena, a promotion, only when it must: scratch
outgrows the slab or spills to overflow, a pre-mark object needs to move (a
regrow that would cross the mark), parent-side storage is needed (a record
for a durable object, the reuse class table), or marks nest past the stack.
The range from the outermost mark to the tip becomes the child's L1 in
place, the host's tip parks at the slab end with the child counted once, the
inner marks and the records attached since the outermost mark move to the
child, and the child becomes active. The child has its own provenance epoch
(the mark's), slab budget, and overflow, inheriting the parent's overflow
permission and hard cap. Fresh allocations through the parent land in the
innermost active child; realloc and release resolve the owner on the active
chain and act there, so a pre-mark object regrows in host storage (a fresh
slab of its host while the tip is parked). An owner header, a record, or a
moved payload belongs to the host whose bytes hold it, whichever handle it
was created through. A checkpoint taken while a child is active nests inside
it. Restore of a promoted mark frees the child (extents, overflow, epoch) and
pops the host tip back to the mark.

Detach and adopt refuse a host with an armed mark or an active child and
refuse an active child. `cc_arena_reset` tears down marks and active children
with the other records, frees outstanding overflow (Main), clears the
used-overflow and reuse flags, returns to the original L1, resets counts and
offset, and advances provenance. `cc_arena_detach` refuses a stack or
caller-owned L1.

## Owners

```c
CCArenaOwner *cc_arena_owner_new(CCArena arena, size_t bytes, size_t align);
bool  cc_arena_owner_live(const CCArenaOwner *o, uint32_t token);
void *cc_arena_owner_regrow(CCArenaOwner *o, uint32_t token, size_t bytes);
bool  cc_arena_owner_release(CCArenaOwner *o, uint32_t token);
uint64_t cc_arena_owner_slice_id(const CCArenaOwner *o);
```

An owner is a header in the owning host's slab tier — arena, payload, bytes,
alignment, provenance, token — split from its payload, which the strategy
supplies. A header minted together with its payload pops with it when the
payload still ends the slab (one CAS, nothing listed). Otherwise the released
header goes on that host's owner list and is reborn with a fresh token by the
next owner minted there; a listed header is never returned to the bump while
the arena lives. Tokens come from the slice generation registry,
so a view id and a handle carry the same token. `regrow` keeps the token on a
tip fit and rebirths it on a move; `release` kills it. Every operation that
takes a token refuses when the header's token differs, so a handle that
outlived a move or another handle's release mismatches instead of touching
bytes that belong to someone else.

## Arena-backed containers

Arena-backed containers keep `CCArena` in their existing APIs and always
release what they own, with the size:

- `CCVec` and heap `CCString` are owners: the handle carries the owner's
  token; growth regrows through the owner; every push, indexed write, view,
  and destroy checks the token.
- Arena-backed maps release replaced table storage during resize, and map
  destruction releases both the table and the arena-backed handle. The
  map handle is a pointer that names released storage afterwards.
- `clear` keeps capacity; destruction releases the payload.
- `Vec` `from` wraps caller storage and has no owner.
- `CCString` registers `@destroy`; its two-argument `release` is kept and
  ignores the arena argument.

Container growth keeps only the current payload live when the strategy can
reclaim the replaced one (a tip pop or a listed block); otherwise the old
payload is a hole until reset.
