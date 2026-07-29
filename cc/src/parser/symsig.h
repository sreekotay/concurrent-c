/*
 * symsig.h — tcc-fed function signatures.
 *
 * The parser-mode tcc parse reports every named global function symbol
 * (declarations included, system headers too) through a sink before its
 * symbol table is torn down. This table holds them for the rest of the
 * pipeline: UFCS bare-name typing sees functions no textual TU/cch scan
 * can (math.h, string.h, ...), AST-phase return typing gets authoritative
 * spellings, and the strict ladder's candidate lines can say "declared"
 * with certainty. Per-parse state: reset before each parse.
 */
#ifndef CC_PARSER_SYMSIG_H
#define CC_PARSER_SYMSIG_H

#include <stddef.h>

void cc_symsig_reset(void);
/* Sink registered with cc_tcc_set_symsig_sink. */
void cc_symsig_note(const char* name, const char* ret_type,
                    const char* param0_type, int nparams, int is_variadic);
size_t cc_symsig_count(void);
/* 1 when `name` was declared in the last parse. */
int cc_symsig_known(const char* name);
/* Return-type / first-parameter spelling, normalized (extern/static/
 * inline dropped, `struct ` tags kept, pointer stars attached).
 * Returns 1 on a known function, 0 otherwise. nparams<0 = old-style. */
int cc_symsig_fn_return(const char* name, char* out, size_t out_sz);
int cc_symsig_fn_first_param(const char* name, char* out, size_t out_sz,
                             int* out_nparams, int* out_variadic);

#endif /* CC_PARSER_SYMSIG_H */
