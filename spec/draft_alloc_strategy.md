# Draft Alloc Strategy

Status: experimental side spec  
Scope: allocator/lifetime draft for `CCArena`, existing stdlib containers, and future collection work  
Normativity: non-normative unless and until folded into the main spec

## Purpose

This document captures the current draft allocator direction discussed during implementation work. It is intentionally a side spec rather than a change to the main language or stdlib spec because the primary goal is to evaluate ergonomics in real code before freezing the semantics.

The central question is:

- can Concurrent-C keep `CCArena` as the primary user-facing lifetime object,
- while making allocation and cleanup feel more flexible and less brittle,
- without immediately rewriting all container APIs around a new allocator abstraction?

The current draft answer is yes: prototype the new behavior on `CCArena` first, then decide later whether a true `CCAlloc`-style abstraction or a broader container rewrite is still needed.

## High-Level Direction

The draft strategy keeps the existing arena-first model:

- containers still accept `CCArena*`,
- allocation is still primarily arena-based,
- normal arena growth policy still runs first,
- heap fallback is explicit and opt-in,
- per-allocation cleanup is exposed through the arena surface.

The current prototype adds two new ideas:

1. `cc_arena_set_heap_overflow(CCArena *arena, bool enabled)`
2. `cc_arena_release(CCArena *arena, void *ptr)`

The intended user feel is:

- allocate through the arena,
- release through the arena,
- keep whole-arena `reset()` / `destroy()` semantics,
- let the implementation decide whether a given allocation is arena-backed, heap-fallback-backed, or immediately recyclable.

## Why This Draft Exists

This direction was chosen to avoid premature API churn.

A full allocator-generic transition would require threading a new allocator handle through:

- `Vec`
- `Map`
- `String`
- codegen/lowering helpers
- parser-mode stubs
- tests, docs, and examples

That would force allocator design questions into every call site up front.

Instead, this draft treats `CCArena` as the stable public handle and experiments with richer allocation behavior behind that API.

## Arena Behavior

### 1. `CCArena` remains the public handle

The draft does not replace `CCArena` with a general allocator object.

Existing call shapes such as:

```c
CCArena arena = cc_arena_heap(4096);
Vec<int> v = vec_new<int>(&arena);
Map<char[:], int> m = map_new<char[:], int>(&arena);
```

remain valid.

### 2. Heap overflow is explicit

Heap fallback is not enabled by default in this draft.

Callers must opt in explicitly:

```c
cc_arena_set_heap_overflow(&arena, true);
```

This is deliberate:

- easy to audit,
- easy to remove or redesign later,
- avoids silently changing existing arena behavior.

### 3. Normal arena policy runs first

Heap fallback is not the first choice.

When `cc_arena_alloc()` cannot satisfy an allocation in the current block:

1. try normal arena growth policy first,
2. only if that fails and heap overflow is enabled,
3. allocate from heap fallback storage.

This preserves the arena-first design and makes heap fallback a pressure-release valve, not a replacement for normal arena behavior.

### 4. Release is per-allocation hygiene

The draft introduces:

```c
bool cc_arena_release(CCArena *arena, void *ptr);
```

This is separate from whole-arena teardown:

- `cc_arena_release(...)` is for an individual allocation,
- `cc_arena_free(...)` / `cc_arena_destroy(...)` remain whole-arena lifecycle operations.

The intended semantics are:

- if the arena recognizes the pointer as one of its tracked live allocations, release it,
- if the pointer corresponds to heap-fallback allocation owned by the arena, reclaim it,
- if heap-overflow mode is enabled and the pointer is not tracked as arena-backed, permissive fallback behavior is currently allowed in the prototype,
- bad inputs and double-release should be surfaced via runtime reporting/assert-style diagnostics.

This part is explicitly still experimental.

### 5. Rewind semantics are weakened once release/spill behavior is used

Classic checkpoint/restore semantics assume monotonic arena allocation.

Once the arena has:

- used heap overflow, or
- begun relying on allocation-level release tracking,

the arena is considered non-rewindable for the remainder of that epoch.

Current draft rule:

- `cc_arena_checkpoint()` refuses or returns an invalid checkpoint for non-rewindable arenas,
- `cc_arena_restore()` is ignored/rejected for such checkpoints,
- `cc_arena_reset()` clears the state and restores rewindability.

So the mental model is:

- fresh arena: checkpoint-safe,
- spilled/releasing arena: usable, but not checkpoint-safe,
- reset arena: checkpoint-safe again.

## Existing Containers

An important part of the draft is that existing stdlib containers should participate in release behavior automatically.

The user should not need to manually chase container internals.

### Vec / String

`CCVec` now releases old backing storage when growth replaces it.

That means:

- allocate new buffer from the arena,
- copy existing elements,
- call `arena.release(old_buffer)`.

Since `CCString` is a `CCVec_char`, string growth inherits the same behavior.

This preserves the current public API while making transient growth buffers releasable earlier.

### Map

The khashl-backed map implementation already has allocator hooks:

- `Kmalloc`
- `Kcalloc`
- `Krealloc`
- `Kfree`

Under this draft, those hooks should flow through arena release semantics where possible.

Current direction:

- map reallocation allocates a replacement payload,
- copies existing content,
- releases the old allocation through the arena,
- map destroy also routes allocator teardown through the arena path.

This is important because `Map` is one of the main motivations for avoiding a full “replace all containers now” rewrite.

**Heap sidecar map (`<ccc/std/map_heap.cch>`):** khashl’s `km` context is overloaded with a reserved sentinel (`CC__MAP_HEAP_KM` in `map.cch`) so the same vendor header and hook macros can route map bucket storage to `malloc`/`realloc`/`free` while arena-backed maps still pass a real `CCArena*`. Declarations use `CC_MAP_DECL_HEAP` / `CC_MAP_DECL_HEAP_SLICE`; see `tests/map_heap_smoke.c`.

**Redis Jackson Allan CC experiment (user space):** `scripts/cc_vendor_jackson_namespace.py` generates `third_party/jackson-allan-cc/cc_for_ccc.h` from upstream `cc.h` by renaming every `cc_` API token to `ccj_`, eliminating clashes with Concurrent-C’s own `cc_*` / `cc_vec_*` runtime. The Redis DB map declaration now lives directly in `real_projects/redis/redis_idiomatic.ccs`.

### Clear vs destroy

This draft does not require every container operation to eagerly release all backing storage immediately.

In particular:

- `clear()` may preserve capacity,
- `destroy()` should release backing allocations,
- growth/reallocation paths should release superseded storage.

This matches the current stdlib direction better than forcing every `clear()` to become a hard allocator event.

## Relationship to a Future `CCAlloc`

This draft does not reject a future `CCAlloc`.

It postpones it.

Potential future direction:

- a first-class allocator interface,
- allocator-backed container constructors,
- arena adapters,
- perhaps alternate container backends.

But that should only happen if the `CCArena`-centric prototype still feels insufficient after real use.

The current hypothesis is:

- heap overflow plus release on `CCArena`,
- plus container participation in release,

may solve enough of the ergonomic pain that a broad allocator-generic transition is no longer obviously worth the churn.

## Relationship to Scratch / Temporary Allocation

A separate line of thought explored during this draft is whether arena allocations could support cheap block reuse when all allocations in a block have been released.

That idea looks like:

- per-block live allocation counts,
- per-allocation release bookkeeping,
- block becomes reusable when live count reaches zero.

This document does not make that behavior normative.

It is recorded here as a plausible future extension because it fits the same `release(...)` surface and may provide useful lightweight temporary/scratch semantics.

For now, the important point is that `release(...)` is not only about heap fallback; it is also a possible path toward more flexible arena-backed scratch allocation.

## Current Draft Principles

1. Keep `CCArena` as the user-facing ownership object.
2. Avoid forcing allocator choices through all call sites.
3. Make heap overflow explicit, not implicit.
4. Run normal arena growth before heap fallback.
5. Keep whole-arena lifecycle (`reset`, `destroy`) separate from per-allocation release.
6. Let existing containers participate automatically.
7. Prefer ergonomic experimentation first; harden semantics after real usage.

## Open Questions

The following remain intentionally unresolved:

1. How permissive should `cc_arena_release()` be for foreign pointers in non-debug builds?
2. Should release tracking remain out-of-line, or move to inline metadata later if performance or memory pressure demands it?
3. Should heap-fallback allocations be tracked more strictly instead of permitting libc-style fallback in the prototype?
4. Should block-level recycling become part of ordinary `CCArena`, or a distinct scratch-arena mode?
5. How much of this should eventually become normative in `concurrent-c-stdlib-spec.md` and `concurrent-c-spec-complete.md`?

## Current Implementation Notes

As of this draft:

- `CCArena` has explicit heap-overflow opt-in,
- `CCArena` has `release(...)`,
- checkpointing is gated once release/spill semantics are active,
- `Vec` releases superseded storage on growth,
- `Map` allocator hooks route old storage through arena release semantics,
- the public `Vec`, `Map`, and `String` APIs remain arena-based.

Those implementation details are useful for discussion, but this file is primarily intended to record the design direction rather than freeze every exact runtime detail.
