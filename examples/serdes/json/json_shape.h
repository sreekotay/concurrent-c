/*
 * json_shape.h — hand-lowered HIDDEN-CLASS JSON DOM (the V8/ActionScript
 * "shapes" model, applied to parsing): the golden target for a shaped
 * @grammar DOM tier.
 *
 * MODEL — keys separated from values:
 *   - An object instance stores ONLY its values, byte-packed: JShObj =
 *     { shape*, slots* } where slots is an exact-size array of 16 B tagged
 *     values. No key bytes, no key nodes, no sibling links per instance.
 *   - Keys live in SHAPES, discovered on the fly: parsing members
 *     "a","b","c" walks shape transitions root -a-> S1 -b-> S2 -c-> S3;
 *     every object with the same key sequence SHARES S3. The 100th tweet
 *     pays three hash probes for its whole key structure.
 *   - Key lookup is per-SHAPE, not per-instance: each shape lazily builds
 *     one open-addressed key->slot table, shared by every instance of that
 *     shape. get() = hash + one probe + one memcmp -> slots[idx]. O(1) vs
 *     the tape DOM's per-instance linear (key,value) walk.
 *
 * Memory per member: 16 B slot (+ amortized shape) vs the pair-tape's 48 B
 * (24 B key node + 24 B value node). Scanning primitives (SWAR strings,
 * class-table ws/num, escape decode) are REUSED from json.h so parse-core
 * quality is identical — any throughput delta is the data structure.
 *
 * Include AFTER json.h (uses its scanners and meta packing).
 */
#ifndef CC_JSON_SHAPE_H
#define CC_JSON_SHAPE_H

#include "json.h"

/* ---- tagged 16 B value (same meta packing as JsonNode) ---- */
typedef struct JShObj JShObj;
typedef struct JShArr JShArr;
typedef struct {
    uint64_t meta;                       /* tag(3) | cow(1) | len_or_count(56) */
    union { const char* bytes; JShObj* obj; JShArr* arr; } u;
} JShVal;
_Static_assert(sizeof(JShVal) == 16, "shaped value is 16 bytes");

typedef struct JShShape JShShape;
struct JShObj { JShShape* shape; JShVal* slots; };
struct JShArr { uint64_t n; JShVal* items; };

/* ---- shapes ---- */
typedef struct { uint32_t hash, klen; const char* key; uint32_t slot; } JShDesc;
struct JShShape {
    JShShape* parent;
    const char* key; uint32_t klen;      /* edge from parent (borrowed/materialized bytes) */
    uint32_t keyhash;
    uint32_t nslots;                     /* slot count at this shape (== chain depth) */
    JShShape* last_to;                   /* 1-entry monomorphic transition cache */
    uint32_t last_hash, last_klen;
    const char* last_key;
    uint32_t nkids;                      /* outgoing transitions (trie width) */
    uint8_t diverge;                     /* too wide: map-like data — objects
                                          * transitioning here fall back to
                                          * per-instance DICTIONARY mode */
    uint8_t is_dict;                     /* the dictionary sentinel shape */
    JShDesc* lookup;                     /* key->slot table (cap = pow2); built
                                          * ON FIRST FINALIZATION only — prefix
                                          * nodes of the shape trie never get
                                          * one (a.b.c.d is shared structure,
                                          * not an object anyone queries) */
    uint32_t lookup_cap;
    CCArena ar;                          /* shape arena (for the lazy build) */
    JShShape* predicted;                 /* final shape last completed from this
                                          * FIRST-key shape — allocation-site
                                          * prediction: slots preallocated
                                          * exact-size, values written direct,
                                          * zero copy on a stable wire shape */
};

/* global transition table: (parent, key) -> shape. Open addressing.
 * SHAPES ARE PERSISTENT: they live in their own arena and survive record-
 * arena resets — the schema discovered on request #1 is reused verbatim on
 * request #10000 (keys are copied into the shape arena at creation, and
 * each shape's key->slot table is built eagerly there too). */
#ifndef JSH_MAX_SHAPES
#define JSH_MAX_SHAPES 4096              /* beyond this NO parse fails — new
                                          * divergence goes to dictionary mode */
#endif
#define JSH_DICT_DEPTH 64                /* an object growing past this many
                                          * members is map-shaped, not struct-
                                          * shaped: switch it to a dictionary */
#define JSH_DICT_WIDTH 32                /* a shape with this many distinct
                                          * child keys is a map key site: mark
                                          * diverging, dict from here on */

/* dictionary-mode object: per-INSTANCE open-addressed (key -> value); the
 * V8 fallback for data that IS a map, where shapes would only explode */
typedef struct { const char* key; uint32_t klen, hash; JShVal v; } JShDEnt;
typedef struct { uint32_t n, cap; JShDEnt ents[]; } JShDict;
typedef struct {
    JsonParser jp;                       /* reuse json.h scanning state (per parse) */
    CCArena shape_arena;                 /* persistent: shapes, keys, lookups, stack */
    JShShape root;
    struct { uint64_t h; JShShape* to; } ttab[JSH_MAX_SHAPES * 2];
    uint32_t nshapes;
    JShVal* stack;                       /* member/element scratch during build */
    struct { const char* k; uint32_t klen, hash; } * kstack;   /* dict-mode keys */
    size_t sp, stack_cap;
    JShShape dict_shape;                 /* is_dict sentinel for instances */
    int alloc_fail;                      /* distinguishes OOM from dict switch */
    long dict_objs;
    char* rb; size_t rb_left;            /* record slab: one arena hit per ~8KB
                                          * (same amortization as json.h's
                                          * node batches) */
    long shape_hits, shape_misses;       /* transition-cache story */
} JShParser;

static inline uint32_t jsh__hash(const char* k, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)k[i]; h *= 16777619u; }
    return h ? h : 1u;
}

/* key equality, 8 bytes at a time (lengths already matched). Shape keys are
 * arena copies padded to 8-byte multiples at creation, and the parse-side
 * key borrows from the source buffer where at least `": ` follows — so the
 * word loads stay in bounds on both sides. */
static inline int jsh__keyeq(const char* a, const char* b, uint32_t n) {
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8); memcpy(&y, b + i, 8);
        if (x != y) return 0;
    }
    if (i < n) {
        uint64_t x = 0, y = 0;
        memcpy(&x, a + i, n - i); memcpy(&y, b + i, n - i);
        if (x != y) return 0;
    }
    return 1;
}

static inline void* jsh__ralloc(JShParser* p, size_t sz) {
    sz = (sz + 15u) & ~(size_t)15u;
    if (p->rb_left < sz) {
        size_t got = sz > 8192 ? sz : 8192;
        char* b = (char*)cc_arena_alloc_local(p->jp.arena, got, 16);
        if (!b) return NULL;
        p->rb = b; p->rb_left = got;
    }
    void* out = p->rb;
    p->rb += sz; p->rb_left -= sz;
    return out;
}

static inline JShParser* jsh_parser(CCArena shape_arena) {
    JShParser* p = (JShParser*)cc_arena_alloc_local(shape_arena, sizeof(JShParser), _Alignof(JShParser));
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->shape_arena = shape_arena;
    p->stack_cap = 1u << 16;
    p->stack = (JShVal*)cc_arena_alloc_local(shape_arena, p->stack_cap * sizeof(JShVal), _Alignof(JShVal));
    p->kstack = cc_arena_alloc_local(shape_arena, p->stack_cap * sizeof(*p->kstack), 16);
    p->dict_shape.is_dict = 1;
    return p->stack && p->kstack ? p : NULL;
}

/* per-shape key->slot table: built ONCE at shape creation (in the shape
 * arena), shared by every instance of the shape */
static JShDesc* jsh__build_lookup(JShShape* s, CCArena arena) {
    uint32_t cap = 4;
    while (cap < s->nslots * 2) cap <<= 1;
    JShDesc* t = (JShDesc*)cc_arena_alloc_local(arena, cap * sizeof(JShDesc), _Alignof(JShDesc));
    if (!t) return NULL;
    memset(t, 0, cap * sizeof(JShDesc));
    /* walk the parent chain; slot index = depth-1 at each edge */
    for (JShShape* e = s; e->parent; e = e->parent) {
        uint32_t i = e->keyhash & (cap - 1);
        while (t[i].key) i = (i + 1) & (cap - 1);
        t[i].hash = e->keyhash; t[i].klen = e->klen; t[i].key = e->key;
        t[i].slot = e->nslots - 1;
    }
    s->lookup = t; s->lookup_cap = cap;
    return t;
}

/* transition: current shape + member key -> next shape (created on first
 * sight only when `create`). At a DIVERGED shape callers pass create=0:
 * known transitions still follow the trie — struct-shaped objects stay fast
 * even when they share a site with map-shaped ones — and only NEW keys fall
 * back to dictionary mode. */
static JShShape* jsh__advance(JShParser* p, JShShape* s, const char* k, uint32_t klen, int create) {
    /* monomorphic fast path FIRST, no hashing: one length check + one memcmp.
     * With a stable wire shape (the overwhelmingly common case) every member
     * of every object after the first takes this path. */
    if (s->last_to && s->last_klen == klen && memcmp(s->last_key, k, klen) == 0) {
        p->shape_hits++;
        return s->last_to;
    }
    uint32_t kh = jsh__hash(k, klen);
    uint64_t th = ((uint64_t)(uintptr_t)s * 0x9E3779B97F4A7C15ULL) ^ kh;
    uint32_t cap = JSH_MAX_SHAPES * 2, i = (uint32_t)(th >> 32) & (cap - 1);
    for (;;) {
        if (!p->ttab[i].to) break;
        if (p->ttab[i].h == th) {
            JShShape* to = p->ttab[i].to;
            if (to->parent == s && to->klen == klen && memcmp(to->key, k, klen) == 0) {
                s->last_to = to; s->last_hash = kh; s->last_klen = klen; s->last_key = to->key;
                p->shape_hits++;
                return to;
            }
        }
        i = (i + 1) & (cap - 1);
    }
    if (!create) return NULL;                        /* diverged site, new key */
    if (p->nshapes >= JSH_MAX_SHAPES) return NULL;   /* dict, not failure */
    p->shape_misses++;
    JShShape* to = (JShShape*)cc_arena_alloc_local(p->shape_arena, sizeof(JShShape), _Alignof(JShShape));
    if (!to) { p->alloc_fail = 1; return NULL; }
    memset(to, 0, sizeof *to);
    /* key bytes are copied ONCE per shape — instances never carry keys and
     * the shape outlives any single parse buffer */
    {
        /* pad the copy to an 8-byte multiple so jsh__keyeq's word loads on
         * the shape side never leave the allocation */
        size_t kcap = ((size_t)klen + 8u) & ~7u;
        char* kc = (char*)cc_arena_alloc_local(p->shape_arena, kcap, 8);
        if (!kc) { p->alloc_fail = 1; return NULL; }
        memset(kc + klen, 0, kcap - klen);
        memcpy(kc, k, klen);
        to->key = kc;
    }
    to->parent = s; to->klen = klen; to->keyhash = kh;
    to->nslots = s->nslots + 1;
    to->ar = p->shape_arena;
    if (++s->nkids > JSH_DICT_WIDTH) s->diverge = 1;   /* map key site */
    p->ttab[i].h = th; p->ttab[i].to = to;
    p->nshapes++;
    s->last_to = to; s->last_hash = kh; s->last_klen = klen; s->last_key = to->key;
    return to;
}

static JsonStatus jsh__value(JShParser* p, JShVal* out);

static JsonStatus jsh__object(JShParser* p, JShVal* out) {
    JsonParser* jp = &p->jp;
    jp->pos++; json__skip_ws(jp);
    JShShape* s = &p->root;
    JShShape* s1 = NULL;                 /* shape after the FIRST key (prediction site) */
    size_t base = p->sp;
    JShVal* direct = NULL;               /* predicted exact-size slot lane */
    uint32_t dcap = 0, dn = 0;
    int first = 1, dict = 0;
    if (jp->pos < jp->len && jp->src[jp->pos] == '}') { jp->pos++; goto done; }
    for (;;) {
        json__skip_ws(jp);
        if (jp->pos >= jp->len || jp->src[jp->pos] != '"') return JSON_BAD;
        const char* kb; uint64_t kl; int kcow;
        if (json__string(jp, &kb, &kl, &kcow) != JSON_OK) return JSON_BAD;
        if (!dict) {
            JShShape* nx = s->nslots >= JSH_DICT_DEPTH
                               ? NULL : jsh__advance(p, s, kb, (uint32_t)kl, !s->diverge);
            if (!nx) {
                if (p->alloc_fail) return JSON_BAD;
                /* SWITCH TO DICTIONARY: this object is map-shaped. Values so
                 * far sit in the stack (or direct lane); their keys are
                 * recoverable from the shape chain — copy both down, then
                 * carry keys per member from here on. */
                uint32_t m = s->nslots;
                if (base + m >= p->stack_cap) return JSON_BAD;
                if (direct) {
                    memcpy(p->stack + base, direct, dn * sizeof(JShVal));
                    p->sp = base + dn;
                    direct = NULL;
                }
                {
                    uint32_t idx = m;
                    for (JShShape* e = s; e->parent; e = e->parent) {
                        idx--;
                        p->kstack[base + idx].k = e->key;
                        p->kstack[base + idx].klen = e->klen;
                        p->kstack[base + idx].hash = e->keyhash;
                    }
                }
                dict = 1;
                p->dict_objs++;
            } else {
                s = nx;
            }
        }
        if (dict) {
            if (p->sp == p->stack_cap) return JSON_BAD;
            p->kstack[p->sp].k = kb;
            p->kstack[p->sp].klen = (uint32_t)kl;
            p->kstack[p->sp].hash = jsh__hash(kb, kl);
        }
        if (first) {
            first = 0;
            if (!dict) {
                s1 = s;
                if (s1->predicted) {     /* seen this opener before: write direct */
                    dcap = s1->predicted->nslots;
                    direct = (JShVal*)jsh__ralloc(p, dcap * sizeof(JShVal));
                    if (!direct) return JSON_BAD;
                }
            }
        }
        json__skip_ws(jp);
        if (jp->pos >= jp->len || jp->src[jp->pos++] != ':') return JSON_BAD;
        json__skip_ws(jp);
        JShVal* tgt;
        if (direct) {
            if (dn == dcap) {            /* prediction too small: spill to stack */
                if (base + dn >= p->stack_cap) return JSON_BAD;
                memcpy(p->stack + base, direct, dn * sizeof(JShVal));
                p->sp = base + dn;
                direct = NULL;
                tgt = &p->stack[p->sp++];
            } else {
                tgt = &direct[dn++];
            }
        } else {
            if (p->sp == p->stack_cap) return JSON_BAD;
            tgt = &p->stack[p->sp++];
        }
        if (jsh__value(p, tgt) != JSON_OK) return JSON_BAD;
        json__skip_ws(jp);
        if (jp->pos >= jp->len) return JSON_BAD;
        char d = jp->src[jp->pos++];
        if (d == '}') break;
        if (d != ',') return JSON_BAD;
    }
done: ;
    if (dict) {
        uint32_t n = (uint32_t)(p->sp - base), cap = 4;
        while (cap < n * 2) cap <<= 1;
        JShDict* dd = (JShDict*)jsh__ralloc(p, sizeof(JShDict) + cap * sizeof(JShDEnt));
        if (!dd) return JSON_BAD;
        memset(dd->ents, 0, cap * sizeof(JShDEnt));
        dd->cap = cap; dd->n = 0;
        for (uint32_t i = 0; i < n; i++) {   /* insert; duplicate key = last wins */
            uint32_t h = p->kstack[base + i].hash, j = h & (cap - 1);
            for (;;) {
                if (!dd->ents[j].key) {
                    dd->ents[j].key = p->kstack[base + i].k;
                    dd->ents[j].klen = p->kstack[base + i].klen;
                    dd->ents[j].hash = h;
                    dd->ents[j].v = p->stack[base + i];
                    dd->n++;
                    break;
                }
                if (dd->ents[j].hash == h && dd->ents[j].klen == p->kstack[base + i].klen &&
                    memcmp(dd->ents[j].key, p->kstack[base + i].k, dd->ents[j].klen) == 0) {
                    dd->ents[j].v = p->stack[base + i];
                    break;
                }
                j = (j + 1) & (cap - 1);
            }
        }
        JShObj* o = (JShObj*)jsh__ralloc(p, sizeof(JShObj));
        if (!o) return JSON_BAD;
        o->shape = &p->dict_shape;
        o->slots = (JShVal*)dd;
        p->sp = base;
        out->meta = JSON_META(JSON_OBJ, 0, dd->n);
        out->u.obj = o;
        return JSON_OK;
    }
    JShObj* o = (JShObj*)jsh__ralloc(p, sizeof(JShObj));
    if (!o) return JSON_BAD;
    uint32_t n = direct ? dn : (uint32_t)(p->sp - base);
    o->shape = s;
    if (direct) {
        o->slots = n ? direct : NULL;    /* zero-copy; short fill wastes tail bump */
    } else {
        o->slots = NULL;
        if (n) {
            o->slots = (JShVal*)jsh__ralloc(p, n * sizeof(JShVal));
            if (!o->slots) return JSON_BAD;
            memcpy(o->slots, p->stack + base, n * sizeof(JShVal));
        }
    }
    if (s->nslots && !s->lookup && !jsh__build_lookup(s, s->ar)) return JSON_BAD;
    if (s1) s1->predicted = s;           /* train the site (monomorphic update) */
    p->sp = base;
    out->meta = JSON_META(JSON_OBJ, 0, n);
    out->u.obj = o;
    return JSON_OK;
}

static JsonStatus jsh__array(JShParser* p, JShVal* out) {
    JsonParser* jp = &p->jp;
    jp->pos++; json__skip_ws(jp);
    size_t base = p->sp;
    if (jp->pos < jp->len && jp->src[jp->pos] == ']') { jp->pos++; goto done; }
    for (;;) {
        if (p->sp == p->stack_cap) return JSON_BAD;
        if (jsh__value(p, &p->stack[p->sp]) != JSON_OK) return JSON_BAD;
        p->sp++;
        json__skip_ws(jp);
        if (jp->pos >= jp->len) return JSON_BAD;
        char d = jp->src[jp->pos++];
        if (d == ']') break;
        if (d != ',') return JSON_BAD;
        json__skip_ws(jp);
    }
done: ;
    JShArr* a = (JShArr*)jsh__ralloc(p, sizeof(JShArr));
    if (!a) return JSON_BAD;
    uint64_t n = (uint64_t)(p->sp - base);
    a->n = n;
    a->items = NULL;
    if (n) {
        a->items = (JShVal*)jsh__ralloc(p, n * sizeof(JShVal));
        if (!a->items) return JSON_BAD;
        memcpy(a->items, p->stack + base, n * sizeof(JShVal));
    }
    p->sp = base;
    out->meta = JSON_META(JSON_ARR, 0, n);
    out->u.arr = a;
    return JSON_OK;
}

static JsonStatus jsh__value(JShParser* p, JShVal* out) {
    JsonParser* jp = &p->jp;
    if (jp->pos >= jp->len) return JSON_BAD;
    char c = jp->src[jp->pos];
    switch (c) {
    case '"': { const char* b; uint64_t l; int cow;
        if (json__string(jp, &b, &l, &cow) != JSON_OK) return JSON_BAD;
        out->meta = JSON_META(JSON_STR, cow, l); out->u.bytes = b; return JSON_OK; }
    case '{': return jsh__object(p, out);
    case '[': return jsh__array(p, out);
    case 't': {
        if (jp->pos + 4 <= jp->len) { uint32_t w; memcpy(&w, jp->src + jp->pos, 4);
            if (w == 0x65757274u) { out->meta = JSON_META(JSON_BOOL, 0, 1); jp->pos += 4; return JSON_OK; } }
        return JSON_BAD; }
    case 'f': {
        if (jp->pos + 5 <= jp->len) { uint32_t w; memcpy(&w, jp->src + jp->pos, 4);
            if (w == 0x736C6166u && jp->src[jp->pos + 4] == 'e') { out->meta = JSON_META(JSON_BOOL, 0, 0); jp->pos += 5; return JSON_OK; } }
        return JSON_BAD; }
    case 'n': {
        if (jp->pos + 4 <= jp->len) { uint32_t w; memcpy(&w, jp->src + jp->pos, 4);
            if (w == 0x6C6C756Eu) { out->meta = JSON_META(JSON_NULL, 0, 0); jp->pos += 4; return JSON_OK; } }
        return JSON_BAD; }
    default: {
        size_t s = jp->pos, e = json__scan_num(jp->src, s, jp->len);
        if (e == s) return JSON_BAD;
        out->meta = JSON_META(JSON_NUM, 0, e - s); out->u.bytes = jp->src + s; jp->pos = e; return JSON_OK; }
    }
}

/* records (objects, arrays, slots, materialized strings) go to rec_arena —
 * resettable per parse; shapes persist in the parser's shape arena */
static inline JsonStatus JShParser_parse(JShParser* p, const char* src, size_t len,
                                         CCArena rec_arena, JShVal* out) {
    p->jp = json_parser(src, len, rec_arena);
    p->sp = 0;
    p->alloc_fail = 0;
    p->rb = NULL; p->rb_left = 0;        /* invalidate across arena reset */
    json__skip_ws(&p->jp);
    return jsh__value(p, out);
}

/* prepared key: hash once, look up many times (atoms/interned keys — and the
 * exact runtime analog of what a comptime schema get computes for free) */
typedef struct { const char* key; uint32_t klen, hash; } JShKey;
static inline JShKey jsh_key(const char* key) {
    JShKey k; k.key = key; k.klen = (uint32_t)strlen(key); k.hash = jsh__hash(key, k.klen);
    return k;
}

static inline JShVal* JShObj_get_k(JShObj* o, JShKey k) {
    JShShape* s = o->shape;
    if (s->is_dict) {
        JShDict* d = (JShDict*)o->slots;
        uint32_t cap = d->cap;
        for (uint32_t i = k.hash & (cap - 1); d->ents[i].key; i = (i + 1) & (cap - 1))
            if (d->ents[i].hash == k.hash && d->ents[i].klen == k.klen &&
                memcmp(d->ents[i].key, k.key, k.klen) == 0)
                return &d->ents[i].v;
        return NULL;
    }
    if (s->nslots == 0) return NULL;
    JShDesc* t = s->lookup;
    uint32_t cap = s->lookup_cap;
    for (uint32_t i = k.hash & (cap - 1); t[i].key; i = (i + 1) & (cap - 1))
        if (t[i].hash == k.hash && t[i].klen == k.klen && memcmp(t[i].key, k.key, k.klen) == 0)
            return &o->slots[t[i].slot];
    return NULL;
}

static inline JShVal* JShObj_get(JShObj* o, const char* key, size_t klen) {
    JShKey k; k.key = key; k.klen = (uint32_t)klen; k.hash = jsh__hash(key, klen);
    return JShObj_get_k(o, k);
}

static inline JShVal* JShVal_get(JShVal* v, const char* key) {
    if (JSON_TAG(v->meta) != JSON_OBJ) return NULL;
    return JShObj_get(v->u.obj, key, strlen(key));
}

static inline CCSlice JShVal_slice(const JShVal* v) {
    uint64_t l = JSON_LEN(v->meta);
    uint64_t id = JSON_COW(v->meta) ? cc_slice_make_id(JSON_ARENA_ID, true, false, false)
                                    : cc_slice_make_id(JSON_SRC_ID, false, false, true);
    return cc_slice_from_parts((void*)v->u.bytes, l, id);
}

#endif /* CC_JSON_SHAPE_H */
