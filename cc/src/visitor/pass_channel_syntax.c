/* pass_channel_syntax.c - Channel syntax lowering passes.
 *
 * Extracted from visit_codegen.c for maintainability.
 */

#include "pass_channel_syntax.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/path.h"
#include "util/text.h"

/* Local aliases for shared helpers */
#define cc__sb_append_local cc_sb_append
#define cc__sb_append_cstr_local cc_sb_append_cstr
#define cc__is_ident_char_local2 cc_is_ident_char
#define cc__is_ident_start_local2 cc_is_ident_start
#define cc__skip_ws_local2 cc_skip_ws

/* Find channel declaration before a given offset.
 * Searches backwards for `name` with preceding [~ ... ] bracket spec. */
static int cc__find_chan_decl_before(const char* src,
                                     size_t len,
                                     size_t search_before_off,
                                     const char* name,
                                     size_t* out_lbrack,
                                     size_t* out_rbrack,
                                     size_t* out_ty_start) {
    if (!src || !name || !*name || !out_lbrack || !out_rbrack || !out_ty_start) return 0;
    size_t nm_len = strlen(name);
    if (search_before_off > len) search_before_off = len;

    for (size_t pos = search_before_off; pos-- > 0; ) {
        if (pos + nm_len > len) continue;
        if (memcmp(src + pos, name, nm_len) != 0) continue;
        char pre = (pos == 0) ? 0 : src[pos - 1];
        char post = (pos + nm_len < len) ? src[pos + nm_len] : 0;
        if (pre && cc__is_ident_char_local2(pre)) continue;
        if (post && cc__is_ident_char_local2(post)) continue;

        size_t scan = pos;
        size_t lbr = (size_t)-1;
        size_t rbr = (size_t)-1;
        for (size_t j = scan; j-- > 0; ) {
            char c = src[j];
            if (c == ';' || c == '{' || c == '}' || c == '\n') break;
            if (c == ']') { rbr = j; continue; }
            if (c == '[') {
                size_t k = j + 1;
                while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k < len && src[k] == '~' && rbr != (size_t)-1 && rbr > j) {
                    lbr = j;
                    break;
                }
            }
        }
        if (lbr == (size_t)-1 || rbr == (size_t)-1) continue;

        size_t ts = lbr;
        while (ts > 0) {
            char c = src[ts - 1];
            if (c == ';' || c == '{' || c == '}' || c == ',' || c == '(' || c == ')' || c == '\n') break;
            ts--;
        }
        while (ts < lbr && (src[ts] == ' ' || src[ts] == '\t')) ts++;

        *out_lbrack = lbr;
        *out_rbrack = rbr;
        *out_ty_start = ts;
        return 1;
    }
    return 0;
}

/* Parse channel bracket spec [~ ... >/< ] */
static int cc__parse_chan_bracket_spec(const CCVisitorCtx* ctx,
                                       const char* src,
                                       size_t len,
                                       size_t lbr,
                                       size_t rbr,
                                       int* out_is_tx,
                                       int* out_is_rx,
                                       long long* out_cap_int,
                                       char* out_cap_expr,
                                       size_t out_cap_expr_cap,
                                       int* out_bp_mode,
                                       int* out_mode,
                                       char* out_topology,
                                       size_t out_topology_cap,
                                       int* out_has_topology,
                                       int* out_unknown_token,
                                       int* out_allow_take,
                                       const char** out_elem_size_expr) {
    if (!src || lbr >= len || rbr >= len || rbr <= lbr) return 0;
    if (out_is_tx) *out_is_tx = 0;
    if (out_is_rx) *out_is_rx = 0;
    if (out_cap_int) *out_cap_int = -1;
    if (out_cap_expr && out_cap_expr_cap > 0) out_cap_expr[0] = 0;
    if (out_bp_mode) *out_bp_mode = -1;
    if (out_mode) *out_mode = -1;
    if (out_topology && out_topology_cap) out_topology[0] = 0;
    if (out_has_topology) *out_has_topology = 0;
    if (out_unknown_token) *out_unknown_token = 0;
    if (out_allow_take) *out_allow_take = 0;
    if (out_elem_size_expr) *out_elem_size_expr = "0";

    int saw_gt = 0, saw_lt = 0;
    for (size_t i = lbr; i <= rbr && i < len; i++) {
        if (src[i] == '>') saw_gt = 1;
        if (src[i] == '<') saw_lt = 1;
    }
    if (out_is_tx) *out_is_tx = saw_gt;
    if (out_is_rx) *out_is_rx = saw_lt;

    size_t t = lbr;
    while (t < rbr && src[t] != '~') t++;
    if (t < rbr && src[t] == '~') t++;
    while (t < rbr) {
        while (t < rbr && (src[t] == ' ' || src[t] == '\t')) t++;
        if (t >= rbr) break;
        char c = src[t];
        if (c == '>' || c == '<' || c == ',') { t++; continue; }
        
        /* Topology token */
        if ((c == 'N' || c == 'n' || c == '1') && (t + 1 < rbr && src[t + 1] == ':')) {
            char topo[8];
            size_t tn = 0;
            size_t tt = t;
            while (tt < rbr && tn + 1 < sizeof(topo)) {
                char tc = src[tt];
                if (tc == ' ' || tc == '\t' || tc == '>' || tc == '<' || tc == ',') break;
                topo[tn++] = tc;
                tt++;
            }
            topo[tn] = 0;
            if (tn == 3 && topo[1] == ':' &&
                ((topo[0] == '1') || (topo[0] == 'N') || (topo[0] == 'n')) &&
                ((topo[2] == '1') || (topo[2] == 'N') || (topo[2] == 'n'))) {
                if (out_topology && out_topology_cap) {
                    snprintf(out_topology, out_topology_cap, "%c:%c",
                             (topo[0] == 'n') ? 'N' : topo[0],
                             (topo[2] == 'n') ? 'N' : topo[2]);
                }
                if (out_has_topology) *out_has_topology = 1;
                t = tt;
                continue;
            }
        }
        
        /* Numeric capacity */
        if (c >= '0' && c <= '9') {
            long long cap = 0;
            while (t < rbr && (src[t] >= '0' && src[t] <= '9')) {
                cap = cap * 10 + (src[t] - '0');
                t++;
            }
            if (out_cap_int && *out_cap_int == -1) {
                *out_cap_int = cap;
            } else {
                if (out_unknown_token) *out_unknown_token = 1;
            }
            continue;
        }
        
        /* Alpha word */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            char word[128];
            size_t wn = 0;
            while (t < rbr && wn + 1 < sizeof(word)) {
                char wc = src[t];
                if (!((wc >= 'A' && wc <= 'Z') || (wc >= 'a' && wc <= 'z') || 
                      (wc >= '0' && wc <= '9') || wc == '_')) break;
                word[wn++] = wc;
                t++;
            }
            word[wn] = 0;
            
            char wlow[128];
            size_t wl = wn < sizeof(wlow) - 1 ? wn : (sizeof(wlow) - 1);
            for (size_t qi = 0; qi < wl; qi++) {
                char ch = word[qi];
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                wlow[qi] = ch;
            }
            wlow[wl] = 0;

            if (strcmp(wlow, "sync") == 0) {
                if (out_mode) *out_mode = 1;
            } else if (strcmp(wlow, "async") == 0) {
                if (out_mode) *out_mode = 0;
            } else if (strcmp(wlow, "drop") == 0 || strcmp(wlow, "dropnew") == 0 || 
                       strcmp(wlow, "drop_new") == 0) {
                if (out_bp_mode) *out_bp_mode = 1;
            } else if (strcmp(wlow, "dropold") == 0 || strcmp(wlow, "drop_old") == 0) {
                if (out_bp_mode) *out_bp_mode = 2;
            } else {
                if (out_cap_int && *out_cap_int == -1 && 
                    out_cap_expr && out_cap_expr_cap > 0 && out_cap_expr[0] == 0) {
                    if (wn < out_cap_expr_cap) {
                        memcpy(out_cap_expr, word, wn);
                        out_cap_expr[wn] = 0;
                        *out_cap_int = -2;
                    } else {
                        if (out_unknown_token) *out_unknown_token = 1;
                    }
                } else {
                    if (out_unknown_token) *out_unknown_token = 1;
                }
            }
            continue;
        }
        
        if (out_unknown_token) *out_unknown_token = 1;
        t++;
    }

    (void)ctx;
    return 1;
}

/* Buffer for dynamic sizeof expr */
static char cc__sizeof_expr_buf[256];

static int cc__elem_type_implies_take(const char* elem_ty, const char** out_elem_size_expr) {
    if (!elem_ty || !*elem_ty) return 0;
    if (strstr(elem_ty, "[:") || strstr(elem_ty, "CCSlice")) {
        if (out_elem_size_expr) *out_elem_size_expr = "sizeof(CCSlice)";
        return 1;
    }
    if (strchr(elem_ty, '*')) {
        if (out_elem_size_expr) *out_elem_size_expr = "sizeof(void*)";
        return 1;
    }
    if (out_elem_size_expr) {
        snprintf(cc__sizeof_expr_buf, sizeof(cc__sizeof_expr_buf), "sizeof(%s)", elem_ty);
        *out_elem_size_expr = cc__sizeof_expr_buf;
    }
    return 0;
}

char* cc__rewrite_channel_pair_calls_text(const CCVisitorCtx* ctx,
                                          const char* src,
                                          size_t len,
                                          size_t* out_len) {
    if (!src || !out_len) return NULL;
    *out_len = 0;
    char* out = NULL;
    size_t o_len = 0, o_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;

    while (i < len) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < len) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < len) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        /* Look for channel_pair token */
        if (c == 'c' && i + 12 < len && memcmp(src + i, "channel_pair", 12) == 0) {
            char pre = (i == 0) ? 0 : src[i - 1];
            char post = (i + 12 < len) ? src[i + 12] : 0;
            if ((i == 0 || !cc__is_ident_char_local2(pre)) && (i + 12 == len || !cc__is_ident_char_local2(post))) {
                size_t call_start = i;
                const char* p = src + i + 12;
                p = cc__skip_ws_local2(p);
                if (*p != '(') { i++; col++; continue; }
                p++;
                p = cc__skip_ws_local2(p);
                if (*p != '&') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair expects `&tx, &rx` at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                p++;
                p = cc__skip_ws_local2(p);
                if (!cc__is_ident_start_local2(*p)) { free(out); return NULL; }
                const char* tx_s = p;
                p++;
                while (cc__is_ident_char_local2(*p)) p++;
                size_t tx_n = (size_t)(p - tx_s);
                p = cc__skip_ws_local2(p);
                if (*p != ',') { free(out); return NULL; }
                p++;
                p = cc__skip_ws_local2(p);
                if (*p != '&') { free(out); return NULL; }
                p++;
                p = cc__skip_ws_local2(p);
                if (!cc__is_ident_start_local2(*p)) { free(out); return NULL; }
                const char* rx_s = p;
                p++;
                while (cc__is_ident_char_local2(*p)) p++;
                size_t rx_n = (size_t)(p - rx_s);
                p = cc__skip_ws_local2(p);
                if (*p != ')') { free(out); return NULL; }
                p++;
                const char* after = cc__skip_ws_local2(p);
                
                /* Detect expression vs statement form */
                int is_expression = 0;
                size_t assign_start = call_start;
                {
                    size_t scan = call_start;
                    while (scan > 0 && (src[scan-1] == ' ' || src[scan-1] == '\t')) scan--;
                    if (scan > 0 && src[scan-1] == '=') {
                        is_expression = 1;
                        scan--;
                        while (scan > 0 && (src[scan-1] == ' ' || src[scan-1] == '\t')) scan--;
                        while (scan > 0) {
                            char pc = src[scan-1];
                            if (pc == ';' || pc == '{' || pc == '}' || pc == '\n') break;
                            scan--;
                        }
                        assign_start = scan;
                    }
                }
                
                if (!is_expression && *after != ';') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair must be used as statement or expression at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }

                char tx_name[128], rx_name[128];
                if (tx_n >= sizeof(tx_name) || rx_n >= sizeof(rx_name)) { free(out); return NULL; }
                memcpy(tx_name, tx_s, tx_n); tx_name[tx_n] = 0;
                memcpy(rx_name, rx_s, rx_n); rx_name[rx_n] = 0;

                size_t tx_lbr=0, tx_rbr=0, tx_ts=0;
                size_t rx_lbr=0, rx_rbr=0, rx_ts=0;
                if (!cc__find_chan_decl_before(src, len, call_start, tx_name, &tx_lbr, &tx_rbr, &tx_ts) ||
                    !cc__find_chan_decl_before(src, len, call_start, rx_name, &rx_lbr, &rx_rbr, &rx_ts)) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair could not find declarations for '%s'/'%s' at %s:%d:%d\n",
                            tx_name, rx_name,
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }

                int tx_is_tx=0, tx_is_rx=0, rx_is_tx=0, rx_is_rx=0;
                long long tx_cap=-1, rx_cap=-1;
                int tx_mode=-1, rx_mode=-1;
                char tx_topo[8], rx_topo[8];
                char tx_cap_expr[128], rx_cap_expr[128];
                int tx_bp=-1, rx_bp=-1;
                int tx_has_topo=0, rx_has_topo=0;
                int tx_unknown=0, rx_unknown=0;
                int dummy_allow=0;
                const char* dummy_sz="0";
                
                cc__parse_chan_bracket_spec(ctx, src, len, tx_lbr, tx_rbr,
                                            &tx_is_tx, &tx_is_rx, &tx_cap,
                                            tx_cap_expr, sizeof(tx_cap_expr),
                                            &tx_bp, &tx_mode, tx_topo, sizeof(tx_topo),
                                            &tx_has_topo, &tx_unknown, &dummy_allow, &dummy_sz);
                cc__parse_chan_bracket_spec(ctx, src, len, rx_lbr, rx_rbr,
                                            &rx_is_tx, &rx_is_rx, &rx_cap,
                                            rx_cap_expr, sizeof(rx_cap_expr),
                                            &rx_bp, &rx_mode, rx_topo, sizeof(rx_topo),
                                            &rx_has_topo, &rx_unknown, &dummy_allow, &dummy_sz);

                if (!tx_is_tx || tx_is_rx || !rx_is_rx || rx_is_tx) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair requires send (>) then recv (<) at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }

                if (tx_unknown || rx_unknown) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair unknown token in spec at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                
                if (tx_mode == -1) tx_mode = 0;
                if (rx_mode == -1) rx_mode = 0;
                if (tx_mode != rx_mode) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair mode mismatch at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                
                if (tx_has_topo != rx_has_topo || (tx_has_topo && strcmp(tx_topo, rx_topo) != 0)) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair topology mismatch at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                
                if (tx_bp == -1) tx_bp = 0;
                if (rx_bp == -1) rx_bp = 0;
                if (tx_bp != rx_bp) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair backpressure mismatch at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }

                char cap_expr[256];
                cap_expr[0] = 0;
                if (tx_cap == -1 && rx_cap == -1) {
                    snprintf(cap_expr, sizeof(cap_expr), "0");
                } else if (tx_cap == -2 && rx_cap == -2) {
                    if (strcmp(tx_cap_expr, rx_cap_expr) != 0) {
                        char rel[1024];
                        fprintf(stderr, "CC: error: channel_pair capacity mismatch at %s:%d:%d\n",
                                cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                line, col);
                        free(out);
                        return NULL;
                    }
                    snprintf(cap_expr, sizeof(cap_expr), "%s", tx_cap_expr);
                } else if (tx_cap >= 1 && rx_cap >= 1 && tx_cap == rx_cap) {
                    snprintf(cap_expr, sizeof(cap_expr), "%lld", tx_cap);
                } else {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair capacity mismatch at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }

                size_t elem_len = tx_lbr > tx_ts ? (tx_lbr - tx_ts) : 0;
                char elem_ty[256];
                if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                memcpy(elem_ty, src + tx_ts, elem_len);
                elem_ty[elem_len] = 0;
                while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t'))
                    elem_ty[--elem_len] = 0;

                const char* elem_sz_expr = "0";
                int allow_take = cc__elem_type_implies_take(elem_ty, &elem_sz_expr);

                const char* topo_enum = "CC_CHAN_TOPO_DEFAULT";
                if (tx_has_topo) {
                    if (strcmp(tx_topo, "1:1") == 0) topo_enum = "CC_CHAN_TOPO_1_1";
                    else if (strcmp(tx_topo, "1:N") == 0) topo_enum = "CC_CHAN_TOPO_1_N";
                    else if (strcmp(tx_topo, "N:1") == 0) topo_enum = "CC_CHAN_TOPO_N_1";
                    else if (strcmp(tx_topo, "N:N") == 0) topo_enum = "CC_CHAN_TOPO_N_N";
                }

                const char* bp_enum = "CC_CHAN_MODE_BLOCK";
                if (tx_bp == 1) bp_enum = "CC_CHAN_MODE_DROP_NEW";
                else if (tx_bp == 2) bp_enum = "CC_CHAN_MODE_DROP_OLD";

                char repl[1024];
                if (is_expression) {
                    cc__sb_append_local(&out, &o_len, &o_cap, src + last_emit, assign_start - last_emit);
                    cc__sb_append_local(&out, &o_len, &o_cap, src + assign_start, call_start - assign_start);
                    snprintf(repl, sizeof(repl),
                             "/* channel_pair */ cc_chan_pair_create_returning(%s, %s, %d, %s, %d, %s, &%s, &%s);",
                             cap_expr, bp_enum, allow_take ? 1 : 0, elem_sz_expr,
                             (tx_mode == 1) ? 1 : 0, topo_enum, tx_name, rx_name);
                    cc__sb_append_cstr_local(&out, &o_len, &o_cap, repl);
                } else {
                    cc__sb_append_local(&out, &o_len, &o_cap, src + last_emit, call_start - last_emit);
                    snprintf(repl, sizeof(repl),
                             "/* channel_pair */ do { int __cc_err = cc_chan_pair_create_full(%s, %s, %d, %s, %d, %s, &%s, &%s); "
                             "if (__cc_err) { fprintf(stderr, \"CC: channel_pair failed: %%d\\n\", __cc_err); abort(); } } while(0);",
                             cap_expr, bp_enum, allow_take ? 1 : 0, elem_sz_expr,
                             (tx_mode == 1) ? 1 : 0, topo_enum, tx_name, rx_name);
                    cc__sb_append_cstr_local(&out, &o_len, &o_cap, repl);
                }

                size_t consumed = (size_t)(after - (src + i));
                i += consumed + 1;
                last_emit = i;
                col += (int)consumed + 1;
                continue;
            }
        }

        i++; col++;
    }

    if (last_emit < len) {
        cc__sb_append_local(&out, &o_len, &o_cap, src + last_emit, len - last_emit);
    }
    *out_len = o_len;
    return out;
}

char* cc__rewrite_chan_handle_types_text(const CCVisitorCtx* ctx,
                                         const char* src,
                                         size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    int line = 1, col = 1;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; } i++; col++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '"') in_str = 0; i++; col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; } if (c == '\'') in_chr = 0; i++; col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; col += 2; continue; }
        if (c == '"') { in_str = 1; i++; col++; continue; }
        if (c == '\'') { in_chr = 1; i++; col++; continue; }

        if (c == '[') {
            size_t j = i + 1;
            while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j < n && src[j] == '~') {
                size_t k = j + 1;
                while (k < n && src[k] != ']' && src[k] != '\n') k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: unterminated channel handle type at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                int saw_gt = 0, saw_lt = 0;
                for (size_t t = j; t < k; t++) {
                    if (src[t] == '>') saw_gt = 1;
                    if (src[t] == '<') saw_lt = 1;
                }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel type cannot be both > and < at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel type requires > or < at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                
                size_t ty_start = i;
                while (ty_start > 0) {
                    char p = src[ty_start - 1];
                    if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
                    ty_start--;
                }
                while (ty_start < i && (src[ty_start] == ' ' || src[ty_start] == '\t')) ty_start++;

                if (ty_start >= last_emit) {
                    cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, saw_gt ? "CCChanTx" : "CCChanRx");
                    last_emit = k + 1;
                }
                while (i < k + 1) { if (src[i] == '\n') { line++; col = 1; } else col++; i++; }
                continue;
            }
        }

        i++; col++;
    }

    if (last_emit < n) {
        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    }
    return out;
}
