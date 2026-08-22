#ifndef CC_SCRIPT_ENTRY_H
#define CC_SCRIPT_ENTRY_H

#include <stddef.h>

/* 1 if path is a script unit: first-line shebang kind shcc, else .shcc suffix. */
int cc_path_is_shcc(const char* path);

/*
 * Rewrite a script-unit source buffer:
 *   - strip shebang
 *   - force-include <ccc/script/prelude.cch>
 *   - if no top-level main: split the body so TU-scope items stay outside
 *     synthetic main, and hoist statements / runtime-init decls into
 *     `int main` (default @errhandler + token-gated a/io/in/args predecls
 *     inside that main; same predecls inside each @task body)
 *   - TU-scope: # directives, @grammar / @comptime, typedef, struct/enum/
 *     union type defs, function definitions and prototypes
 *   - discover CCDoc-`@task` TU tasks (`int name(void)` or argc/argv);
 *     synthetic main dispatches `@name` (strip that arg) or naked `@`
 *     (list tasks); else statement body
 *   - stamp #line (TU) / masked CC_LN (MAIN) so diagnostics map to .shcc
 *   - explicit main + top-level statements → #error (and stderr diag)
 *
 * Returns malloc'd rewritten text and sets *out_len.
 * Returns NULL (and leaves *out_len unchanged) on allocation failure —
 * caller keeps the original buffer. Caller decides the unit is a script.
 */
char* cc_script_rewrite_source(const char* path,
                               const char* src,
                               size_t len,
                               size_t* out_len);

/* CLI `--no-line` for the next rewrite (suppress #line / CC_LN stamps). */
void cc_script_set_no_line(int on);

#endif /* CC_SCRIPT_ENTRY_H */
