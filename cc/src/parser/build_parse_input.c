#include "build_parse_input.h"
#include "util/text.h"

#include "../diag/diag.h"
#include "../preprocess/cc_closure_markers.h"
#include "../preprocess/cc_l2_rewriter.h"
#include "../preprocess/cpp_expand.h"
#include "../preprocess/type_graph.h"
#include "../preprocess/type_registry.h"
#include "../visitor/pass_check_type_of.h"
#include "../visitor/pass_create.h"
#include "../visitor/pass_unwrap_destroy.h"
#include "../preprocess/emit_plan.h"
#include "../preprocess/comptime_prepare.h"
#include "preprocess/preprocess.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared in parse.c */
extern char* cc_blank_comptime_blocks_for_prep(const char* src, size_t n);

/* `#pragma cc_depends("...")` is a build-graph directive consumed by the driver
 * (it folds the named file's content into the emit cache key; see
 * COMPTIME_CAPABILITY_MODEL.md §2).  It carries no codegen meaning, so blank it
 * in place here — before lowering — so it never reaches the emitted C and never
 * trips the host compiler's unknown-pragma warning.  Length-preserving (spaces),
 * so source offsets/line numbers are unaffected. */
static void cc__blank_pragma_cc_depends(char* buf, size_t len) {
    size_t i = 0;
    if (!buf) return;
    while (i < len) {
        size_t ls = i;
        size_t p = i;
        while (p < len && (buf[p] == ' ' || buf[p] == '\t')) p++;
        if (p < len && buf[p] == '#') {
            size_t q = p + 1;
            while (q < len && (buf[q] == ' ' || buf[q] == '\t')) q++;
            if (q + 6 <= len && memcmp(buf + q, "pragma", 6) == 0) {
                size_t r = q + 6;
                while (r < len && (buf[r] == ' ' || buf[r] == '\t')) r++;
                if (r + 10 <= len && memcmp(buf + r, "cc_depends", 10) == 0) {
                    size_t e = ls;
                    while (e < len && buf[e] != '\n') e++;
                    for (size_t k = ls; k < e; k++) buf[k] = ' ';
                    i = e;
                    continue;
                }
            }
        }
        while (i < len && buf[i] != '\n') i++;
        if (i < len) i++;
    }
}

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

    cc__blank_pragma_cc_depends(buf, got);
    cc_reset_included_cch_sources();

    {
        char* lowered = cc_rewrite_local_cch_includes_to_lowered_headers(buf, got, input_path);
        if (lowered) { free(buf); buf = lowered; got = strlen(buf); }
    }
    {
        char* lowered = cc_rewrite_system_cch_includes_to_lowered_headers(buf, got);
        if (lowered) { free(buf); buf = lowered; got = strlen(buf); }
    }
    /* Header-defined generic factories: harvest CC_GENERIC_FACTORY/_EXTEND blocks
     * from the included local .cch files into this TU's comptime scope so they
     * register and run alongside .ccs-defined factories.  Appended AFTER the
     * user body (so the body's own physical line numbers — used by use-site
     * diagnostics — are unperturbed); each block carries a `#line` back to its
     * .cch for provenance.  No-op when no included header defines a factory. */
    {
        char* harvested = cc_harvest_local_header_factories();
        if (harvested) {
            size_t hlen = strlen(harvested);
            /* At most one separator newline: the body usually already ends
             * with `\n`, and harvested blocks start with `#line`.  A second
             * blank would inherit the TU's last #line and map past EOF. */
            size_t sep = (got > 0 && buf[got - 1] == '\n') ? 0 : 1;
            char* nb = (char*)malloc(got + sep + hlen + 1);
            if (nb) {
                memcpy(nb, buf, got);
                if (sep) nb[got] = '\n';
                memcpy(nb + got + sep, harvested, hlen + 1);
                free(buf);
                buf = nb;
                got = got + sep + hlen;
            }
            free(harvested);
        }
    }
    {
        char* harvested = cc_harvest_header_comptime_functions();
        if (harvested) {
            size_t hlen = strlen(harvested);
            size_t sep = (got > 0 && buf[got - 1] == '\n') ? 0 : 1;
            char* nb = (char*)malloc(got + sep + hlen + 1);
            if (!nb) { free(harvested); goto fail_buf; }
            memcpy(nb, buf, got);
            if (sep) nb[got] = '\n';
            memcpy(nb + got + sep, harvested, hlen + 1);
            free(harvested);
            free(buf);
            buf = nb;
            got += sep + hlen;
        }
    }
    /* Local-header `@comptime { }` blocks (e.g. static_map): see
     * cc_harvest_local_header_comptime_blocks — blanked from .h, re-run here. */
    {
        char* harvested = cc_harvest_local_header_comptime_blocks();
        if (harvested) {
            size_t hlen = strlen(harvested);
            size_t sep = (got > 0 && buf[got - 1] == '\n') ? 0 : 1;
            char* nb = (char*)malloc(got + sep + hlen + 1);
            if (!nb) { free(harvested); goto fail_buf; }
            memcpy(nb, buf, got);
            if (sep) nb[got] = '\n';
            memcpy(nb + got + sep, harvested, hlen + 1);
            free(harvested);
            free(buf);
            buf = nb;
            got += sep + hlen;
        }
    }
    /* Seam: expand `@comptime for`/`if`, then lower `@emit`/`@string` templates
     * before comptime execution (see cc_comptime_prepare_source). */
    if (cc_comptime_prepare_source(&buf, &got, input_path) != 0) goto fail_buf;
    cc_emit_plan_clear_generic_factory_registrations();
    cc_emit_plan_clear_comptime_fragments();
    if (cc_emit_plan_exec_comptime_blocks(buf, got, input_path) != 0) goto fail_buf;
    cc_emit_plan_collect_comptime_emits(buf, got);
    /* Generic factory dylibs compile lazily at Name::[args] use sites. */
    cc_emit_plan_clear_comptime_instantiations();
    cc_emit_plan_collect_comptime_instantiations(buf, got);
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
        /* Create-arg typing consults the global type registry (literals and
         * named vars). Install/clear it here: canonicalize would do this later,
         * but create rewrite runs before that. Previously this was an accidental
         * side effect of phase-1 over the include-expanded comptime buffer. */
        if (!cc_type_graph_ensure_global_cleared()) goto fail_buf;
        char* reg = cc_rewrite_registered_type_create_destroy(buf, got, input_path, symbols);
        if (reg == (char*)-1) goto fail_buf;
        if (reg) { free(buf); buf = reg; got = strlen(buf); }
    }

    cc_unwrap_destroy_set_symbols(symbols);
    char* canonical = for_reparse
        ? cc_preprocess_canonicalize(buf, got, input_path, 1, 0)
        : cc_preprocess_canonicalize(buf, got, input_path, 0, 1);
    cc_unwrap_destroy_set_symbols(NULL);
    if (!canonical) { free(buf); goto fail; }

    /* `@as` is AST / reflect-table only — erase from host-C-shaped buffers.
     * Comptime harvests `is_as` from Concurrent-C source before this strip
     * (or from emitted tables); never from comment markers. */
    {
        size_t clen = strlen(canonical);
        char* as_rw = cc_rewrite_as_attr_to_comment(canonical, clen);
        if (as_rw) { free(canonical); canonical = as_rw; }
    }

    /* --emit-c-inspect: on a clean lowering, dump the full merged translation
     * unit (post-comptime, post-generic-instantiation, blanked @comptime) to the
     * requested path.  The failure case (generic factory emits invalid C) is
     * handled deeper in the rewrite, where it reconstructs the TU up to the first
     * blocking error.  Initial parse only — reparses reuse this buffer. */
    if (!for_reparse) {
        const char* insp = getenv("CC_EMIT_C_INSPECT");
        if (insp && insp[0]) {
            FILE* f = fopen(insp, "w");
            if (f) {
                fputs("/* CC: merged translation unit (--emit-c-inspect). */\n", f);
                fwrite(canonical, 1, strlen(canonical), f);
                fclose(f);
                fprintf(stderr, "note: merged translation unit written to %s\n", insp);
            }
        }
    }

    /* Closure-ID marker injection (milestone 4b).  Insert /\*CC_CLO:N*\/
     * block comments immediately before every closure literal, in source
     * order, into the *canonical* (codegen) buffer.  This buffer becomes
     * `out->buffer_codegen` (the text the visitor's closure-literal pass
     * later walks), so the markers give that pass an exact, comment-safe
     * anchor for each closure — replacing the brittle `(line,col)` +
     * forward-`=>`-scan recovery heuristic.  Markers are inert C comments:
     * TCC strips them from the parse, and `cc__strip_cc_decl_markers`
     * strips them from the emitted C.  Initial parse only (reparses reuse
     * the already-marked `src_ufcs`).  Markers are the ONLY closure
     * identity — the heuristic fallback is gone, so there is deliberately
     * no disable knob (the old CC_NO_CLOSURE_MARKERS would now just
     * guarantee a marker/node mismatch error on every closure file). */
    if (!for_reparse) {
        size_t can_len = strlen(canonical);
        size_t cm_len = 0;
        char* cm = cc_inject_closure_markers(canonical, can_len, &cm_len);
        if (cm) {
            free(canonical);
            canonical = cm;
            if (getenv("CC_DEBUG_CLOSURE_MARKERS")) {
                fprintf(stderr, "[cc:clo-markers] %s: %zu -> %zu bytes (codegen buffer)\n",
                        input_path ? input_path : "<input>", can_len, cm_len);
            }
        }
    }

    char* pp = cc_preprocess_emit_splice(canonical, strlen(canonical), input_path, for_reparse);
    if (!pp) {
        free(buf);
        free(canonical);
        goto fail;
    }

    /* Compile-time check: every `type_of(T)` / `cc_type_of("T")` call
     * in this TU should reference a type whose `cc_type_info` will be
     * registered in this binary.  The check runs ONLY on the initial
     * parse (reparses re-traverse the same source; one diag per real
     * site is enough), and reads from the RAW user `buf` — not the
     * preprocessed `pp` — because `type_of(T)` is a user-facing macro
     * that hasn't been CPP-expanded yet, so we still see the literal
     * `T` token the user wrote.
     *
     * On the type-registry ratchet rule (PASS_INVENTORY rule #6):
     * this call is a single inspect-only read at the natural seam
     * where the preprocess pass above just finished populating the
     * global registry.  We are not extending the registry's
     * lifecycle or installing a new owner — just observing the
     * Vec/Map instantiations collected during generic-container
     * lowering so we can validate `type_of(CCVec_T)` references
     * against them.  Threading an explicit `CCTypeRegistry*` here
     * would require widening every preprocess return type up to
     * `cc_build_parse_input`; that is a worthwhile follow-up but
     * out of scope for this commit. */
    if (!for_reparse &&
        (cc_contains_token_top_level(buf, got, "type_of") ||
         cc_contains_token_top_level(buf, got, "cc_type_of"))) {
        size_t before = (size_t)cc_diag_error_count();
        cc__check_type_of_calls(buf, got, input_path,
                                cc_type_registry_get_global());
        /* Strict mode emits diagnostics as `CC_DIAG_ERROR`, which
         * bumps the global error count.  Fail the build immediately
         * if that happened — the buffered diags are already on the
         * driver's path to `cc_diag_print_all`. */
        if ((size_t)cc_diag_error_count() > before) {
            free(buf);
            free(pp);
            free(canonical);
            goto fail;
        }
    }
    free(buf);

    /* Stash canonical buffer for visit_codegen; emit-ready splice is parse-only. */
    if (!for_reparse) {
        out->buffer_codegen = canonical;
        out->buffer_codegen_len = strlen(canonical);
        canonical = NULL;
        /* Local quoted .cch were rewritten to #include out/include/*.h above.
         * Splice those bodies into the UFCS/codegen buffer so phase3 sees them
         * with parent-TU symbols (e.g. CC_MAP_DECL_UFCS). Write-back happens
         * after UFCS in visit_codegen. */
        {
            char* spliced = cc_splice_local_lowered_headers_for_codegen(
                out->buffer_codegen, out->buffer_codegen_len);
            if (spliced) {
                free(out->buffer_codegen);
                out->buffer_codegen = spliced;
                out->buffer_codegen_len = strlen(spliced);
            }
        }
    } else {
        free(canonical);
    }

    /* M7: pre-expand via CPP, applied after all CC text passes have run.
     * Resolves the prepended `#include` directives added by preprocessing
     * (containers, result types) so
     * TCC's second-pass parser sees a fully-expanded translation unit.
     *
     * Combined with the scanner's `in_pp` tracking (which makes phase-1
     * text passes skip `#define` directive bodies), this lets macro
     * definitions whose body contains CC syntax (`#define CHAN(T) T[~4 >]`)
     * survive intact through phase-1; CPP then expands the macro at the
     * call site. Full end-to-end support for macro-produced CC syntax
     * still requires post-expand re-lowering (M7.C) which needs the type
     * registry to survive the second pass — tracked separately. */
    /* Unconditional (2026-05-29): the initial-parse buffer is always
     * pre-expanded through TCC's CPP.  The legacy non-expanded path and its
     * `CC_PRE_EXPAND` opt-out were collapsed once the full suite + real
     * projects (redis, pigz) passed identically both ways and the emit path
     * was confirmed to read the (never-pre-expanded) `src_ufcs` text, so the
     * mode never affected emitted C — only the strength of initial-parse
     * analysis.  Reparses still never pre-expand (they bypass this function
     * entirely; see cc__reparse_source_to_ast_ex in visit_codegen.c). */
    if (!for_reparse) {
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
            /* M7.C: post-expand re-lower of CC type syntax that may have
             * appeared via macro expansion (e.g. `int[~4 >]` from
             * `#define CHAN(T) T[~4 >]` invoked as `CHAN(int)`). Uses a
             * registry-preserving variant so existing Result/Vec/Map type
             * registrations from initial canonicalization survive.
             *
             * Stash the pre-relower buffer (still has `[~ ... >]` brackets)
             * for downstream visitor passes that need to introspect chan
             * handle bracket specs of macro-generated decls (M7.C3). */
            /* Stash + re-lower only when CPP may have produced residual CC
             * type surface (macro-expanded `[~` / `[:` / `::[` / `@string` /
             * `@slice`).  Redis pays a full expanded-TU clone + walk
             * otherwise for a no-op. */
            int need_relower =
                (memmem(pp, exp_len, "[~", 2) != NULL) ||
                (memmem(pp, exp_len, "[:", 2) != NULL) ||
                (memmem(pp, exp_len, "::[", 3) != NULL) ||
                cc_contains_token_top_level(pp, exp_len, "@string") ||
                cc_contains_token_top_level(pp, exp_len, "@slice");
            if (need_relower) {
                /* Stash a copy of the post-CPP-expand buffer BEFORE the
                 * relower so the visitor's channel-pair scanner can fall
                 * back to a view that still has `[~ ... >]` brackets for
                 * macro-generated decls (M7.C3). */
                char* pre_relower_copy = (char*)malloc(exp_len + 1);
                if (pre_relower_copy) {
                    memcpy(pre_relower_copy, pp, exp_len);
                    pre_relower_copy[exp_len] = '\0';
                    out->buffer_pre_relower = pre_relower_copy;
                    out->buffer_pre_relower_len = exp_len;
                }
                char* relowered =
                    cc_relower_cc_type_syntax_preserving_registry(pp, exp_len,
                                                                 input_path);
                if (relowered) {
                    free(pp);
                    pp = relowered;
                    if (getenv("CC_DEBUG_PRE_EXPAND")) {
                        fprintf(stderr,
                                "[cc:pre-expand] re-lowered post-expand CC "
                                "type syntax\n");
                    }
                }
            } else if (getenv("CC_DEBUG_PRE_EXPAND")) {
                fprintf(stderr,
                        "[cc:pre-expand] skip re-lower (no residual CC type "
                        "surface)\n");
            }
        } else if (getenv("CC_DEBUG_PRE_EXPAND")) {
            fprintf(stderr, "[cc:pre-expand] %s: cc_cpp_expand failed, falling back\n",
                    input_path ? input_path : "<input>");
        }
    }

    /* L2 prelude rewriter (2026-05-27): convert a small set of
     * *standard C* idioms that TCC's stub-AST parser rejects into
     * TCC-acceptable equivalents.  Runs LAST in the pipeline so
     * it sees the same buffer the parser will see, including any
     * macro-expanded text produced by the M7 CPP-expand step
     * above.  See cc/src/preprocess/cc_l2_rewriter.h for the
     * current idiom list and the "user-facing surface ratchet"
     * rationale (PASS_INVENTORY rule #7). */
    {
        size_t pp_len = strlen(pp);
        size_t l2_len = 0;
        char* l2 = cc_l2_rewrite_all(pp, pp_len, &l2_len);
        if (l2) {
            free(pp);
            pp = l2;
            if (getenv("CC_DEBUG_L2_REWRITE")) {
                fprintf(stderr, "[cc:l2-rewrite] %s: %zu -> %zu bytes\n",
                        input_path ? input_path : "<input>", pp_len, l2_len);
            }
        }
    }

    /* (Closure-ID markers are injected into `canonical`/`buffer_codegen`
     * above, not into the parse buffer `pp`: TCC strips the comment markers
     * during parse, and the visitor's closure pass walks the codegen buffer,
     * not this one.) */

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
    free(in->buffer_pre_relower);
    in->buffer_pre_relower = NULL;
    in->buffer_pre_relower_len = 0;
    free(in->buffer_codegen);
    in->buffer_codegen = NULL;
    in->buffer_codegen_len = 0;
    cc_source_map_free(in->source_map);
    in->source_map = NULL;
}
