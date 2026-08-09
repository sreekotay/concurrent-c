/*
 * FFI adopt registry: alloc_id → {base, deleter} for unique non-transferable
 * slices from cc_adopt(). Linked via concurrent_c.c.
 */
#include <ccc/cc_slice.cch>
#include <ccc/cc_atomic.cch>
#include <pthread.h>
#include <stdlib.h>

typedef struct CCAdoptNode {
    uint64_t alloc_id;
    void* base;
    CCSliceDeleter deleter;
    struct CCAdoptNode* next;
} CCAdoptNode;

static pthread_mutex_t cc__adopt_mu = PTHREAD_MUTEX_INITIALIZER;
static CCAdoptNode* cc__adopt_head = NULL;
static cc_atomic_u64 cc__adopt_next_id = 2; /* avoid 0 / start above 1 */

static uint64_t cc__adopt_mint_id(void) {
    for (;;) {
        uint64_t id = (uint64_t)cc_atomic_fetch_add(&cc__adopt_next_id, 1);
        id &= CC_SLICE_ID_ALLOC_MASK;
        if (id == 0 || id == CC_SLICE_ID_CANONICAL_ALLOC) continue;
        return id;
    }
}

static int cc__adopt_register(uint64_t alloc_id, void* base, CCSliceDeleter deleter) {
    CCAdoptNode* n = (CCAdoptNode*)malloc(sizeof(CCAdoptNode));
    if (!n) return -1;
    n->alloc_id = alloc_id;
    n->base = base;
    n->deleter = deleter;
    pthread_mutex_lock(&cc__adopt_mu);
    n->next = cc__adopt_head;
    cc__adopt_head = n;
    pthread_mutex_unlock(&cc__adopt_mu);
    return 0;
}

static int cc__adopt_take(uint64_t alloc_id, void** out_base, CCSliceDeleter* out_del) {
    int found = 0;
    pthread_mutex_lock(&cc__adopt_mu);
    CCAdoptNode** pp = &cc__adopt_head;
    while (*pp) {
        if ((*pp)->alloc_id == alloc_id) {
            CCAdoptNode* n = *pp;
            *pp = n->next;
            if (out_base) *out_base = n->base;
            if (out_del) *out_del = n->deleter;
            free(n);
            found = 1;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&cc__adopt_mu);
    return found;
}

CCSliceUnique cc_adopt(void* ptr, size_t nbytes, CCSliceDeleter deleter) {
    if (!ptr && nbytes == 0) {
        return cc_slice_from_parts(NULL, 0,
                                   cc_slice_make_id(0, true, false, false));
    }
    if (!ptr) return cc_slice_empty();

    uint64_t alloc_id = cc__adopt_mint_id();
    if (cc__adopt_register(alloc_id, ptr, deleter) != 0) {
        /* OOM registering — still hand back a unique view; caller must free. */
        return cc_slice_from_parts(ptr, nbytes,
                                   cc_slice_make_id(alloc_id, true, false, false));
    }
    return cc_slice_from_parts(ptr, nbytes,
                               cc_slice_make_id(alloc_id, true, false, false));
}

void cc_slice_destroy(CCSlice* s) {
    if (!s) return;
    if (!cc_slice_is_unique(*s)) {
        *s = cc_slice_empty();
        return;
    }
    uint64_t alloc_id = cc_slice_alloc_id(s->id);
    void* base = NULL;
    CCSliceDeleter del = NULL;
    if (alloc_id != 0 && cc__adopt_take(alloc_id, &base, &del)) {
        if (del && base) del(base);
    }
    *s = cc_slice_empty();
}
