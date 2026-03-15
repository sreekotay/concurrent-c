#ifndef CC_COMPTIME_HOOK_COMPILE_H
#define CC_COMPTIME_HOOK_COMPILE_H

#include <stddef.h>

typedef enum {
    CC_COMPTIME_TYPE_HOOK_CREATE = 0,
    CC_COMPTIME_TYPE_HOOK_UFCS = 1,
} CCComptimeTypeHookKind;

int cc_comptime_compile_type_hook_callable(const char* registration_input_path,
                                           const char* logical_file,
                                           const char* original_src,
                                           size_t original_len,
                                           const char* expr_src,
                                           size_t expr_len,
                                           CCComptimeTypeHookKind kind,
                                           void** out_owner,
                                           const void** out_fn_ptr);

void cc_comptime_type_hook_owner_free(void* owner);

#endif
