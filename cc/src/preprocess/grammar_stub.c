/*
 * STAGE-1 BOOTSTRAP STUB for the @grammar engines.
 *
 * lower_headers links the full preprocess pipeline, and the pipeline links
 * the grammar engines (grammar_rules.c) — but the engines' emitter text is
 * itself CC (src/preprocess/emit/ star-dot-cch) that lower_headers must lower first.
 * This stub breaks that cycle: a stage-1 lower_headers links THIS file in
 * place of grammar_rules.c, lowers the engine sources, and then the real
 * compiler builds against the generated header.
 *
 * The stub is sound because engine .cch sources are plain CC (@string
 * templates, CCString, arenas) and contain no @grammar declarations.  It is
 * also self-enforcing: if an engine header ever grows a @grammar block,
 * stage-1 lowering fails LOUDLY here instead of mislowering — the hard
 * error below, not the "not a builtin, fall back to comptime" protocol,
 * because during bootstrap the builtins simply do not exist yet.
 *
 * Keep in lockstep with grammar_engine.h (the linker enforces the names;
 * -Wmissing-prototypes + the shared header enforce the signatures).
 */
#include "preprocess/grammar_engine.h"

#include <stdio.h>

char* cc_grammar_builtin_emit(const char* engine,
                              const char* name,
                              const char* body, size_t body_len,
                              const char* file, int line,
                              const char* src, size_t src_len,
                              char* err, size_t err_sz) {
    (void)body; (void)body_len; (void)src; (void)src_len;
    if (err && err_sz)
        snprintf(err, err_sz,
                 "@grammar(%s) %s (%s:%d): @grammar is unavailable during the "
                 "stage-1 bootstrap — engine sources under src/preprocess/emit/ "
                 "must not declare grammars",
                 engine ? engine : "?", name ? name : "?",
                 file ? file : "?", line);
    return NULL;
}

void cc__grammar_registry_reset(void) {}

void cc__grammar_note_ufcs_type(const char* type_name) { (void)type_name; }

int cc_grammar_pending_ufcs_type_count(void) { return 0; }

const char* cc_grammar_pending_ufcs_type(int i) { (void)i; return ""; }

int cc_grammar_pending_ufcs_field_count(void) { return 0; }

const char* cc_grammar_pending_ufcs_field_type(int i) {
    (void)i;
    return "";
}

const char* cc_grammar_pending_ufcs_field_name(int i) {
    (void)i;
    return "";
}

const char* cc_grammar_pending_ufcs_field_fty(int i) {
    (void)i;
    return "";
}
