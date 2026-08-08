#include "comptime_prepare.h"

#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "preprocess/template_scan.h"

#include <stdlib.h>
#include <string.h>

int cc_comptime_unified_exec_enabled(void) {
    const char* flag = getenv("CC_COMPTIME_UNIFIED_EXEC");
    if (!flag || !flag[0]) return 1;
    if (flag[0] == '0' && flag[1] == '\0') return 0;
    if (flag[0] == '1' && flag[1] == '\0') return 1;
    return 1;
}

int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path) {
    char* resolved;
    char* templ;
    char* factory;
    if (!inout_buf || !*inout_buf || !inout_len) return -1;

    /* Closer-anchored template dedent, before any pass reads a template
     * body (grammar fences carry no backticks, so running first is safe;
     * idempotent — a dedented closer sits at column 0). */
    {
        size_t dlen = 0;
        char* ded = cc_tpl_dedent_text(*inout_buf, *inout_len, input_path, &dlen);
        if (ded == (char*)-1) return -1;
        if (ded) {
            free(*inout_buf);
            *inout_buf = ded;
            *inout_len = dlen;
        }
    }

    /* @grammar(engine) Name {SENT...SENT}: capture the fenced body VERBATIM and
     * rewrite to a synthesized @comptime engine call.  Must run before every
     * other rewrite — the body is raw non-C bytes that later passes (templates,
     * factories) must never see. */
    {
        char* grammar = cc_rewrite_grammar_decls_text(*inout_buf, *inout_len, input_path);
        if (grammar == (char*)-1) return -1;
        if (grammar) {
            free(*inout_buf);
            *inout_buf = grammar;
            *inout_len = strlen(grammar);
        }
    }

    /* `@comptime <directive>(...);` module-export sugar: expand the
     * header-declared CC_MODULE_EXPORT template into the same entry
     * stanza a hand-written registration spells, before any other pass
     * reads the TU — the splice is indistinguishable from source. */
    {
        char* mex = cc_rewrite_module_export_directives_text(*inout_buf, *inout_len, input_path);
        if (mex == (char*)-1) return -1;
        if (mex) {
            free(*inout_buf);
            *inout_buf = mex;
            *inout_len = strlen(mex);
        }
    }

    /* Type-scoped calls (`Tweet.parse(...)` -> `Tweet_parse(...)`) rewrite
     * AFTER the grammar splice so lowered names emitted by the engines count
     * as visible; must happen before the C parse (syntax error otherwise). */
    {
        char* tsc = cc_rewrite_type_scoped_calls_text(*inout_buf, *inout_len);
        if (tsc) {
            free(*inout_buf);
            *inout_buf = tsc;
            *inout_len = strlen(tsc);
        }
    }

    /* Expand CC_GENERIC_FACTORY(Name){...} sugar before anything else so the
     * downstream @comptime if/for + @emit lowering and the comptime collector
     * see canonical @comptime constructs. */
    factory = cc_rewrite_generic_factory_text(*inout_buf, *inout_len, input_path);
    if (factory == (char*)-1) return -1;
    if (factory) {
        free(*inout_buf);
        *inout_buf = factory;
        *inout_len = strlen(factory);
    }

    /* Typed static_map(name, entries, flags) → layout-carrying internal call
     * before @comptime if/for so the executor sees the expanded arity. */
    {
        char* sm = cc_rewrite_static_map_calls_text(*inout_buf, *inout_len, input_path);
        if (sm == (char*)-1) return -1;
        if (sm) {
            free(*inout_buf);
            *inout_buf = sm;
            *inout_len = strlen(sm);
        }
    }

    resolved = cc__resolve_comptime_if(*inout_buf, *inout_len, input_path);
    if (resolved == (char*)-1) return -1;
    if (resolved) {
        free(*inout_buf);
        *inout_buf = resolved;
        *inout_len = strlen(resolved);
    }

    /* Value-position `@comptime(expr)`: evaluate and splice the projected C
     * literal in place.  Runs after `@comptime if/for` pruning (so only live
     * sites are evaluated) and before template lowering.  The splice lands in
     * the buffer that feeds both the parse buffer and `buffer_codegen`, so the
     * hoisted literal is visible in the lowered C — no anchor plumbing needed. */
    {
        char* valued = cc__resolve_comptime_value(*inout_buf, *inout_len, input_path);
        if (valued == (char*)-1) return -1;
        if (valued) {
            free(*inout_buf);
            *inout_buf = valued;
            *inout_len = strlen(valued);
        }
    }

    templ = cc_normalize_template_recv_chains_text(*inout_buf, *inout_len);
    if (templ == (char*)-1) return -1;
    if (templ) {
        free(*inout_buf);
        *inout_buf = templ;
        *inout_len = strlen(templ);
    }

    templ = cc_rewrite_string_templates_text(*inout_buf, *inout_len, input_path);
    if (templ == (char*)-1) return -1;
    if (templ) {
        free(*inout_buf);
        *inout_buf = templ;
        *inout_len = strlen(templ);
    }
    return 0;
}
