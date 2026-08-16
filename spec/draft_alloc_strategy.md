# `CCArena` Allocation Strategy

Status: implemented

## Scope

The [main language specification](concurrent-c-spec-complete.md#5-arenas)
defines arena creation, growth policy, lifetime, slice provenance, and
checkpoint language semantics. This document defines the shipped individual
release and explicit heap-overflow strategy. Its checkpoint section states the
runtime behavior required by the main specification.

## Ownership handle

An arena names a lifetime. Its allocation strategy is an implementation
policy for storage belonging to that lifetime. `CCArena` is the handle for
that lifetime and for arena-backed containers. Existing constructors and
container APIs take `CCArena*`; allocation does not require a separate
general allocator object.

## Constructors and root sizing

Teach three constructors:

```c
CCArena h = cc_arena_heap(N) @destroy;   /* request/window scratch */
cc_arena_stack(s, N);                    /* same policy; root on the stack */
CCArena m = cc_arena_malloc(N) @destroy; /* durable: fixed root + overflow */
```

`cc_arena_heap` / `cc_arena_stack` use root capacity exactly `N`,
`block_max = CC_ARENA_DEFAULT_BLOCK_MAX` (4), and heap overflow on: up to four
slabs with 1.5× growth, then tier-3 malloc overflow. Size `N` for the typical
request live set (about 16MiB root covers roughly 100MiB-class live under the
default budget). A tiny root still allocates, but most traffic becomes overflow
(higher alloc cost and reset drain). Overflow allocations remain arena-owned and
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
bool cc_arena_set_heap_overflow(CCArena *arena, bool enabled);
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
free every outstanding overflow allocation (tier 3). Using a pre-reset
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
bool cc_arena_release(CCArena *arena, void *ptr);
```

The caller passes a live pointer owned through that arena. A pointer in an arena
slab decrements that slab's live-allocation count. Releasing the last live
allocation in the current slab resets its bump offset, making the slab reusable.
A recognized double release or live-count mismatch returns `false` and reports
a diagnostic.

When heap overflow is enabled, a pointer outside the arena's slab chain takes
the overflow-header path. The caller must pass a live overflow allocation
obtained through the same arena.

A mid-slab release (any successful slab release that is not the last live
allocation on the root slab) makes the current arena epoch non-rewindable.
Overflow release does not. Restore of a checkpoint whose `ovf_keep` no
longer matches the live overflow count for that epoch refuses. Whole-arena
`cc_arena_reset`, `cc_arena_free`, and `cc_arena_destroy` remain distinct
lifecycle operations.

`cc_arena_realloc` preserves the shared prefix of the old and new sizes. When
a slab allocation sits at the active bump tip (`ptr + old_size` equals the
current offset) and the new size still fits the active block, realloc extends
or shrinks the tip in place with no copy. Otherwise slab realloc allocates,
copies, and releases. An overflow allocation may use `realloc`. A cross-arena
reallocation allocates from the destination arena, copies, then releases
through the source arena.

## Checkpoint and restore

Checkpoint and restore operate while the slab prefix is intact (no mid-slab
hole). Overflow keep-set puncture is detected at restore, not by disabling
a later `checkpoint()`:

```c
CCArenaCheckpoint checkpoint = cc_arena_checkpoint(&arena);
cc_arena_restore(checkpoint);
```

A rewindable checkpoint records the active block, offset, root-slab
`live_allocs`, live overflow count for that epoch (`ovf_keep`), and current
provenance epoch, then advances the arena to a fresh provenance epoch for
subsequent allocations and seals the active overflow chunk. Restore discards
newer growth blocks, writes back the saved offset and live count, restores
provenance, and frees overflow whose header epoch does not match the
checkpoint. Slices minted from the later epoch become stale; pre-checkpoint
slices retain the restored epoch.

After a mid-slab hole, `cc_arena_checkpoint` returns a null checkpoint with
`checkpoint.arena == NULL`. `cc_arena_restore` returns false and does not
mutate on a null handle, a slab hole, an `ovf_keep` mismatch (keep-set
object released), or a checkpoint that would advance the tip. Last-live
root release may clear a slab hole; it does not make a punctured keep-set
restorable. Dropping a checkpoint handle does not block a later capture.
`cc_arena_reset` frees outstanding overflow (tier 3), clears the
non-rewindable and used-overflow flags, returns to the original block, resets
allocation counts and offset, advances provenance, and enables checkpointing
for the new epoch.

## Arena-backed containers

Arena-backed containers retain `CCArena*` in their existing APIs and use the
release path automatically:

- `CCVec` and `CCString` release replaced backing allocations during growth.
- Arena-backed maps release replaced table storage during resize.
- Map destruction releases both table storage and the arena-backed map handle.
- `clear` may retain capacity; destruction releases backing allocations.

Container growth therefore keeps only the current backing allocation live when
the arena's release accounting can reclaim the replaced allocation.
