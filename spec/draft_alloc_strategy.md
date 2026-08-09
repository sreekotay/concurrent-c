# `CCArena` Allocation Strategy

Status: implemented

## Scope

The [main language specification](concurrent-c-spec-complete.md#5-arenas)
defines arena creation, growth policy, lifetime, slice provenance, and
checkpoint language semantics. This document defines the shipped individual
release and explicit heap-overflow strategy. Its checkpoint section states the
runtime behavior required by the main specification.

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
greater than one bounds the total slab count; when heap overflow is enabled,
allocation spills to malloc after that budget. `cc_arena_heap` /
`cc_arena_stack` default to `CC_ARENA_DEFAULT_BLOCK_MAX` (4) with overflow on.

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

Overflow allocation makes the current arena epoch non-rewindable. Overflow
pointers remain individually releasable and reallocatable through the arena
via `cc_arena_release` / `cc_arena_realloc`. `cc_arena_reset` and
`cc_arena_free` / `cc_arena_destroy` also free every outstanding overflow
allocation (tier 3). Using a pre-reset overflow pointer afterward is undefined
behavior, same as using a pre-reset slab pointer.

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
the permissive overflow path: `free(ptr)` is performed and the available size
information is removed from overflow accounting. By default this path has no
per-allocation ownership table and cannot verify ownership. The caller must
pass a live overflow allocation obtained through the same arena. Passing a
foreign, stale, or already released pointer, including a double release, is
undefined behavior even if `cc_arena_release` returns `true`.

Define `CC_DEBUG_ARENA_OVERFLOW_OWNERSHIP` to a non-zero value to record each
overflow allocation against its arena and refuse overflow-path
`cc_arena_release` / `cc_arena_realloc` for pointers not so recorded (returns
`false` / `NULL` with a diagnostic instead of calling `free`/`realloc`).

Any successful individual release makes the current arena epoch
non-rewindable. Whole-arena `cc_arena_reset`, `cc_arena_free`, and
`cc_arena_destroy` remain distinct lifecycle operations.

`cc_arena_realloc` preserves the shared prefix of the old and new sizes. When
a slab allocation sits at the active bump tip (`ptr + old_size` equals the
current offset) and the new size still fits the active block, realloc extends
or shrinks the tip in place with no copy. Otherwise slab realloc allocates,
copies, and releases. An overflow allocation may use `realloc`. A cross-arena
reallocation allocates from the destination arena, copies, then releases
through the source arena.

## Checkpoint and restore

Checkpoint and restore operate only while allocation is monotonic:

```c
CCArenaCheckpoint checkpoint = cc_arena_checkpoint(&arena);
cc_arena_restore(checkpoint);
```

A rewindable checkpoint records the active block, offset, and current
provenance epoch, then advances the arena to a fresh provenance epoch for
subsequent allocations. Restore discards newer growth blocks and restores the
saved offset and provenance. Slices minted from the later epoch become stale;
pre-checkpoint slices retain the restored epoch.

After heap overflow or individual release makes an epoch non-rewindable,
`cc_arena_checkpoint` returns a null checkpoint with
`checkpoint.arena == NULL`. Restoring a null checkpoint is a no-op. Restoring
any checkpoint while its arena is non-rewindable is also a no-op.
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
