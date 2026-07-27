#ifndef CC_SCRIPT_ENTRY_H
#define CC_SCRIPT_ENTRY_H

#include <stddef.h>

/* 1 if path ends with .shcc */
int cc_path_is_shcc(const char* path);

/*
 * Rewrite a .shcc source buffer:
 *   - strip shebang
 *   - force-include <ccc/script/prelude.cch>
 *   - if no top-level main: split the body so TU-scope items stay outside
 *     synthetic main, and hoist statements / runtime-init decls into
 *     `int main` (default @errhandler injected inside that main)
 *   - TU-scope: # directives, @grammar / @comptime, typedef, struct/enum/
 *     union type defs, function definitions and prototypes
 *   - discover `int name(int, char**)` TU tasks; synthetic main dispatches
 *     `@name` (strip that arg) or naked `@` (list tasks); else statement body
 *   - explicit main + top-level statements → #error (and stderr diag)
 *
 * Returns malloc'd rewritten text and sets *out_len.
 * Returns NULL (and leaves *out_len unchanged) when path is not .shcc
 * or on allocation failure — caller keeps the original buffer.
 */
char* cc_script_rewrite_source(const char* path,
                               const char* src,
                               size_t len,
                               size_t* out_len);

#endif /* CC_SCRIPT_ENTRY_H */
