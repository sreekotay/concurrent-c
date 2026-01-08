#ifndef CC_BUILD_BUILD_H
#define CC_BUILD_BUILD_H

#include <stddef.h>

#include "comptime/symbols.h"

typedef struct {
    const char* os;
    const char* arch;
    const char* abi;
    const char* endian;
    int ptr_width;
} CCBuildTarget;

typedef struct {
    const CCBuildTarget* target;
    // Placeholder for future whitelisted env; kept NULL/pure for now.
    const char* const* envp;
} CCBuildInputs;

// Stub loader: if build.cc exists, return a fixed const set; else empty.
// Returns 0 on success, non-zero on error. The returned bindings are owned
// by the caller (no allocations are performed here).
int cc_build_load_consts(const char* build_path, const CCBuildInputs* inputs, CCConstBinding* out_bindings, size_t* out_count);

#endif // CC_BUILD_BUILD_H

