/*
 * cc_source.h: sources and derived values as generic types. No language
 * support: every write is a visible method call, every byte is in the
 * type, and the only writable access to a derived value comes from a
 * stale check. Built on cc_gen.h.
 *
 *   CC_SOURCE_DECL(Name, T)    value + generation
 *     x.set(v)     store and bump
 *     x.mut()      a T* for mutation through verbs; bumps now
 *     x.val()      a copy          x.ref()   a const T*      x.gen()  the generation
 *
 *   CC_DERIVED_DECL(Name, T)   value + stamp + generation
 *     x.check(CC_IN(...))  compare inputs; when stale, .slot is the writable value
 *     x.commit(&k)         record the inputs, bump the generation
 *     x.ref()  x.gen()
 */
#ifndef CC_SOURCE_H
#define CC_SOURCE_H
#include "cc_gen.h"

#define CC_SOURCE_DECL(Name, T)                                                   \
    typedef struct { T value; CCGen g; } Name;                                    \
    static inline void     Name##_set(Name* s, T v)  { s->value = v; cc_gen_bump(&s->g); } \
    static inline T*       Name##_mut(Name* s)       { cc_gen_bump(&s->g); return &s->value; } \
    static inline T        Name##_val(const Name* s) { return s->value; }       \
    static inline const T* Name##_ref(const Name* s) { return &s->value; }      \
    static inline uint64_t Name##_gen(const Name* s) { return cc_gen_get(&s->g); }

#define CC_DERIVED_DECL(Name, T)                                                  \
    typedef struct { T value; CCStamp at; CCGen g; } Name;                        \
    typedef struct { CCStampCheck c; T* slot; bool stale; } Name##_Check;         \
    static inline Name##_Check Name##_check(Name* d, const uint64_t* in, uint32_t n) { \
        Name##_Check k = { .c = cc_stamp_check(&d->at, in, n) };                  \
        k.stale = k.c.stale;                                                      \
        k.slot = k.stale ? &d->value : 0;   /* writable only when stale */       \
        return k;                                                                 \
    }                                                                             \
    static inline void Name##_commit(Name* d, const Name##_Check* k) {            \
        cc_stamp_commit(&k->c); cc_gen_bump(&d->g);                               \
    }                                                                             \
    static inline const T* Name##_ref(const Name* d) { return &d->value; }        \
    static inline uint64_t Name##_gen(const Name* d) { return cc_gen_get(&d->g); } \
    static inline void     Name##_invalidate(Name* d) { cc_stamp_clear(&d->at); }

#endif
