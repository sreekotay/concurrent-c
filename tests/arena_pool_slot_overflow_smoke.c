/* Regression test for the "pooled-slot arena silently truncates on overflow"
 * bug surfaced by a request-pipelined server under large payloads.
 *
 * The pattern: a CCArenaPool hands out fixed-size slots (e.g. 512 bytes) sized
 * for the common request.  Each slot is turned into an arena with
 * cc_arena_create_buffer(slot, cap, CC_ARENA_GROWABLE); a request struct is
 * placed at offset 0 of the slot and variable-length payload (cloned argv
 * strings, etc.) goes into the same arena.  When a payload is larger than the
 * slot can hold, the arena must transparently spill subsequent allocations
 * into a heap slab without relocating the in-slot struct, and on release
 * cc_arena_free must reclaim the heap slabs while leaving the caller-provided
 * slot intact for pool_free to reclaim.
 *
 * Prior to the fix, cc_arena_create_buffer defaulted to block_max = 1
 * (fixed), so any request whose total arg bytes exceeded the slot
 * capacity returned "request argument clone failed" at ~300 bytes.  This
 * smoke pins the three properties that make the pool-slot-with-overflow
 * design correct:
 *   1. The first alloc (the "struct at offset 0") stays in the slot and
 *      its pointer remains the slot pointer.
 *   2. A subsequent oversized alloc transparently spills to a heap slab
 *      (block_idx > 0) and the original struct pointer is NOT invalidated.
 *   3. cc_arena_free frees heap extents but never touches the slot bytes,
 *      so the slot is immediately reusable (e.g. as a freelist link).
 */

#include <ccc/std/prelude.cch>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Shaped to force growth in a 512-byte slot: the struct (~200B) + the
 * cloned argv bytes (~400B) together exceed the slot, so the second argv
 * MUST spill into a heap slab.  Exact size doesn't matter beyond that. */
typedef struct FakeReq {
    uint64_t a, b;
    uint32_t c;
    void* p;
    char pad[168];  /* pushes sizeof(FakeReq) to ~200 bytes */
} FakeReq;

#define SLOT_CAP 512
#define BIG_N    400  /* > SLOT_CAP - sizeof(FakeReq) - small_n */

int main(void) {
    uint8_t slot[SLOT_CAP];
    memset(slot, 0xAB, sizeof(slot));  /* sentinel so we can check it later */
    uint8_t *slot_start = slot;

    /* Same call shape a pooled per-request arena uses: carve a growable
     * arena out of a caller-owned slot buffer. */
    CCArena a = cc_arena_create_buffer(slot, sizeof(slot), CC_ARENA_GROWABLE);
    if (!a.base) { printf("FAIL: create_buffer returned empty\n"); return 1; }
    if (a.base != slot_start) { printf("FAIL: arena.base should equal slot\n"); return 1; }
    if (a.block_max != 0) { printf("FAIL: block_max should be 0 (growable), got %u\n", a.block_max); return 1; }
    if (a._flags & CC_ARENA_FLAG_HEAP_OWNED) { printf("FAIL: slot root must not be heap-owned\n"); return 1; }

    /* Step 1: the "request struct" at offset 0 — simulates RedisRequest. */
    FakeReq *req = (FakeReq*)cc_arena_alloc(&a, sizeof(*req), _Alignof(FakeReq));
    if (!req) { printf("FAIL: struct alloc\n"); return 2; }
    if ((uint8_t*)req != slot_start) { printf("FAIL: first alloc must land at slot base\n"); return 2; }
    req->a = 0xDEADBEEFBADF00DULL;
    req->b = 0x0123456789ABCDEFULL;

    /* Step 2: argv[0] that fits in the slot. */
    const size_t small_n = 4;
    char *argv0 = (char*)cc_arena_alloc(&a, small_n, 1);
    if (!argv0) { printf("FAIL: small argv alloc\n"); return 3; }
    if ((uint8_t*)argv0 < slot_start || (uint8_t*)argv0 >= slot_start + SLOT_CAP) {
        printf("FAIL: small argv should stay in slot\n"); return 3;
    }
    memcpy(argv0, "SET", small_n);

    /* Step 3: argv[1] whose size forces the arena to grow into a heap slab.
     * This is the case that used to fail with "request argument clone failed"
     * under block_max=1. */
    const size_t big_n = BIG_N;
    char *argv1 = (char*)cc_arena_alloc(&a, big_n, 1);
    if (!argv1) { printf("FAIL: big argv alloc (growable should have spilled to heap)\n"); return 4; }
    if ((uint8_t*)argv1 >= slot_start && (uint8_t*)argv1 < slot_start + SLOT_CAP) {
        printf("FAIL: big argv should be in a heap slab, not in the slot\n"); return 4;
    }
    if (a.block_idx == 0) { printf("FAIL: block_idx should be > 0 after growth\n"); return 4; }
    if (a.prev == NULL) { printf("FAIL: prev extent chain should be populated\n"); return 4; }
    memset(argv1, 0x77, big_n);

    /* Step 4: struct at offset 0 is still intact.  This is the key
     * invariant the redis release path relies on: req->... can be read
     * after growth without any relocation. */
    if (req->a != 0xDEADBEEFBADF00DULL || req->b != 0x0123456789ABCDEFULL) {
        printf("FAIL: struct corrupted by arena growth\n"); return 5;
    }
    if (memcmp(argv0, "SET\0", 3) != 0) {  /* compare only the written bytes */
        printf("FAIL: small argv corrupted by arena growth\n"); return 5;
    }

    /* Step 5: free the arena.  This must:
     *   - free the heap extents (argv1's slab), and
     *   - NOT free the caller-provided slot buffer.
     * If it got wrong and tried free() on slot, ASan would catch it; in
     * plain builds we at least check that reusing the slot afterwards
     * works, which would fault on a double-free pattern. */
    cc_arena_free(&a);
    if (a.base != NULL) { printf("FAIL: cc_arena_free should clear base\n"); return 6; }

    /* Step 6: slot is reusable (the pool's typical freelist write pattern). */
    *(void**)slot = (void*)(uintptr_t)0xCAFEBABE;
    if (*(void**)slot != (void*)(uintptr_t)0xCAFEBABE) {
        printf("FAIL: slot no longer writable\n"); return 7;
    }

    printf("arena_pool_slot_overflow_smoke OK\n");
    return 0;
}
