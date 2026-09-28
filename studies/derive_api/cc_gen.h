/*
 * cc_gen.h: the lowered form of derived state. Plain C; no compiler support.
 *
 *   CCGen        a source's generation: "has this changed since"
 *   CCStamp      the inputs a derived value was last built from
 *   CCStateId    a state's identity: "is this the same state" (not freshness)
 *   CC_GENLOG    a generation plus the last N typed edits (optional)
 *
 * Every number comes from cc_gen_next(): nonzero, never repeated in the
 * process. That makes a whole-object copy carry a meaningful generation:
 * restoring a full snapshot restores content and generation together.
 */
#ifndef CC_GEN_H
#define CC_GEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ---- unique numbers ------------------------------------------------- */
/* One counter per process, defined by exactly one translation unit:
 * #define CC_GEN_IMPL before including this header there. (A static
 * counter inside the inline function below would be one per translation
 * unit, and two units storing to the same source could give it the same
 * number twice: a stamp would then call a changed input unchanged.)
 * Each thread takes numbers in blocks, so a bump is a thread-local
 * increment and one shared atomic add per CC_GEN_BLOCK bumps. Numbers are
 * unique in the process, not ordered in time across threads; stamps only
 * compare them for equality. */

#define CC_GEN_BLOCK 1024

#ifdef CC_GEN_IMPL
_Atomic uint64_t cc_gen_counter = 0;
#else
extern _Atomic uint64_t cc_gen_counter;
#endif

static inline uint64_t cc_gen_next(void) {
    static _Thread_local uint64_t next, end;
    if (next == end) {
        next = atomic_fetch_add_explicit(&cc_gen_counter, CC_GEN_BLOCK, memory_order_relaxed) + 1;
        end = next + CC_GEN_BLOCK;
    }
    return next++;
}

/* ---- a source's generation ------------------------------------------ */

typedef struct { uint64_t v; } CCGen;          /* 0: never changed since init */

static inline void     cc_gen_bump(CCGen* g)      { g->v = cc_gen_next(); }
static inline uint64_t cc_gen_get(const CCGen* g) { return g->v; }

/* ---- a stamp: the inputs a derived value was last built from --------- */
/* An input is a uint64_t: a source's generation, or a scalar compared by
 * value. n == 0 means never built, so a zeroed stamp is always stale. */

#define CC_STAMP_MAX 4

typedef struct {
    uint32_t n;
    uint64_t at[CC_STAMP_MAX];
} CCStamp;

typedef struct {
    CCStamp* s;
    uint32_t n;
    uint64_t in[CC_STAMP_MAX];     /* the inputs now */
    uint64_t prev[CC_STAMP_MAX];   /* the inputs at the last build; valid when built */
    uint32_t changed;              /* bit i set: input i moved (all set when !built) */
    bool     built;                /* there was a previous build to patch from */
    bool     stale;                /* changed != 0 */
} CCStampCheck;

#define CC_INPUT(i) (1u << (i))

/* Compare the inputs with the stamp. Nothing is written yet. */
static inline CCStampCheck cc_stamp_check(CCStamp* s, const uint64_t* in, uint32_t n) {
    CCStampCheck c = { .s = s, .n = n };
    uint32_t i;
    if (n > CC_STAMP_MAX) __builtin_trap();     /* split the derivation */
    c.built = s->n == n;
    for (i = 0; i < n; i++) {
        c.in[i] = in[i];
        c.prev[i] = c.built ? s->at[i] : 0;
        if (!c.built || s->at[i] != in[i]) c.changed |= CC_INPUT(i);
    }
    c.stale = c.changed != 0;
    return c;
}

/* Record the inputs the rebuild used. Call only after the rebuild
 * succeeded; a failed rebuild leaves the stamp stale, so it retries. */
static inline void cc_stamp_commit(const CCStampCheck* c) {
    uint32_t i;
    for (i = 0; i < c->n; i++) c->s->at[i] = c->in[i];
    c->s->n = c->n;
}

/* A build is about to run from these inputs: record them in the stamp,
 * marked as building. Until cc_stamp_done a check finds the stamp stale,
 * so a build that fails part way (no done) runs again at the next read. */
#define CC_STAMP_BUILDING 0x80000000u

static inline void cc_stamp_begin(CCStamp* s, const uint64_t* in, uint32_t n) {
    uint32_t i;
    if (n > CC_STAMP_MAX) __builtin_trap();
    for (i = 0; i < n; i++) s->at[i] = in[i];
    s->n = n | CC_STAMP_BUILDING;
}

static inline void cc_stamp_done(CCStamp* s) { s->n &= ~CC_STAMP_BUILDING; }

/* Force the next check stale: explicit invalidation, and the test hook
 * for the execution check (clear, rebuild, compare). */
static inline void cc_stamp_clear(CCStamp* s) { s->n = 0; }

/* The input list, written once: CC_IN(a, b) expands to (array, count). */
#define CC_IN(...) (const uint64_t[]){ __VA_ARGS__ }, \
    (uint32_t)(sizeof((uint64_t[]){ __VA_ARGS__ }) / sizeof(uint64_t))

/* The same list as one value, for methods: cc_inputs(a, b). */
typedef struct { uint32_t n; uint64_t v[CC_STAMP_MAX]; } CCInputs;
#define cc_inputs(...) ((CCInputs){ \
    .n = (uint32_t)(sizeof((uint64_t[]){ __VA_ARGS__ }) / sizeof(uint64_t)), \
    .v = { __VA_ARGS__ } })

/* ---- a recipe's handle: states its inputs, learns whether to build ---- */
/* A recipe calls cc_derive_inputs() first. It returns false when the value
 * is fresh, and the recipe returns without building. The caller commits
 * only after a recipe that built returns successfully. */

typedef struct { CCStampCheck c; bool asked; } CCDerive;

static inline bool cc_derive_inputs(CCDerive* q, CCStamp* s, CCInputs in) {
    q->c = cc_stamp_check(s, in.v, in.n);
    q->asked = true;
    return q->c.stale;
}

/* ---- state identity ------------------------------------------------- */
/* A new id when a state is created; a move back to an earlier state
 * restores that state's id. Equal ids mean the same state. A CCGen can
 * over-report change; a CCStateId never reports "same" for different. */

typedef struct { uint64_t v; } CCStateId;

static inline CCStateId cc_state_id_new(void)                  { return (CCStateId){ cc_gen_next() }; }
static inline bool      cc_state_id_eq(CCStateId a, CCStateId b) { return a.v == b.v; }

/* ---- an edit log: a generation plus the last N typed edits ----------- */
/* push() bumps the generation and records the edit under it. A bump that
 * did not go through push() leaves a gap, and since() answers REBUILD. */

typedef enum { CC_SINCE_NONE, CC_SINCE_EDITS, CC_SINCE_REBUILD } CCSinceKind;

typedef struct { CCSinceKind kind; uint32_t first; uint32_t n; } CCSince;

#define CC_GENLOG_DECL(Name, T, N)                                            \
    typedef struct {                                                          \
        CCGen    gen;                                                         \
        uint64_t gen_at[N];                                                   \
        T        edit[N];                                                     \
        uint32_t head, count;                                                 \
    } Name;                                                                   \
    static inline void Name##_push(Name* l, T e) {                            \
        cc_gen_bump(&l->gen);                                                 \
        l->edit[l->head] = e;                                                 \
        l->gen_at[l->head] = l->gen.v;                                        \
        l->head = (l->head + 1) % (N);                                        \
        if (l->count < (N)) l->count++;                                       \
    }                                                                         \
    /* the edits after generation `seen`, oldest first */                     \
    static inline CCSince Name##_since(const Name* l, uint64_t seen) {        \
        uint32_t k, newest = (l->head + (N) - 1) % (N);                       \
        if (seen == l->gen.v) return (CCSince){ CC_SINCE_NONE, 0, 0 };        \
        if (l->count == 0 || l->gen_at[newest] != l->gen.v)                   \
            return (CCSince){ CC_SINCE_REBUILD, 0, 0 };  /* a gap */          \
        for (k = 1; k <= l->count; k++) {                                     \
            uint32_t slot = (l->head + (N) - k) % (N);                        \
            if (l->gen_at[slot] == seen)                                      \
                return (CCSince){ CC_SINCE_EDITS, (slot + 1) % (N), k - 1 };  \
        }                                                                     \
        return (CCSince){ CC_SINCE_REBUILD, 0, 0 };      /* too far behind */ \
    }                                                                         \
    static inline const T* Name##_at(const Name* l, CCSince s, uint32_t i) {  \
        return &l->edit[(s.first + i) % (N)];                                 \
    }

#endif
