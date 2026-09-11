#ifndef CC_COMPTIME_PREPARE_H
#define CC_COMPTIME_PREPARE_H

#include <stddef.h>

/* Which of the prepare passes to run. The clean lowerer resolves
 * type-scoped calls and string templates itself, from its own index and
 * AST: running them here too would hand it C where it expects the
 * language, and its own steps would then have nothing to lower. */
#define CC_PREPARE_GRAMMAR      1u
#define CC_PREPARE_MODULE_EXPORT 2u
#define CC_PREPARE_TYPE_SCOPED   4u
#define CC_PREPARE_FACTORY_SUGAR 8u
#define CC_PREPARE_STATIC_MAP    16u
#define CC_PREPARE_COMPTIME_IF   32u
#define CC_PREPARE_TEMPLATES     64u
/* Value-position `@comptime(expr)`. Separate from the branch pass because
 * the clean lowerer resolves it itself, from the AST: it calls the same
 * executor, naming the expression by the span it parsed. */
#define CC_PREPARE_COMPTIME_VALUE 128u
#define CC_PREPARE_COMPTIME      (CC_PREPARE_COMPTIME_IF | CC_PREPARE_COMPTIME_VALUE)
#define CC_PREPARE_ALL           (CC_PREPARE_GRAMMAR | CC_PREPARE_MODULE_EXPORT | \
                                  CC_PREPARE_TYPE_SCOPED | CC_PREPARE_FACTORY_SUGAR | \
                                  CC_PREPARE_STATIC_MAP | CC_PREPARE_COMPTIME | \
                                  CC_PREPARE_TEMPLATES)

int cc_comptime_prepare_source_ex(char** inout_buf, size_t* inout_len,
                                  const char* input_path, unsigned passes);

/* Space-blank every `@comptime` construct, keeping the layout: the source
 * a parser may read once the blocks have been run. A file-scope block
 * leaves an `enum{__ccs<n>=0};` marker on one of its blanked lines, and a
 * block inside an `enum { }` leaves a dummy enumerator, so an
 * `CC_EMIT_AT_COMPTIME_SITE` fragment has somewhere to splice. Newlines
 * are never overwritten, so the line map holds. Returns NULL on an
 * unterminated construct, having said which. Caller frees. */
char* cc_comptime_blank_blocks(const char* src, size_t n);

/* Leave value-position `@comptime(expr)` in place: the caller resolves it
 * itself, from the expression it parsed. */
#define CC_BLANK_KEEP_VALUE 1u
/* Leave `@comptime` function and constant definitions in place: the
 * caller registers them from its own parse, so what a value-position
 * `@comptime(expr)` calls is there to be read, and drops them itself. */
#define CC_BLANK_KEEP_FN 2u
/* Leave a file-scope `@comptime { }` block in place when it registers type
 * hooks (`cc_type_register` / `cc_type_define` / `cc_ufcs_register`) and
 * emits nothing: the caller reads the registrations from its own parse of
 * the block, and drops the block itself. A block that also emits is
 * blanked as usual, so its site marker stays where the emits splice. */
#define CC_BLANK_KEEP_HOOKS 4u
char* cc_comptime_blank_blocks_ex(const char* src, size_t n, unsigned keep);

/* Resolve `@comptime if/for`, then lower `@emit` / `@string` templates.
 * Updates *inout_buf / *inout_len in place (frees the prior buffer on change).
 * Returns 0 on success, -1 on hard error ((char*)-1 from a sub-pass). */
int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path);

/* 1 = route @comptime if/for predicate + field load through libtcc (default).
 * Set CC_COMPTIME_UNIFIED_EXEC=0 to use the legacy structural resolver only. */
int cc_comptime_unified_exec_enabled(void);

#endif /* CC_COMPTIME_PREPARE_H */
