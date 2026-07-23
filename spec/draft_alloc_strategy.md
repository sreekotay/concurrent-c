# `CCArena` Allocation Strategy

Status: draft — implemented

## Ownership handle

`CCArena` is the ownership handle for arena allocations and arena-backed
containers. Existing constructors and container APIs continue to take
`CCArena*`; allocation does not require a separate general allocator object.

Heap-created arenas grow by default. Caller-backed arenas use an explicit block
policy:

```c
CCArena heap = cc_arena_heap(4096);
CCArena fixed = cc_arena_create_buffer(buf, sizeof buf, CC_ARENA_FIXED);
CCArena growable =
    cc_arena_create_buffer(buf, sizeof buf, CC_ARENA_GROWABLE);
```

`CC_ARENA_FIXED` limits allocation to the root block.
`CC_ARENA_GROWABLE` permits an unbounded chain of heap-owned extents. A value
greater than one bounds the total block count.

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

Overflow allocation makes the current arena epoch non-rewindable. Overflow
pointers remain individually releasable and reallocatable through the arena.
`cc_arena_reset` and `cc_arena_free` do not release outstanding overflow
pointers; the caller releases them before reset or teardown.

## Individual release

```c
bool cc_arena_release(CCArena *arena, void *ptr);
```

The caller passes a live pointer owned through that arena. A pointer in an arena
slab decrements that slab's live-allocation count. Releasing the last live
allocation in the current slab resets its bump offset, making the slab reusable.
A recognized double release or live-count mismatch returns `false` and reports
a diagnostic.

When heap overflow is enabled, a pointer outside the arena's slab chain is
treated as an overflow allocation, freed with `free`, and removed from overflow
accounting. Because this path has no per-allocation ownership table, passing a
foreign, stale, or already released pointer violates the caller contract.

Any successful individual release makes the current arena epoch
non-rewindable. Whole-arena `cc_arena_reset`, `cc_arena_free`, and
`cc_arena_destroy` remain distinct lifecycle operations.

`cc_arena_realloc` preserves the shared prefix of the old and new sizes. Slab
allocations allocate, copy, and release. An overflow allocation may use
`realloc`. A cross-arena reallocation allocates from the destination arena,
copies, then releases through the source arena.

## Checkpoint and restore

Checkpoint and restore operate only while allocation is monotonic:

```c
CCArenaCheckpoint checkpoint = cc_arena_checkpoint(&arena);
cc_arena_restore(checkpoint);
```

A rewindable checkpoint records the active block, offset, and provenance epoch.
Restore discards newer growth blocks, restores the saved offset and provenance,
and invalidates allocations made after the checkpoint.

After heap overflow or individual release makes an epoch non-rewindable,
`cc_arena_checkpoint` returns a null checkpoint with
`checkpoint.arena == NULL`. Restoring a null checkpoint is a no-op. Restoring
any checkpoint while its arena is non-rewindable is also a no-op.
`cc_arena_reset` clears the non-rewindable and used-overflow flags, returns to
the original block, resets allocation counts and offset, advances provenance,
and enables checkpointing for the new epoch.

## Arena-backed containers

Arena-backed containers retain `CCArena*` in their existing APIs and use the
release path automatically:

- `CCVec` and `CCString` release replaced backing allocations during growth.
- Arena-backed maps release replaced table storage during resize.
- Map destruction releases both table storage and the arena-backed map handle.
- `clear` may retain capacity; destruction releases backing allocations.

Container growth therefore keeps only the current backing allocation live when
the arena's release accounting can reclaim the replaced allocation.
