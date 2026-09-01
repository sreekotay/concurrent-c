/* Product-path comptime: reuse legacy prepare/exec/splice (not a pp_emit VM). */
#include "shadow_comptime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "preprocess/comptime_prepare.h"
#include "preprocess/emit_plan.h"
#include "preprocess/preprocess.h"
#include "preprocess/unit_header.h"
#include "util/path.h"
#include "util/text.h"
#include "util/text_scan.h"

static int shadow_ct_prof_on(void) {
    static int once = 0, on = 0;
    if (!once) {
        const char* e = getenv("CC_SHADOW_PROFILE");
        if (!e || !e[0] || e[0] == '0') e = getenv("CC_CCC_PROFILE");
        on = e && e[0] && e[0] != '0';
        once = 1;
    }
    return on;
}

static long long shadow_ct_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void shadow_ct_prof(const char* name, long long t0) {
    if (!shadow_ct_prof_on()) return;
    {
        long long now = shadow_ct_now_ms();
        fprintf(stderr, "ct_profile: %-22s %4lld ms  wall=%lld\n", name,
                now - t0, now);
    }
}

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
    {
        size_t skip = cc_unit_header_skip(buf, n);
        if (skip > 0 && skip <= n) {
            size_t blank = skip;
            if (blank > 0 && buf[blank - 1] == '\n') blank--;
            memset(buf, ' ', blank);
        }
    }
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

/* Original `.cch` text for lowered quoted headers — harvest `#define`
 * bodies that carry `@destroy` (the lowered `.h` has those attrs stripped). */
static char* shadow_ct_life_macro_extra(size_t* out_n) {
    size_t i, n = 0, cap = 0;
    char* buf = NULL;
    if (out_n) *out_n = 0;
    for (i = 0; i < cc_lowered_local_header_count(); i++) {
        const char* path = cc_lowered_local_header_source_path(i);
        size_t fn = 0;
        char* f;
        if (!path || !path[0]) continue;
        f = shadow_ct_read_file(path, &fn);
        if (!f) continue;
        if (n + fn + 1 >= cap) {
            size_t nc = cap ? cap * 2 + fn + 64 : fn + 256;
            char* nb = (char*)realloc(buf, nc + 1);
            if (!nb) { free(f); free(buf); return NULL; }
            buf = nb;
            cap = nc;
        }
        memcpy(buf + n, f, fn);
        n += fn;
        buf[n++] = '\n';
        free(f);
    }
    if (!buf) return NULL;
    buf[n] = 0;
    if (out_n) *out_n = n;
    return buf;
}

static int shadow_ct_apply_life_macros(char** buf, size_t* n) {
    char* extra;
    size_t extra_n = 0;
    char* r;
    if (!buf || !*buf || !n) return -1;
    extra = shadow_ct_life_macro_extra(&extra_n);
    r = cc_rewrite_life_macros(*buf, *n, extra, extra_n);
    free(extra);
    if (!r) return 0;
    free(*buf);
    *buf = r;
    *n = strlen(r);
    return 0;
}

/* Space-blank @comptime {…} / @comptime fn/const decls (layout-preserving).
 * File-scope `@comptime {…}` leaves `enum{__ccs<body_l>=0};` on one all-space
 * line. A block inside `enum { … }` leaves `__ccs<body_l>=0,` (a dummy
 * enumerator) so CC_EMIT_AT_COMPTIME_SITE can splice members there. In-function
 * blocks stay space-blank only (whitelist rejects enum as a statement).
 * Never overwrite a newline. Leaves CC_GENERIC_FACTORY sugar intact.
 * Unmatched braces → NULL (same fail-loud contract as header lower_header). */
static void shadow_ct_blank_unterminated(const char* src, size_t at,
                                         const char* what) {
    size_t line = 1, k;
    for (k = 0; k < at; k++)
        if (src[k] == '\n') line++;
    fprintf(stderr,
            "error: unterminated '%s' at line %zu; refusing silent skip of "
            "the @comptime blanker\n",
            what ? what : "@comptime", line);
}

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
                    shadow_ct_blank_unterminated(src, i, "@comptime {…}");
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
                    shadow_ct_blank_unterminated(
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
                                shadow_ct_blank_unterminated(
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
                            shadow_ct_blank_unterminated(src, i,
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

/* `@grammar(cli)` is a comptime engine (`cli` in cli.cch). The seam rewrites
 * it to `@comptime { cli(...) }`; do not mask the keyword before that pass. */

/* Apply quoted `.cch` → lowered `.h` (or impl-grade splice).  Interface
 * headers — including `@typeview` / `@typehooks` — take the stdlib path.
 * Returns -1 when a local header could not be lowered. */
static int shadow_ct_apply_local_cch_rewrite(char** buf, size_t* n,
                                            const char* input_path) {
    char* lowered;
    if (!buf || !*buf || !n) return -1;
    {
        long long t0 = shadow_ct_now_ms();
        lowered = cc_rewrite_local_cch_includes_to_lowered_headers(*buf, *n,
                                                                  input_path);
        shadow_ct_prof("cch_rewrite", t0);
    }
    if (cc_local_header_lower_failed()) {
        free(lowered);
        return -1;
    }
    if (lowered) {
        free(*buf);
        *buf = lowered;
        *n = strlen(lowered);
    }
    return 0;
}

/* Whitelist stage1 buffer: interface quoted `.cch` includes are rewritten
 * to lowered `.h` (stdlib model); impl-grade headers still splice.
 * `@comptime if`/value resolved and `@comptime` blocks blanked.
 * Exec/fragments still use the full prepare path below. */
static char* shadow_ct_stage1_src(const char* input_path, size_t* out_n) {
    char* buf;
    size_t n = 0;
    char* r;
    if (out_n) *out_n = 0;
    buf = shadow_ct_read_file(input_path, &n);
    if (!buf) return NULL;

    if (shadow_ct_apply_local_cch_rewrite(&buf, &n, input_path) != 0) {
        free(buf);
        return NULL;
    }

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

    /* `@typehooks on T {…}` → `@comptime { cc_type_register(...) }` before blank. */
    r = cc_rewrite_typehooks_to_register(buf, n);
    if (r) {
        free(buf);
        buf = r;
        n = strlen(buf);
    }

    /* `#define` / header `@destroy` — expand before comptime blank. */
    if (shadow_ct_apply_life_macros(&buf, &n) != 0) {
        free(buf);
        return NULL;
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
        long long t0 = shadow_ct_now_ms();
        if (shadow_ct_apply_local_cch_rewrite(&buf, &n, input_path) != 0) {
            free(buf);
            return NULL;
        }
        shadow_ct_prof("load_rewrite", t0);
    }
    {
        long long t0 = shadow_ct_now_ms();
        char* lowered = cc_rewrite_system_cch_includes_to_lowered_headers(buf, n);
        if (lowered) {
            free(buf);
            buf = lowered;
            n = strlen(buf);
        }
        shadow_ct_prof("load_sysinc", t0);
    }
    {
        long long t0 = shadow_ct_now_ms();
        shadow_ct_append_harvest(&buf, &n, cc_harvest_local_header_factories());
        shadow_ct_append_harvest(&buf, &n, cc_harvest_header_comptime_functions());
        shadow_ct_append_harvest(&buf, &n, cc_harvest_local_header_comptime_blocks());
        shadow_ct_prof("load_harvest", t0);
    }
    {
        long long t0 = shadow_ct_now_ms();
        char* th = cc_rewrite_typehooks_to_register(buf, n);
        if (th) {
            free(buf);
            buf = th;
            n = strlen(buf);
        }
        if (shadow_ct_apply_life_macros(&buf, &n) != 0) {
            free(buf);
            return NULL;
        }
        shadow_ct_prof("load_typehooks", t0);
    }
    if (out_n) *out_n = n;
    return buf;
}

/* True when this file (or a quoted `.cch` it includes) might need the
 * type-pass harvest. Full rewrite+harvest just to return NULL was the
 * bulk of `type_pass_skip` on projects with no `@comptime`. */
static int shadow_ct_type_pass_needles(const char* src, size_t n) {
    /* Same second gate as after harvest: `@typehooks` → `cc_type_register`
     * does not need the field-registry parse. Only these do. */
    return cc_contains_token_top_level(src, n, "type_of") ||
           cc_contains_token_top_level(src, n, "cc_reflect_field_") ||
           cc_contains_token_top_level(src, n, "__cc_rf_");
}

static int shadow_ct_quoted_cch_rel(const char* line, size_t len, char* rel,
                                    size_t cap) {
    size_t i = 0, q, e;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != '#') return 0;
    i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 7 > len || memcmp(line + i, "include", 7) != 0) return 0;
    i += 7;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= len || line[i] != '"') return 0;
    i++;
    q = i;
    while (i < len && line[i] != '"') i++;
    if (i >= len || i < q + 4) return 0;
    e = i - q;
    if (memcmp(line + q + e - 4, ".cch", 4) != 0) return 0;
    if (e >= cap) return 0;
    memcpy(rel, line + q, e);
    rel[e] = 0;
    return 1;
}

static int shadow_ct_type_pass_tree_needed(const char* path, const char* alt_dir,
                                          int depth) {
    char* buf;
    size_t n = 0, off = 0;
    char dir[1024];
    const char* slash;
    if (!path || !path[0] || depth > 24) return 0;
    buf = shadow_ct_read_file(path, &n);
    if (!buf) return 0;
    if (shadow_ct_type_pass_needles(buf, n)) {
        free(buf);
        return 1;
    }
    slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = 0;
    } else {
        memcpy(dir, ".", 2);
    }
    while (off < n) {
        size_t end = off;
        char rel[512];
        char child[1024];
        int hit = 0;
        while (end < n && buf[end] != '\n') end++;
        if (shadow_ct_quoted_cch_rel(buf + off, end - off, rel, sizeof(rel))) {
            if (rel[0] == '/')
                snprintf(child, sizeof(child), "%s", rel);
            else
                snprintf(child, sizeof(child), "%s/%s", dir, rel);
            hit = shadow_ct_type_pass_tree_needed(child, NULL, depth + 1);
            if (!hit && alt_dir && alt_dir[0] && rel[0] != '/') {
                snprintf(child, sizeof(child), "%s/%s", alt_dir, rel);
                hit = shadow_ct_type_pass_tree_needed(child, NULL, depth + 1);
            }
            if (hit) {
                free(buf);
                return 1;
            }
        }
        off = (end < n) ? end + 1 : end;
    }
    free(buf);
    return 0;
}

char* shadow_comptime_type_pass_src(const char* input_path, size_t* out_n) {
    char* buf;
    size_t n = 0;
    char* blanked;
    if (out_n) *out_n = 0;
    if (!input_path || !input_path[0]) return NULL;
    shadow_ct_ensure_repo_root(input_path);
    {
        const char* qd = getenv("SHADOW_QUOTE_DIR");
        if (!shadow_ct_type_pass_tree_needed(input_path, qd, 0)) {
            cc_ct_field_reg_set_type_pass_skipped(0);
            return NULL;
        }
    }
    buf = shadow_ct_load_with_harvest(input_path, &n);
    if (!buf) return NULL;
    /* Skip the type-pass parse+harvest when the TU (incl. harvest) has no
     * `@comptime` — field registry is only consumed by comptime exec. */
    if (!cc_contains_token_top_level(buf, n, "@comptime")) {
        cc_ct_field_reg_set_type_pass_skipped(0);
        free(buf);
        return NULL;
    }
    /* `@typehooks` → `@comptime { cc_type_register(...) }` and similar do not
     * need `__cc_rf_*` / field registry. Only run the type-pass harvest when
     * the harvested TU actually mentions field reflection. */
    if (!cc_contains_token_top_level(buf, n, "type_of") &&
        !cc_contains_token_top_level(buf, n, "cc_reflect_field_") &&
        !cc_contains_token_top_level(buf, n, "__cc_rf_")) {
        cc_ct_field_reg_set_type_pass_skipped(1);
        free(buf);
        return NULL;
    }
    cc_ct_field_reg_set_type_pass_skipped(0);
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
    {
        long long t0 = shadow_ct_now_ms();
        buf = shadow_ct_load_with_harvest(input_path, &n);
        shadow_ct_prof("exec_load", t0);
    }
    if (!buf) {
        fprintf(stderr, "%s:1:1: error: comptime: cannot read input\n",
                input_path);
        return -1;
    }

    {
        long long t0 = shadow_ct_now_ms();
        if (cc_comptime_prepare_source(&buf, &n, input_path) != 0) {
            free(buf);
            return -1;
        }
        shadow_ct_prof("exec_prepare", t0);
    }
    cc_emit_plan_clear_generic_factory_registrations();
    cc_emit_plan_clear_comptime_fragments();
    /* Field is_as comes from type-pass registry via cc_reflect_field_*;
     * reflect_src stays Concurrent-C for methods / fallbacks. */
    {
        long long t0 = shadow_ct_now_ms();
        if (cc_emit_plan_exec_comptime_blocks(buf, n, input_path) != 0) {
            free(buf);
            return -1;
        }
        shadow_ct_prof("exec_blocks", t0);
    }
    cc_emit_plan_collect_comptime_emits(buf, n);
    cc_emit_plan_clear_comptime_instantiations();
    cc_emit_plan_collect_comptime_instantiations(buf, n);
    free(buf);

    if (out_stage1_src) {
        long long t0 = shadow_ct_now_ms();
        char* s1 = shadow_ct_stage1_src(input_path, out_stage1_len);
        shadow_ct_prof("exec_stage1", t0);
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
