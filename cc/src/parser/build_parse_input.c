#include "build_parse_input.h"

#include "../diag/diag.h"
#include "../preprocess/cpp_expand.h"
#include "../visitor/pass_create.h"
#include "../visitor/pass_unwrap_destroy.h"
#include "preprocess/preprocess.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared in parse.c */
extern char* cc_blank_comptime_blocks_for_prep(const char* src, size_t n);

int cc_build_parse_input(const char* file_buf,
                         size_t file_len,
                         const char* input_path,
                         CCSymbolTable* symbols,
                         int for_reparse,
                         CCBuildParseInput* out) {
    if (!file_buf || !out) return -1;
    memset(out, 0, sizeof(*out));

    out->primary_file_id = cc_diag_register_file(input_path);
    out->source_map = cc_source_map_new(out->primary_file_id);
    if (!out->source_map) return -1;

    char* buf = strdup(file_buf);
    if (!buf) goto fail;
    size_t got = file_len ? file_len : strlen(buf);

    {
        char* lowered = cc_rewrite_local_cch_includes_to_lowered_headers(buf, got, input_path);
        if (lowered) { free(buf); buf = lowered; got = strlen(buf); }
    }
    {
        char* lowered = cc_rewrite_system_cch_includes_to_lowered_headers(buf, got);
        if (lowered) { free(buf); buf = lowered; got = strlen(buf); }
    }
    {
        char* blanked = cc_blank_comptime_blocks_for_prep(buf, got);
        if (blanked) { free(buf); buf = blanked; got = strlen(buf); }
    }
    {
        char* nursery = cc_rewrite_nursery_create_destroy_proto(buf, got, input_path);
        if (nursery == (char*)-1) goto fail_buf;
        if (nursery) { free(buf); buf = nursery; got = strlen(buf); }
    }
    if (symbols) {
        char* reg = cc_rewrite_registered_type_create_destroy(buf, got, input_path, symbols);
        if (reg == (char*)-1) goto fail_buf;
        if (reg) { free(buf); buf = reg; got = strlen(buf); }
    }

    cc_unwrap_destroy_set_symbols(symbols);
    char* pp = for_reparse
        ? cc_preprocess_for_reparse(buf, got, input_path)
        : cc_preprocess_for_initial_parse(buf, got, input_path);
    cc_unwrap_destroy_set_symbols(NULL);
    free(buf);
    if (!pp) goto fail;

    /* M7 spike: opt-in pre-expand via CPP, applied after all CC text
     * passes have run. Resolves the prepended `#include` directives that
     * `cc_preprocess_for_initial_parse` adds (containers, result types) so
     * TCC's second-pass parser sees a fully-expanded translation unit.
     *
     * Known limitation: macros whose BODY contains CC syntax (e.g.
     *   #define CHAN(T) T[~4 >]
     * ) are mangled by phase-1 text passes (P3/P4) before this point and
     * cannot be rescued here. Fix requires making text passes #define-aware
     * (skip strings/comments/macro bodies) — tracked as M7.B. */
    if (!for_reparse && getenv("CC_PRE_EXPAND")) {
        size_t pp_len = strlen(pp);
        size_t exp_len = 0;
        char* expanded = cc_cpp_expand(pp, pp_len, input_path, &exp_len);
        if (expanded) {
            free(pp);
            pp = expanded;
            if (getenv("CC_DEBUG_PRE_EXPAND")) {
                fprintf(stderr, "[cc:pre-expand] %s: %zu -> %zu bytes\n",
                        input_path ? input_path : "<input>", pp_len, exp_len);
                const char* dump = getenv("CC_DEBUG_PRE_EXPAND_DUMP");
                if (dump && dump[0]) {
                    FILE* df = fopen(dump, "wb");
                    if (df) { fwrite(pp, 1, exp_len, df); fclose(df); }
                    fprintf(stderr, "[cc:pre-expand] dumped to %s\n", dump);
                }
            }
        } else if (getenv("CC_DEBUG_PRE_EXPAND")) {
            fprintf(stderr, "[cc:pre-expand] %s: cc_cpp_expand failed, falling back\n",
                    input_path ? input_path : "<input>");
        }
    }

    out->buffer = pp;
    out->len = strlen(pp);
    return 0;

fail_buf:
    free(buf);
fail:
    cc_build_parse_input_free(out);
    return -1;
}

void cc_build_parse_input_free(CCBuildParseInput* in) {
    if (!in) return;
    free(in->buffer);
    in->buffer = NULL;
    in->len = 0;
    cc_source_map_free(in->source_map);
    in->source_map = NULL;
}
