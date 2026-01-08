#ifndef CC_DRIVER_H
#define CC_DRIVER_H

#include <stddef.h>

#include "comptime/symbols.h"

// Optional configuration used to preload comptime consts (e.g. from build.cc).
typedef struct {
    const CCConstBinding* consts;
    size_t const_count;
} CCCompileConfig;

// Minimal compiler driver interface.
// Returns 0 on success, non-zero on error.
int cc_compile(const char *input_path, const char *output_path);

// Variant that accepts predefined consts; future hook for build.cc.
int cc_compile_with_config(const char *input_path, const char *output_path, const CCCompileConfig* config);

#endif // CC_DRIVER_H

