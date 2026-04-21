#ifndef CC_PASS_UNWRAP_DESTROY_H
#define CC_PASS_UNWRAP_DESTROY_H

#include <stddef.h>

#include "comptime/symbols.h"

/* Install the ambient symbol table the unwrap-destroy pass consults when
 * resolving a bodyless `@destroy;` on a user-declared type (via
 * `@comptime cc_type_register(...)`).  Pass NULL to clear.  Callers that
 * have a live CCSymbolTable* should set it before invoking the pass so the
 * bodyless form works for registered types; otherwise the pass falls back
 * to the hardcoded builtin-owned list (CCNursery*, CCArena, CCChan*). */
void cc_unwrap_destroy_set_symbols(CCSymbolTable* symbols);

/* Rewrite a trailing `@destroy { body }` suffix on a statement that
 * contains a `!>` or `?>` operator into a standalone `@defer { body };`
 * placed immediately after the original statement terminator.
 *
 *   T* p = get_ptr() !> { abort(); } @destroy { free(p); };
 *     => T* p = get_ptr() !> { abort(); }                 ;
 *        @defer { free(p); };
 *
 *   char* s = open() ?> "fallback" @destroy { log(s); };
 *     => char* s = open() ?> "fallback"                   ;
 *        @defer { log(s); };
 *
 *   f() !> @destroy { cleanup(); };
 *     => f() !>                                           ;
 *        @defer { cleanup(); };
 *
 * The rewrite runs BEFORE pass_result_unwrap so the downstream `!>`/`?>`
 * lowering sees a standard form.  `@create(...) @destroy { ... };` is
 * left alone (pass_create owns that form); the disambiguator is the
 * presence of `!>` or `?>` in the current statement.
 *
 * Returns 1 if any rewrites occurred (out_buf populated), 0 if no work
 * was needed (out_buf left NULL), -1 on syntax error.
 */
int cc__rewrite_unwrap_destroy_suffix(const char* src,
                                      size_t n,
                                      const char* input_path,
                                      char** out_buf,
                                      size_t* out_len);

#endif
