#ifndef CC_SCRIPT_ENTRY_H
#define CC_SCRIPT_ENTRY_H

#include <stddef.h>

/* 1 if path ends with .ccscript */
int cc_path_is_ccscript(const char* path);

/*
 * Rewrite a .ccscript source buffer:
 *   - strip shebang
 *   - force-include <ccc/script/prelude.cch>
 *   - if no top-level main: split the body so TU-scope items stay outside
 *     synthetic main, and hoist statements / runtime-init decls into
 *     `int main` (default @errhandler injected inside that main)
 *   - TU-scope: # directives, @grammar / @comptime, typedef, struct/enum/
 *     union type defs, function definitions and prototypes
 *   - explicit main + top-level statements → #error (and stderr diag)
 *
 * Returns malloc'd rewritten text and sets *out_len.
 * Returns NULL (and leaves *out_len unchanged) when path is not .ccscript
 * or on allocation failure — caller keeps the original buffer.
 */
char* cc_script_rewrite_source(const char* path,
                               const char* src,
                               size_t len,
                               size_t* out_len);

#endif /* CC_SCRIPT_ENTRY_H */
