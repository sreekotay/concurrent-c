/*
 * Frozen hostile host contract — language-neutral C ABI sketch.
 *
 * Implementations (CC, Rust/FFI, later C) must preserve these semantics.
 * This header does not include QuickJS or Concurrent-C types.
 */
#ifndef STUDIES_QJS_HOSTILE_CONTRACT_H
#define STUDIES_QJS_HOSTILE_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HOSTILE_OK = 0,
    HOSTILE_ERR_EXPIRED = 1,       /* weak upgrade / invoke after unregister */
    HOSTILE_ERR_DOUBLE_RELEASE = 2,
    HOSTILE_ERR_STALE_REALM = 3,   /* claim/reg after context destroyed */
    HOSTILE_ERR_STALE_BORROW = 4,  /* borrow used after scope end */
    HOSTILE_ERR_NOT_LIVE = 5,      /* claim already released */
    HOSTILE_ERR_BAD_VALUE = 6,     /* null / wrong type for op */
    HOSTILE_ERR_ARENA = 7
};

/* Opaque host-side handles (implementation-defined layout behind API). */
typedef struct HostileClaim HostileClaim;
typedef struct HostileWeak HostileWeak;
typedef struct HostileRegistration HostileRegistration;
typedef struct HostileBorrow HostileBorrow;
typedef struct HostileRealm HostileRealm;

/* Realm = one JSRuntime+JSContext (or equivalent). */
HostileRealm *hostile_realm_new(void);
void hostile_realm_destroy(HostileRealm *realm);

/*
 * Eval source in the realm; returns a host-owned temporary value that
 * must be retained or released via the claim API. Implementations may
 * expose this as a transient handle — the study drivers use retain
 * immediately on payloads.
 */
typedef struct HostileValue HostileValue;

HostileValue *hostile_eval(HostileRealm *realm, const char *src, size_t len,
                           int *err);
void hostile_value_drop(HostileValue *v); /* no claim; drops one ref */

HostileClaim *hostile_retain(HostileRealm *realm, HostileValue *v, int *err);
int hostile_release(HostileClaim *claim);

/* Borrow is scoped: begin → use → end. Nested begins allowed. */
HostileBorrow *hostile_borrow_begin(HostileClaim *claim, int *err);
HostileValue *hostile_borrow_value(HostileBorrow *b); /* temporary view */
int hostile_borrow_end(HostileBorrow *b);

HostileWeak *hostile_weak(HostileRealm *realm, HostileValue *v, int *err);
void hostile_weak_drop(HostileWeak *w);
HostileClaim *hostile_upgrade(HostileWeak *w, int *err);

HostileRegistration *hostile_register_callback(HostileRealm *realm,
                                               HostileValue *fn, int *err);
int hostile_unregister(HostileRegistration *reg);
/* Invoke retained callback with no args; returns HOSTILE_OK or error. */
int hostile_invoke_registered(HostileRegistration *reg);

/* Host-side counters for checkpoints (optional; 0 if unimplemented). */
size_t hostile_outstanding_claims(HostileRealm *realm);
size_t hostile_outstanding_weaks(HostileRealm *realm);
size_t hostile_outstanding_regs(HostileRealm *realm);

#ifdef __cplusplus
}
#endif

#endif /* STUDIES_QJS_HOSTILE_CONTRACT_H */
