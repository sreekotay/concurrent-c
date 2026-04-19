#ifndef CC_COMPTIME_HOOK_COMPILE_H
#define CC_COMPTIME_HOOK_COMPILE_H

#include <stddef.h>

typedef enum {
    CC_COMPTIME_TYPE_HOOK_CREATE = 0,
    CC_COMPTIME_TYPE_HOOK_UFCS = 1,
} CCComptimeTypeHookKind;

/*
 * Single-hook API.  Compiles an expression (either an identifier pointing at a
 * top-level handler in the main TU, or a non-capturing lambda expression) into
 * its own host dylib and returns a resolved function pointer plus a shared
 * refcounted owner.
 *
 * The owner starts with refcount 1.  The caller typically transfers that ref
 * to the symbol table via `cc_symbols_set_type_*_callable(..., owner,
 * cc_comptime_type_hook_owner_free)`.  If registration fails, the caller drops
 * the ref by calling `cc_comptime_type_hook_owner_free(owner)` directly.
 */
int cc_comptime_compile_type_hook_callable(const char* registration_input_path,
                                           const char* logical_file,
                                           const char* original_src,
                                           size_t original_len,
                                           const char* expr_src,
                                           size_t expr_len,
                                           CCComptimeTypeHookKind kind,
                                           void** out_owner,
                                           const void** out_fn_ptr);

/*
 * Batch API.  A single TU is compiled to one dylib containing N wrappers (one
 * per spec).  Each spec is either a named handler visible in the compiled TU
 * or a lambda expression that is emitted as a unique `static` helper.
 *
 * On success, `*out_owner` points at the shared refcounted owner with refcount
 * 1 and `out_fn_ptrs[i]` is the resolved function pointer for spec i.
 *
 * Ownership protocol:
 *   - The caller transfers one reference to each symbol-table entry that it
 *     registers by calling `cc_comptime_type_hook_owner_retain(owner)` before
 *     the registration, and passing `cc_comptime_type_hook_owner_free` as the
 *     free callback.
 *   - After all registrations, the caller drops the creator ref by calling
 *     `cc_comptime_type_hook_owner_free(owner)` exactly once.
 */
typedef struct {
    CCComptimeTypeHookKind kind;

    /* Exported symbol name in the produced dylib.  Must be unique per batch.
       The corresponding `out_fn_ptrs[i]` is the resolved dlsym for this name. */
    const char* entry_name;

    /* Named-handler mode: identifier of a top-level function that exists in
       the compiled TU (top-level defs are always included for batches).
       Either this OR (lambda_src, lambda_len) must be set. */
    const char* handler_name;

    /* Lambda mode: source span of a non-capturing lambda
       "(a, b, ...) => expr_or_{block}".  Emitted as a `static` function
       named `entry_name` preceded by "__lambda__"; `handler_name` is set to
       that mangled name internally.  Ignored if `handler_name` is non-NULL. */
    const char* lambda_src;
    size_t      lambda_len;
} CCComptimeHookSpec;

int cc_comptime_compile_type_hooks(const char* registration_input_path,
                                   const char* logical_file,
                                   const char* original_src,
                                   size_t original_len,
                                   const CCComptimeHookSpec* specs,
                                   size_t n_specs,
                                   void** out_owner,
                                   const void** out_fn_ptrs);

/* Shared-owner lifecycle.  `_free` decrements refcount and, at zero, closes
   the dylib and unlinks the temporary files. */
void cc_comptime_type_hook_owner_retain(void* owner);
void cc_comptime_type_hook_owner_free(void* owner);

#endif
