/*
 * Builtin @grammar engines (native, compiled into ccc).
 *
 * The seam (grammar_seam.c) routes `@grammar(engine) Name {body}` here first.
 * A builtin engine is a pure source-to-source transform: parse the fenced body
 * and return freshly-malloc'd generated C (a `Name` type plus `Name_*`
 * operations) to splice in place of the declaration — no comptime, no libtcc.
 * Unknown engines are not builtins; the seam falls back to the @comptime-fn
 * path for user-defined engines.
 *
 * Return protocol:
 *   non-NULL            -> generated C (caller frees); splice it.
 *   NULL, err[0]!=0     -> hard error (engine parsed the body and rejected it).
 *   NULL, err[0]==0     -> `engine` is not a builtin; use the comptime path.
 */
#ifndef CC_GRAMMAR_ENGINE_H
#define CC_GRAMMAR_ENGINE_H

#include <stddef.h>

/* src/src_len: the WHOLE input file's text. Engines use it for demand
 * analysis — a grammar declaration is a factory, and a projection (tier) is
 * only stamped out when the file actually references its entry points. */
char* cc_grammar_builtin_emit(const char* engine,
                              const char* name,
                              const char* body, size_t body_len,
                              const char* file, int line,
                              const char* src, size_t src_len,
                              char* err, size_t err_sz);

/* Cross-block state (rules bodies that @grammar(schema) blocks `use`, schema
 * type names) is scoped to one input file; the seam resets it per rewrite. */
void cc__grammar_registry_reset(void);

#endif /* CC_GRAMMAR_ENGINE_H */
