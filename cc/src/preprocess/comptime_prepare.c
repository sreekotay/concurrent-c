#include "comptime_prepare.h"

#include <stdlib.h>
#include <string.h>

#include "preprocess/preprocess.h"
#include "preprocess/template_scan.h"
#include "util/text.h"
#include "util/text_scan.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

int cc_comptime_unified_exec_enabled(void) {
    const char* flag = getenv("CC_COMPTIME_UNIFIED_EXEC");
    if (!flag || !flag[0]) return 1;
    if (flag[0] == '0' && flag[1] == '\0') return 0;
    if (flag[0] == '1' && flag[1] == '\0') return 1;
    return 1;
}

/* Space-blank @comptime {…} / @comptime fn/const decls (layout-preserving).
 * File-scope `@comptime {…}` leaves `enum{__ccs<body_l>=0};` on one all-space
 * line. A block inside `enum { … }` leaves `__ccs<body_l>=0,` (a dummy
 * enumerator) so CC_EMIT_AT_COMPTIME_SITE can splice members there. In-function
 * blocks stay space-blank only (whitelist rejects enum as a statement).
 * Never overwrite a newline. Leaves CC_GENERIC_FACTORY sugar intact.
 * Unmatched braces → NULL (same fail-loud contract as header lower_header). */
static void cc__blank_unterminated(const char* src, size_t at,
                                         const char* what) {
    size_t line = 1, k;
    for (k = 0; k < at; k++)
        if (src[k] == '\n') line++;
    fprintf(stderr,
            "error: unterminated '%s' at line %zu; refusing silent skip of "
            "the @comptime blanker\n",
            what ? what : "@comptime", line);
}

char* cc_comptime_blank_blocks(const char* src, size_t n) {
    char* out;
    CCInertScan sc;
    int brace_depth = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&sc, NULL);
    int pending_enum = 0;
    int enum_at[32];
    int nenum = 0;
    for (size_t i = 0; i < n;) {
        if (cc_inert_scan_step(&sc, src, n, &i)) continue;
        if (src[i] == '{') {
            if (pending_enum && nenum < 32)
                enum_at[nenum++] = brace_depth;
            pending_enum = 0;
            brace_depth++;
            i++;
            continue;
        }
        if (src[i] == '}' && brace_depth > 0) {
            brace_depth--;
            if (nenum && enum_at[nenum - 1] == brace_depth)
                nenum--;
            i++;
            continue;
        }
        if (pending_enum) {
            /* `enum` / `enum Tag` then `{` — whitespace must not drop the flag
             * or `enum {\n@comptime for` never plants an enumerator marker. */
            if (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' ||
                src[i] == '\r') {
                i++;
                continue;
            }
            if (cc_is_ident_start(src[i])) {
                while (i < n && cc_is_ident_char(src[i])) i++;
                continue;
            }
            pending_enum = 0;
        }
        if (src[i] == 'e' && i + 4 <= n && memcmp(src + i, "enum", 4) == 0 &&
            (i + 4 >= n || !cc_is_ident_char(src[i + 4])) &&
            (i == 0 || !cc_is_ident_char(src[i - 1]))) {
            pending_enum = 1;
            i += 4;
            continue;
        }
        if (src[i] != '@' || !cc_match_ident_kw(src, n, i + 1, "comptime")) {
            i++;
            continue;
        }
        {
            size_t kw_end = i + 1 + strlen("comptime");
            size_t body_l = cc_skip_ws_and_comments(src, n, kw_end);
            size_t body_r;
            int file_scope = (brace_depth == 0);
            if (body_l >= n) {
                i++;
                continue;
            }
            if (src[body_l] == '{') {
                char marker[64];
                int mlen;
                if (!cc_find_matching_brace(src, n, body_l, &body_r)) {
                    cc__blank_unterminated(src, i, "@comptime {…}");
                    free(out);
                    return NULL;
                }
                for (size_t k = i; k <= body_r; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                if (file_scope || nenum > 0) {
                    mlen = file_scope
                               ? snprintf(marker, sizeof(marker),
                                          "enum{__ccs%zu=0};", body_l)
                               : snprintf(marker, sizeof(marker),
                                          "__ccs%zu=0,", body_l);
                    /* Place on a single blanked line that has room — never
                     * overwrite '\n' (line-map must stay stable). */
                    if (mlen > 0 && (size_t)mlen < sizeof(marker)) {
                        size_t p = i;
                        while (p <= body_r) {
                            size_t line_end = p;
                            size_t room;
                            while (line_end <= body_r && out[line_end] != '\n')
                                line_end++;
                            room = line_end - p;
                            if (room >= (size_t)mlen) {
                                memcpy(out + p, marker, (size_t)mlen);
                                break;
                            }
                            p = (line_end <= body_r) ? line_end + 1 : body_r + 1;
                        }
                    }
                }
                i = body_r + 1;
                continue;
            }
            /* Type-pass / stage1: blank `@comptime for|if (...) {…}` so
             * whitelist parse sees plain C; expand runs on the original later. */
            if (cc_match_ident_kw(src, n, body_l, "for") ||
                cc_match_ident_kw(src, n, body_l, "if")) {
                size_t kw_len = cc_match_ident_kw(src, n, body_l, "for") ? 3 : 2;
                size_t lp = cc_skip_ws_and_comments(src, n, body_l + kw_len);
                size_t rp = 0, end = 0, after;
                if (lp >= n || src[lp] != '(' ||
                    !cc_find_matching_paren(src, n, lp, &rp)) {
                    i++;
                    continue;
                }
                after = cc_skip_ws_and_comments(src, n, rp + 1);
                if (after >= n || src[after] != '{' ||
                    !cc_find_matching_brace(src, n, after, &body_r)) {
                    cc__blank_unterminated(
                        src, i, kw_len == 3 ? "@comptime for" : "@comptime if");
                    free(out);
                    return NULL;
                }
                end = body_r;
                /* `@comptime if (…) {…} else {…}` */
                if (kw_len == 2) {
                    size_t e = cc_skip_ws_and_comments(src, n, body_r + 1);
                    if (e + 4 <= n && memcmp(src + e, "else", 4) == 0 &&
                        (e + 4 >= n || !cc_is_ident_char(src[e + 4]))) {
                        size_t eb = cc_skip_ws_and_comments(src, n, e + 4);
                        size_t er;
                        if (eb < n && src[eb] == '{') {
                            if (!cc_find_matching_brace(src, n, eb, &er)) {
                                cc__blank_unterminated(
                                    src, i, "@comptime if … else");
                                free(out);
                                return NULL;
                            }
                            end = er;
                        }
                    }
                }
                for (size_t k = i; k <= end; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                /* Inside `enum { }`: one dummy enumerator so unrolled
                 * `@emit(CC_EMIT_AT_COMPTIME_SITE)` splices into this list
                 * (stage1 never sees the inner `@comptime { }` wraps). */
                if (nenum > 0) {
                    char marker[64];
                    int mlen = snprintf(marker, sizeof(marker),
                                        "__ccs%zu=0,", body_l);
                    if (mlen > 0 && (size_t)mlen < sizeof(marker)) {
                        size_t p = i;
                        while (p <= end) {
                            size_t line_end = p;
                            size_t room;
                            while (line_end <= end && out[line_end] != '\n')
                                line_end++;
                            room = line_end - p;
                            if (room >= (size_t)mlen) {
                                memcpy(out + p, marker, (size_t)mlen);
                                break;
                            }
                            p = (line_end <= end) ? line_end + 1 : end + 1;
                        }
                    }
                }
                i = end + 1;
                continue;
            }
            /* @comptime fn/const decl — blank through body or ';'. */
            {
                size_t p = body_l, lpar = 0, rpar = 0, end = 0;
                {
                    size_t lp = cc_find_char_top_level(src, p, n, '(');
                    if (lp < n) lpar = lp;
                }
                if (lpar) {
                    if (!cc_find_matching_paren(src, n, lpar, &rpar)) {
                        i++;
                        continue;
                    }
                    p = cc_skip_ws_and_comments(src, n, rpar + 1);
                    if (p < n && src[p] == '{') {
                        if (!cc_find_matching_brace(src, n, p, &body_r)) {
                            cc__blank_unterminated(src, i,
                                                         "@comptime fn/const");
                            free(out);
                            return NULL;
                        }
                        end = body_r;
                    } else {
                        end = cc_find_char_top_level(src, p, n, ';');
                        if (end >= n) {
                            i++;
                            continue;
                        }
                    }
                } else {
                    end = cc_find_char_top_level(src, p, n, ';');
                    if (end >= n) {
                        i++;
                        continue;
                    }
                }
                for (size_t k = i; k <= end; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                i = end + 1;
            }
        }
    }
    return out;
}


int cc_comptime_prepare_source(char** inout_buf, size_t* inout_len,
                               const char* input_path) {
    return cc_comptime_prepare_source_ex(inout_buf, inout_len, input_path,
                                         CC_PREPARE_ALL);
}

int cc_comptime_prepare_source_ex(char** inout_buf, size_t* inout_len,
                                  const char* input_path, unsigned passes) {
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
    if (passes & CC_PREPARE_GRAMMAR) {
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
    if (passes & CC_PREPARE_MODULE_EXPORT) {
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
     * as visible; must happen before the C parse (syntax error otherwise).
     * The clean lowerer resolves these itself, from the index. */
    if (passes & CC_PREPARE_TYPE_SCOPED) {
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
    if (passes & CC_PREPARE_FACTORY_SUGAR) {
        factory = cc_rewrite_generic_factory_text(*inout_buf, *inout_len, input_path);
        if (factory == (char*)-1) return -1;
        if (factory) {
            free(*inout_buf);
            *inout_buf = factory;
            *inout_len = strlen(factory);
        }
    }

    /* Typed static_map(name, entries, flags) → layout-carrying internal call
     * before @comptime if/for so the executor sees the expanded arity. */
    if (passes & CC_PREPARE_STATIC_MAP) {
        char* sm = cc_rewrite_static_map_calls_text(*inout_buf, *inout_len, input_path);
        if (sm == (char*)-1) return -1;
        if (sm) {
            free(*inout_buf);
            *inout_buf = sm;
            *inout_len = strlen(sm);
        }
    }

    if (passes & CC_PREPARE_COMPTIME) {
        resolved = cc__resolve_comptime_if(*inout_buf, *inout_len, input_path);
        if (resolved == (char*)-1) return -1;
        if (resolved) {
            free(*inout_buf);
            *inout_buf = resolved;
            *inout_len = strlen(resolved);
        }
    }

    /* Value-position `@comptime(expr)`: evaluate and splice the projected C
     * literal in place.  Runs after `@comptime if/for` pruning (so only live
     * sites are evaluated) and before template lowering.  The splice lands in
     * the buffer that feeds both the parse buffer and `buffer_codegen`, so the
     * hoisted literal is visible in the lowered C — no anchor plumbing needed. */
    if (passes & CC_PREPARE_COMPTIME) {
        char* valued = cc__resolve_comptime_value(*inout_buf, *inout_len, input_path);
        if (valued == (char*)-1) return -1;
        if (valued) {
            free(*inout_buf);
            *inout_buf = valued;
            *inout_len = strlen(valued);
        }
    }

    if (!(passes & CC_PREPARE_TEMPLATES)) return 0;

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
