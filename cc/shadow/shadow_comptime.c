/* Product-path comptime: reuse legacy prepare/exec/splice (not a pp_emit VM). */
#include "shadow_comptime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preprocess/comptime_prepare.h"
#include "preprocess/emit_plan.h"
#include "preprocess/preprocess.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"

static void shadow_ct_ensure_repo_root(const char* input_path) {
    char root[1024];
    if (getenv("CC_REPO_ROOT") && getenv("CC_REPO_ROOT")[0]) return;
    root[0] = 0;
    if (cc_path_find_repo_root(input_path, root, sizeof(root)) && root[0])
        setenv("CC_REPO_ROOT", root, 0);
}

static char* shadow_ct_read_file(const char* path, size_t* out_n) {
    FILE* f;
    char* buf = NULL;
    size_t n = 0, cap = 0;
    int c;
    if (out_n) *out_n = 0;
    f = fopen(path, "rb");
    if (!f) return NULL;
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= cap) {
            size_t nc = cap ? cap * 2 : 4096;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = nc;
        }
        buf[n++] = (char)c;
    }
    fclose(f);
    if (!buf) {
        buf = (char*)malloc(1);
        if (!buf) return NULL;
    }
    buf[n] = 0;
    if (out_n) *out_n = n;
    return buf;
}

static void shadow_ct_append_harvest(char** buf, size_t* n, char* harvested) {
    size_t hlen, sep;
    char* nb;
    if (!harvested) return;
    hlen = strlen(harvested);
    sep = (*n > 0 && (*buf)[*n - 1] == '\n') ? 0 : 1;
    nb = (char*)malloc(*n + sep + hlen + 1);
    if (!nb) {
        free(harvested);
        return;
    }
    memcpy(nb, *buf, *n);
    if (sep) nb[*n] = '\n';
    memcpy(nb + *n + sep, harvested, hlen + 1);
    free(*buf);
    free(harvested);
    *buf = nb;
    *n = *n + sep + hlen;
}

/* Space-blank @comptime {…} / @comptime fn/const decls (layout-preserving).
 * File-scope `@comptime {…}` also leaves `enum{__ccs<body_l>=0};` on one
 * all-space line inside the blanked span (never crossing newlines — eating a
 * newline shifts every later diagnostic line by -1). Nested / in-function
 * blocks stay space-blank only (whitelist rejects enum as a statement).
 * Leaves CC_GENERIC_FACTORY sugar intact for shadow instantiate. */
static char* shadow_ct_blank_comptime(const char* src, size_t n) {
    char* out;
    CCInertScan sc;
    int brace_depth = 0;
    if (!src) return NULL;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n);
    out[n] = '\0';
    cc_inert_scan_init(&sc, NULL);
    for (size_t i = 0; i < n;) {
        if (cc_inert_scan_step(&sc, src, n, &i)) continue;
        if (src[i] == '{') {
            brace_depth++;
            i++;
            continue;
        }
        if (src[i] == '}' && brace_depth > 0) {
            brace_depth--;
            i++;
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
                    i++;
                    continue;
                }
                for (size_t k = i; k <= body_r; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
                }
                if (file_scope) {
                    mlen = snprintf(marker, sizeof(marker),
                                    "enum{__ccs%zu=0};", body_l);
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
                    i++;
                    continue;
                }
                end = body_r;
                /* `@comptime if (…) {…} else {…}` */
                if (kw_len == 2) {
                    size_t e = cc_skip_ws_and_comments(src, n, body_r + 1);
                    if (e + 4 <= n && memcmp(src + e, "else", 4) == 0 &&
                        (e + 4 >= n || !cc_is_ident_char(src[e + 4]))) {
                        size_t eb = cc_skip_ws_and_comments(src, n, e + 4);
                        size_t er;
                        if (eb < n && src[eb] == '{' &&
                            cc_find_matching_brace(src, n, eb, &er))
                            end = er;
                    }
                }
                for (size_t k = i; k <= end; ++k) {
                    if (out[k] != '\n') out[k] = ' ';
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
                            i++;
                            continue;
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

/* `@grammar(cli)` is a builtin seam engine (same as rules/schema) — do not
 * mask it; `cc_rewrite_grammar_decls_text` splices the typed opts + helpers. */

/* Whitelist stage1 buffer: original spelling (keeps CC_GENERIC_FACTORY and
 * relative .cch includes) with @comptime if/value resolved and @comptime
 * blocks blanked. Exec/fragments still use the full prepare path below. */
static char* shadow_ct_stage1_src(const char* input_path, size_t* out_n) {
    char* buf;
    size_t n = 0;
    char* r;
    if (out_n) *out_n = 0;
    buf = shadow_ct_read_file(input_path, &n);
    if (!buf) return NULL;

    /* `@comptime <directive>(...);` module-export sugar expands to the
     * entry stanza BEFORE the blank pass below — a bare `@comptime` decl
     * is otherwise space-blanked, and the stanza is plain C the
     * whitelist emit must carry.  Same engine as the driver's prepare. */
    r = cc_rewrite_module_export_directives_text(buf, n, input_path);
    if (r == (char*)-1) {
        free(buf);
        return NULL;
    }
    if (r) {
        free(buf);
        buf = r;
        n = strlen(buf);
    }

    r = cc__resolve_comptime_if(buf, n, input_path);
    if (r == (char*)-1) {
        free(buf);
        return NULL;
    }
    if (r) {
        free(buf);
        buf = r;
        n = strlen(buf);
    }

    r = cc__resolve_comptime_value(buf, n, input_path);
    if (r == (char*)-1) {
        free(buf);
        return NULL;
    }
    if (r) {
        free(buf);
        buf = r;
        n = strlen(buf);
    }

    /* Splice @grammar(rules|schema|cli) → generated C. */
    r = cc_rewrite_grammar_decls_text(buf, n, input_path);
    if (r == (char*)-1) {
        free(buf);
        return NULL;
    }
    if (r) {
        free(buf);
        buf = r;
        n = strlen(buf);
    }

    {
        char* blanked = shadow_ct_blank_comptime(buf, n);
        free(buf);
        if (!blanked) return NULL;
        if (out_n) *out_n = strlen(blanked);
        return blanked;
    }
}

/* Shared load: file + .cch→.h rewrite + header harvest. */
static char* shadow_ct_load_with_harvest(const char* input_path, size_t* out_n) {
    char* buf;
    size_t n = 0;
    if (out_n) *out_n = 0;
    buf = shadow_ct_read_file(input_path, &n);
    if (!buf) return NULL;
    cc_reset_included_cch_sources();
    {
        char* lowered =
            cc_rewrite_local_cch_includes_to_lowered_headers(buf, n, input_path);
        if (lowered) {
            free(buf);
            buf = lowered;
            n = strlen(buf);
        }
    }
    {
        char* lowered = cc_rewrite_system_cch_includes_to_lowered_headers(buf, n);
        if (lowered) {
            free(buf);
            buf = lowered;
            n = strlen(buf);
        }
    }
    shadow_ct_append_harvest(&buf, &n, cc_harvest_local_header_factories());
    shadow_ct_append_harvest(&buf, &n, cc_harvest_header_comptime_functions());
    shadow_ct_append_harvest(&buf, &n, cc_harvest_local_header_comptime_blocks());
    if (out_n) *out_n = n;
    return buf;
}

char* shadow_comptime_type_pass_src(const char* input_path, size_t* out_n) {
    char* buf;
    size_t n = 0;
    char* blanked;
    if (out_n) *out_n = 0;
    if (!input_path || !input_path[0]) return NULL;
    shadow_ct_ensure_repo_root(input_path);
    buf = shadow_ct_load_with_harvest(input_path, &n);
    if (!buf) return NULL;
    /* Skip the second whitelist lower when the TU (incl. harvest) has no
     * `@comptime` — field registry is only consumed by comptime exec. */
    if (!cc_contains_token_top_level(buf, n, "@comptime")) {
        free(buf);
        return NULL;
    }
    blanked = shadow_ct_blank_comptime(buf, n);
    free(buf);
    if (!blanked) return NULL;
    if (out_n) *out_n = strlen(blanked);
    return blanked;
}

int shadow_comptime_exec_file(const char* input_path, char** out_stage1_src,
                              size_t* out_stage1_len) {
    char* buf;
    size_t n = 0;
    if (out_stage1_src) *out_stage1_src = NULL;
    if (out_stage1_len) *out_stage1_len = 0;
    if (!input_path || !input_path[0]) return -1;
    shadow_ct_ensure_repo_root(input_path);
    buf = shadow_ct_load_with_harvest(input_path, &n);
    if (!buf) {
        fprintf(stderr, "%s:1:1: error: comptime: cannot read input\n",
                input_path);
        return -1;
    }

    if (cc_comptime_prepare_source(&buf, &n, input_path) != 0) {
        free(buf);
        return -1;
    }
    cc_emit_plan_clear_generic_factory_registrations();
    cc_emit_plan_clear_comptime_fragments();
    /* Field is_as comes from type-pass registry via cc_reflect_field_*;
     * reflect_src stays Concurrent-C for methods / fallbacks. */
    if (cc_emit_plan_exec_comptime_blocks(buf, n, input_path) != 0) {
        free(buf);
        return -1;
    }
    cc_emit_plan_collect_comptime_emits(buf, n);
    cc_emit_plan_clear_comptime_instantiations();
    cc_emit_plan_collect_comptime_instantiations(buf, n);
    free(buf);

    if (out_stage1_src) {
        char* s1 = shadow_ct_stage1_src(input_path, out_stage1_len);
        if (!s1) return -1;
        *out_stage1_src = s1;
    }
    return 0;
}

int shadow_comptime_splice_emit(char** emit_buf, size_t* emit_len,
                                const char* input_path) {
    if (!emit_buf || !*emit_buf || !emit_len) return 0;
    shadow_ct_ensure_repo_root(input_path);
    if (cc_emit_plan_splice_comptime_fragments(emit_buf, emit_len, input_path) !=
        0)
        return -1;
    return 0;
}
