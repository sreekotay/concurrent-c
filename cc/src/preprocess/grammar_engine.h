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

/* @grammar(cli) emitter (grammar_cli.c). */
char* cc_grammar_cli_emit(const char* name,
                          const char* body, size_t body_len,
                          const char* file, int line,
                          const char* src, size_t src_len,
                          char* err, size_t err_sz);

/* Cross-block state (rules bodies that @grammar(schema) blocks `use`, schema
 * type names) is scoped to one input file; the seam resets it per rewrite. */
void cc__grammar_registry_reset(void);

/* Generated types needing instance-UFCS dispatch (NameReader, NameNode).
 * The engines note them at emission; codegen registers each with the native
 * Type_method hook (cc_ufcs_grammar_type_method_native, ufcs.c) so
 * `r.next(&out)` / `nd.first()` lower without user-written registrations. */
int cc_grammar_pending_ufcs_type_count(void);
const char* cc_grammar_pending_ufcs_type(int i);

/* Schema bind fields for UFCS path walks (`c.framed.args[i].data.slice()`).
 * Noted at grammar emit; shadow installs into the struct-field table. */
int cc_grammar_pending_ufcs_field_count(void);
const char* cc_grammar_pending_ufcs_field_type(int i);
const char* cc_grammar_pending_ufcs_field_name(int i);
const char* cc_grammar_pending_ufcs_field_fty(int i);

#endif /* CC_GRAMMAR_ENGINE_H */
