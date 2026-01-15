#include "visitor.h"
#include "visit_codegen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>

#include "visitor/ufcs.h"
#include "visitor/pass_strip_markers.h"
#include "visitor/pass_await_normalize.h"
#include "visitor/pass_ufcs.h"
#include "visitor/pass_closure_calls.h"
#include "visitor/pass_autoblock.h"
#include "visitor/pass_arena_ast.h"
#include "visitor/pass_nursery_spawn_ast.h"
#include "visitor/pass_closure_literal_ast.h"
#include "visitor/pass_defer_syntax.h"
#include "visitor/pass_with_deadline_syntax.h"
#include "visitor/visitor_fileutil.h"
#include "visitor/text_span.h"
#include "parser/tcc_bridge.h"
#include "preprocess/preprocess.h"
#include "util/path.h"

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

static void cc__sb_append_local(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s || n == 0) return;
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = (*cap ? *cap * 2 : 1024);
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void cc__sb_append_cstr_local(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!s) return;
    cc__sb_append_local(buf, len, cap, s, strlen(s));
}

static int cc__is_ident_char_local2(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

static int cc__is_ident_start_local2(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static const char* cc__skip_ws_local2(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

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

    /* Search backwards for the identifier. Best-effort: this is a text scan over the original source. */
    for (size_t pos = search_before_off; pos-- > 0; ) {
        if (pos + nm_len > len) continue;
        if (memcmp(src + pos, name, nm_len) != 0) continue;
        char pre = (pos == 0) ? 0 : src[pos - 1];
        char post = (pos + nm_len < len) ? src[pos + nm_len] : 0;
        if (pre && cc__is_ident_char_local2(pre)) continue;
        if (post && cc__is_ident_char_local2(post)) continue;

        /* Find preceding '[~' ... ']' before the name (same statement chunk). */
        size_t scan = pos;
        size_t lbr = (size_t)-1;
        size_t rbr = (size_t)-1;
        for (size_t j = scan; j-- > 0; ) {
            char c = src[j];
            if (c == ';' || c == '{' || c == '}' || c == '\n') break;
            if (c == ']') { rbr = j; continue; }
            if (c == '[') {
                /* check for "~" soon after '[' */
                size_t k = j + 1;
                while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k < len && src[k] == '~' && rbr != (size_t)-1 && rbr > j) {
                    lbr = j;
                    break;
                }
            }
        }
        if (lbr == (size_t)-1 || rbr == (size_t)-1) continue;

        /* Type start: walk back to delimiter. */
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

static int cc__parse_chan_bracket_spec(const CCVisitorCtx* ctx,
                                      const char* src,
                                      size_t len,
                                      size_t lbr,
                                      size_t rbr,
                                      int* out_is_tx,
                                      int* out_is_rx,
                                      long long* out_cap_int,
                                      char* out_cap_expr, /* optional; caller-provided buffer for macro/expr capacity */
                                      size_t out_cap_expr_cap,
                                      int* out_mode, /* -1 unspecified, 0 async, 1 sync */
                                      char* out_topology, /* optional; caller-provided buffer */
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
    if (out_mode) *out_mode = -1;
    if (out_topology && out_topology_cap) out_topology[0] = 0;
    if (out_has_topology) *out_has_topology = 0;
    if (out_unknown_token) *out_unknown_token = 0;
    if (out_allow_take) *out_allow_take = 0;
    if (out_elem_size_expr) *out_elem_size_expr = "0";

    /* Direction */
    int saw_gt = 0, saw_lt = 0;
    for (size_t i = lbr; i <= rbr && i < len; i++) {
        if (src[i] == '>') saw_gt = 1;
        if (src[i] == '<') saw_lt = 1;
    }
    if (out_is_tx) *out_is_tx = saw_gt;
    if (out_is_rx) *out_is_rx = saw_lt;

    /* Tokens inside [~ ... ]: capacity? + mode? + topology? + direction */
    size_t t = lbr;
    while (t < rbr && src[t] != '~') t++;
    if (t < rbr && src[t] == '~') t++;
    while (t < rbr) {
        while (t < rbr && (src[t] == ' ' || src[t] == '\t')) t++;
        if (t >= rbr) break;
        char c = src[t];
        if (c == '>' || c == '<' || c == ',' ) { t++; continue; }
        /* Topology token like 1:1 / N:N / 1:N / N:1 - check BEFORE numeric to avoid consuming '1' from '1:1' as capacity */
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
            /* validate: X:Y where X,Y in {1,N,n} */
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
        /* Numeric: capacity */
        if (c >= '0' && c <= '9') {
            long long cap = 0;
            while (t < rbr && (src[t] >= '0' && src[t] <= '9')) { cap = cap * 10 + (src[t] - '0'); t++; }
            /* Only first integer token is capacity; subsequent numeric tokens are currently rejected. */
            if (out_cap_int && *out_cap_int == -1) {
                *out_cap_int = cap;
            } else {
                if (out_unknown_token) *out_unknown_token = 1;
            }
            continue;
        }
        /* Alpha word: sync/async keywords OR identifier capacity (macro) */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            char word[128];
            size_t wn = 0;
            while (t < rbr && wn + 1 < sizeof(word)) {
                char wc = src[t];
                if (!((wc >= 'A' && wc <= 'Z') || (wc >= 'a' && wc <= 'z') || (wc >= '0' && wc <= '9') || wc == '_')) break;
                word[wn++] = wc;
                t++;
            }
            word[wn] = 0;
            if (strcmp(word, "sync") == 0) {
                if (out_mode) *out_mode = 1;
            } else if (strcmp(word, "async") == 0) {
                if (out_mode) *out_mode = 0;
            } else {
                /* Identifier: treat as capacity expression (macro) if we haven't seen one yet */
                if (out_cap_int && *out_cap_int == -1 && 
                    out_cap_expr && out_cap_expr_cap > 0 && out_cap_expr[0] == 0) {
                    if (wn < out_cap_expr_cap) {
                        memcpy(out_cap_expr, word, wn);
                        out_cap_expr[wn] = 0;
                        *out_cap_int = -2; /* sentinel: capacity is in out_cap_expr */
                    } else {
                        if (out_unknown_token) *out_unknown_token = 1;
                    }
                } else {
                    if (out_unknown_token) *out_unknown_token = 1;
                }
            }
            continue;
        }
        /* Anything else is unknown for now. */
        if (out_unknown_token) *out_unknown_token = 1;
        t++;
    }

    (void)ctx;
    return 1;
}

/* Buffer for dynamic sizeof expr (used by cc__elem_type_implies_take). */
static char cc__sizeof_expr_buf[256];

static int cc__elem_type_implies_take(const char* elem_ty, const char** out_elem_size_expr) {
    if (!elem_ty || !*elem_ty) return 0;
    /* Slice element: enable take and set elem size to CCSlice */
    if (strstr(elem_ty, "[:") || strstr(elem_ty, "CCSlice")) {
        if (out_elem_size_expr) *out_elem_size_expr = "sizeof(CCSlice)";
        return 1;
    }
    /* Pointer element: enable take and set elem size to void* */
    if (strchr(elem_ty, '*')) {
        if (out_elem_size_expr) *out_elem_size_expr = "sizeof(void*)";
        return 1;
    }
    /* For other types (primitives, structs), generate sizeof(elem_ty). */
    if (out_elem_size_expr) {
        snprintf(cc__sizeof_expr_buf, sizeof(cc__sizeof_expr_buf), "sizeof(%s)", elem_ty);
        *out_elem_size_expr = cc__sizeof_expr_buf;
    }
    return 0;
}

static char* cc__rewrite_channel_pair_calls_text(const CCVisitorCtx* ctx, const char* src, size_t len, size_t* out_len) {
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
                p++; /* consume '(' */
                p = cc__skip_ws_local2(p);
                if (*p != '&') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair expects `&tx, &rx` at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }
                p++;
                p = cc__skip_ws_local2(p);
                if (!cc__is_ident_start_local2(*p)) return NULL;
                const char* tx_s = p;
                p++;
                while (cc__is_ident_char_local2(*p)) p++;
                size_t tx_n = (size_t)(p - tx_s);
                p = cc__skip_ws_local2(p);
                if (*p != ',') return NULL;
                p++;
                p = cc__skip_ws_local2(p);
                if (*p != '&') return NULL;
                p++;
                p = cc__skip_ws_local2(p);
                if (!cc__is_ident_start_local2(*p)) return NULL;
                const char* rx_s = p;
                p++;
                while (cc__is_ident_char_local2(*p)) p++;
                size_t rx_n = (size_t)(p - rx_s);
                p = cc__skip_ws_local2(p);
                if (*p != ')') return NULL;
                p++;
                const char* after = cc__skip_ws_local2(p);
                if (*after != ';') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair must be used as a statement (end with ';') at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }

                char tx_name[128], rx_name[128];
                if (tx_n >= sizeof(tx_name) || rx_n >= sizeof(rx_name)) return NULL;
                memcpy(tx_name, tx_s, tx_n); tx_name[tx_n] = 0;
                memcpy(rx_name, rx_s, rx_n); rx_name[rx_n] = 0;

                size_t tx_lbr=0, tx_rbr=0, tx_ts=0;
                size_t rx_lbr=0, rx_rbr=0, rx_ts=0;
                if (!cc__find_chan_decl_before(src, len, call_start, tx_name, &tx_lbr, &tx_rbr, &tx_ts) ||
                    !cc__find_chan_decl_before(src, len, call_start, rx_name, &rx_lbr, &rx_rbr, &rx_ts)) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair could not find matching channel handle declarations for '%s'/'%s' at %s:%d:%d\n",
                            tx_name, rx_name,
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }

                int tx_is_tx=0, tx_is_rx=0, rx_is_tx=0, rx_is_rx=0;
                long long tx_cap=-1, rx_cap=-1;
                int tx_mode=-1, rx_mode=-1;
                char tx_topo[8]; char rx_topo[8];
                int tx_has_topo=0, rx_has_topo=0;
                int tx_unknown=0, rx_unknown=0;
                int dummy_allow=0;
                const char* dummy_sz="0";
                (void)cc__parse_chan_bracket_spec(ctx, src, len, tx_lbr, tx_rbr,
                                                  &tx_is_tx, &tx_is_rx, &tx_cap,
                                                  &tx_mode, tx_topo, sizeof(tx_topo), &tx_has_topo, &tx_unknown,
                                                  &dummy_allow, &dummy_sz);
                (void)cc__parse_chan_bracket_spec(ctx, src, len, rx_lbr, rx_rbr,
                                                  &rx_is_tx, &rx_is_rx, &rx_cap,
                                                  &rx_mode, rx_topo, sizeof(rx_topo), &rx_has_topo, &rx_unknown,
                                                  &dummy_allow, &dummy_sz);

                if (!tx_is_tx || tx_is_rx || !rx_is_rx || rx_is_tx) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair requires a send handle (>) then a recv handle (<) at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }

                /* Mode + topology: enforce tx/rx agreement if specified. */
                if (tx_unknown || rx_unknown) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair could not parse channel handle spec tokens (expected: optional capacity, optional 'sync'/'async', optional topology '1:1'/'1:N'/'N:1'/'N:N') at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }
                if (tx_mode == -1) tx_mode = 0; /* default: async */
                if (rx_mode == -1) rx_mode = 0;
                if (tx_mode != rx_mode) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair requires matching mode on tx/rx (sync vs async) at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }
                if (tx_has_topo != rx_has_topo || (tx_has_topo && strcmp(tx_topo, rx_topo) != 0)) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair requires matching topology token on tx/rx (e.g. 1:1, 1:N, N:1, N:N) at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }
                long long cap_to_use = -1;
                if (tx_cap == -1 && rx_cap == -1) {
                    /* Unbuffered rendezvous channel (capacity omitted). */
                    cap_to_use = 0;
                } else if (tx_cap >= 1 && rx_cap >= 1 && tx_cap == rx_cap) {
                    cap_to_use = tx_cap;
                } else {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel_pair requires matching capacity on tx/rx (or omit both for unbuffered) at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    return NULL;
                }

                /* Element type: slice substring before tx_lbr */
                size_t elem_len = tx_lbr > tx_ts ? (tx_lbr - tx_ts) : 0;
                char elem_ty[256];
                if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                memcpy(elem_ty, src + tx_ts, elem_len);
                elem_ty[elem_len] = 0;
                /* Trim trailing whitespace */
                while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t')) elem_ty[--elem_len] = 0;

                const char* elem_sz_expr = "0";
                int allow_take = cc__elem_type_implies_take(elem_ty, &elem_sz_expr);

                /* Map topology string to enum value. */
                const char* topo_enum = "CC_CHAN_TOPO_DEFAULT";
                if (tx_has_topo) {
                    if (strcmp(tx_topo, "1:1") == 0) topo_enum = "CC_CHAN_TOPO_1_1";
                    else if (strcmp(tx_topo, "1:N") == 0) topo_enum = "CC_CHAN_TOPO_1_N";
                    else if (strcmp(tx_topo, "N:1") == 0) topo_enum = "CC_CHAN_TOPO_N_1";
                    else if (strcmp(tx_topo, "N:N") == 0) topo_enum = "CC_CHAN_TOPO_N_N";
                }

                /* Emit up to call_start, then replace call statement. */
                cc__sb_append_local(&out, &o_len, &o_cap, src + last_emit, call_start - last_emit);
                char repl[1024];
                snprintf(repl, sizeof(repl),
                         "/* channel_pair: mode=%s topo=%s */ do { int __cc_err = cc_chan_pair_create_full(%lld, CC_CHAN_MODE_BLOCK, %d, %s, %d, %s, &%s, &%s); "
                         "if (__cc_err) { fprintf(stderr, \"CC: channel_pair failed: %%d\\n\", __cc_err); abort(); } } while(0);",
                         (tx_mode == 1) ? "sync" : "async",
                         tx_has_topo ? tx_topo : "<default>",
                         cap_to_use,
                         allow_take ? 1 : 0,
                         elem_sz_expr,
                         (tx_mode == 1) ? 1 : 0,  /* is_sync */
                         topo_enum,
                         tx_name, rx_name);
                cc__sb_append_cstr_local(&out, &o_len, &o_cap, repl);

                /* Advance i to after the ';' */
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

/* Text-based: rewrite channel handle types `T[~ ... >]` / `T[~ ... <]` into `CCChanTx` / `CCChanRx`.
   This is a surface-syntax lowering step: it must run before the generated C is compiled. */
static char* cc__rewrite_chan_handle_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;

    int line = 1;
    int col = 1;

    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        if (c == '\n') { line++; col = 1; }

        if (in_line_comment) {
            if (c == '\n') in_line_comment = 0;
            i++; col++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; col += 2; continue; }
            i++; col++;
            continue;
        }
                if (in_str) {
            if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
            if (c == '"') in_str = 0;
            i++; col++;
                    continue;
                }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i += 2; col += 2; continue; }
            if (c == '\'') in_chr = 0;
            i++; col++;
            continue;
        }
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
                    fprintf(stderr, "CC: error: unterminated channel handle type (missing ']') at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                int saw_gt = 0, saw_lt = 0;
                for (size_t t = j; t < k; t++) { if (src[t] == '>') saw_gt = 1; if (src[t] == '<') saw_lt = 1; }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel handle type cannot be both send ('>') and recv ('<') at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    fprintf(stderr, "CC: error: channel handle type requires direction: use `T[~ ... >]` or `T[~ ... <]` at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                /* back to stmt delimiter */
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
                /* advance scan to k+1 */
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

static int cc__is_ident_char_local(char c) {
    return (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
}

static size_t cc__strip_leading_cv_qual(const char* s, size_t ty_start, char* out_qual, size_t out_cap) {
    if (!s || !out_qual || out_cap == 0) return ty_start;
    out_qual[0] = 0;
    size_t p = ty_start;
    while (s[p] == ' ' || s[p] == '\t') p++;
    for (;;) {
        int matched = 0;
        if (strncmp(s + p, "const", 5) == 0 && !cc__is_ident_char_local(s[p + 5])) {
            strncat(out_qual, "const ", out_cap - strlen(out_qual) - 1);
            p += 5;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        } else if (strncmp(s + p, "volatile", 8) == 0 && !cc__is_ident_char_local(s[p + 8])) {
            strncat(out_qual, "volatile ", out_cap - strlen(out_qual) - 1);
            p += 8;
            while (s[p] == ' ' || s[p] == '\t') p++;
            matched = 1;
        }
        if (!matched) break;
    }
    return p;
}

static char* cc__rewrite_slice_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
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
            if (j < n && src[j] == ':') {
                size_t k = j + 1;
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                int is_unique = 0;
                if (k < n && src[k] == '!') { is_unique = 1; k++; }
                while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    fprintf(stderr, "CC: error: unterminated slice type (missing ']') at %s:%d:%d\n",
                            cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col);
                    free(out);
                    return NULL;
                }
                /* Find start of type token sequence and preserve leading cv qualifiers */
                size_t ty_start = i;
                while (ty_start > 0) {
                    char p = src[ty_start - 1];
                    if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
                    ty_start--;
                }
                while (ty_start < i && (src[ty_start] == ' ' || src[ty_start] == '\t')) ty_start++;

                if (ty_start >= last_emit) {
                    char quals[64];
                    size_t after_qual = cc__strip_leading_cv_qual(src, ty_start, quals, sizeof(quals));
                    (void)after_qual;
                    cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, quals);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, is_unique ? "CCSliceUnique" : "CCSlice");
                    last_emit = k + 1;
                }
                while (i < k + 1) { if (src[i] == '\n') { line++; col = 1; } else col++; i++; }
                    continue;
                }
        }

        i++; col++;
    }

    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Mangle a type name for use in CCOptional_T or CCResult_T_E. */
static void cc__mangle_type_name(const char* src, size_t len, char* out, size_t out_sz) {
    if (!src || len == 0 || !out || out_sz == 0) { if (out && out_sz > 0) out[0] = 0; return; }
    
    /* Skip leading whitespace */
    while (len > 0 && (*src == ' ' || *src == '\t')) { src++; len--; }
    /* Skip trailing whitespace */
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    
    size_t j = 0;
    for (size_t i = 0; i < len && j < out_sz - 1; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '*') {
            if (j + 3 < out_sz - 1) { out[j++] = 'p'; out[j++] = 't'; out[j++] = 'r'; }
        } else if (c == '[' || c == ']') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
        } else if (c == '<' || c == '>' || c == ',') {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
                                } else {
            out[j++] = c;
        }
    }
    /* Remove trailing underscore */
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = 0;
}

/* Scan back from position `from` to find the start of a type token (delimited by ; { } , ( ) newline). */
static size_t cc__scan_back_to_type_start(const char* s, size_t from) {
    size_t i = from;
    while (i > 0) {
        char p = s[i - 1];
        if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
        i--;
    }
    while (s[i] && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Rewrite optional types: T? -> CCOptional_T */
static char* cc__rewrite_optional_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect T? pattern: identifier followed by '?' (not '?:' ternary or '??') */
        if (c == '?' && c2 != ':' && c2 != '?') {
            if (i > 0) {
                char prev = src[i - 1];
                /* Valid type-ending chars: identifier char, ')', ']', '>' */
                if (cc__is_ident_char_local(prev) || prev == ')' || prev == ']' || prev == '>') {
                    size_t ty_start = cc__scan_back_to_type_start(src, i);
                    if (ty_start < i) {
                        size_t ty_len = i - ty_start;
                        char mangled[256];
                        cc__mangle_type_name(src + ty_start, ty_len, mangled, sizeof(mangled));
                        
                        if (mangled[0]) {
                            cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, "CCOptional_");
                            cc__sb_append_cstr_local(&out, &out_len, &out_cap, mangled);
                            last_emit = i + 1; /* skip past '?' */
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Collection of result type pairs for CC_DECL_RESULT_SPEC emission */
typedef struct {
    char ok_type[128];
    char err_type[128];
    char mangled_ok[128];
    char mangled_err[128];
} CCCodegenResultTypePair;

static CCCodegenResultTypePair cc__cg_result_types[64];
static size_t cc__cg_result_type_count = 0;

static void cc__cg_add_result_type(const char* ok, size_t ok_len, const char* err, size_t err_len,
                                    const char* mangled_ok, const char* mangled_err) {
    /* Skip built-in result types (already declared in cc_result.cch) */
    if (strcmp(mangled_err, "CCError") == 0) return;
    
    /* Check for duplicates */
    for (size_t i = 0; i < cc__cg_result_type_count; i++) {
        if (strcmp(cc__cg_result_types[i].mangled_ok, mangled_ok) == 0 &&
            strcmp(cc__cg_result_types[i].mangled_err, mangled_err) == 0) {
            return; /* Already have this type */
        }
    }
    if (cc__cg_result_type_count >= sizeof(cc__cg_result_types)/sizeof(cc__cg_result_types[0])) return;
    CCCodegenResultTypePair* p = &cc__cg_result_types[cc__cg_result_type_count++];
    if (ok_len >= sizeof(p->ok_type)) ok_len = sizeof(p->ok_type) - 1;
    if (err_len >= sizeof(p->err_type)) err_len = sizeof(p->err_type) - 1;
    memcpy(p->ok_type, ok, ok_len);
    p->ok_type[ok_len] = '\0';
    memcpy(p->err_type, err, err_len);
    p->err_type[err_len] = '\0';
    strncpy(p->mangled_ok, mangled_ok, sizeof(p->mangled_ok) - 1);
    p->mangled_ok[sizeof(p->mangled_ok) - 1] = '\0';
    strncpy(p->mangled_err, mangled_err, sizeof(p->mangled_err) - 1);
    p->mangled_err[sizeof(p->mangled_err) - 1] = '\0';
}

static void cc__cg_emit_result_type_decls(FILE* out) {
    for (size_t i = 0; i < cc__cg_result_type_count; i++) {
        CCCodegenResultTypePair* p = &cc__cg_result_types[i];
        /* Emit CC_DECL_RESULT_SPEC(CCResult_T_E, T, E) */
        fprintf(out, "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                p->mangled_ok, p->mangled_err, p->ok_type, p->err_type);
    }
}

/* Scan for already-lowered CCResult_T_E patterns and collect type pairs.
   This handles the case where the preprocessor already rewrote T!E -> CCResult_T_E */
static void cc__scan_for_existing_result_types(const char* src, size_t n) {
    /* Reset collection */
    cc__cg_result_type_count = 0;
    
    const char* prefix = "CCResult_";
    size_t prefix_len = strlen(prefix);
    
    size_t i = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for CCResult_ prefix */
        if (i + prefix_len < n && strncmp(src + i, prefix, prefix_len) == 0) {
            /* Make sure this isn't part of a longer identifier (check char before) */
            if (i > 0 && cc__is_ident_char_local(src[i-1])) {
                i++;
                continue;
            }
            
            /* Parse: CCResult_OkType_ErrType */
            size_t name_start = i;
            size_t j = i + prefix_len;
            
            /* Find the ok type (everything until next '_') */
            size_t ok_start = j;
            while (j < n && src[j] != '_' && cc__is_ident_char_local(src[j])) j++;
            if (j >= n || src[j] != '_') { i++; continue; }
            size_t ok_end = j;
            
            j++; /* skip '_' */
            
            /* Find the error type (rest of identifier) */
            size_t err_start = j;
            while (j < n && cc__is_ident_char_local(src[j])) j++;
            size_t err_end = j;
            
            if (ok_end > ok_start && err_end > err_start) {
                /* Extract type names */
                char ok_type[128];
                char err_type[128];
                size_t ok_len = ok_end - ok_start;
                size_t err_len = err_end - err_start;
                
                if (ok_len < sizeof(ok_type) && err_len < sizeof(err_type)) {
                    memcpy(ok_type, src + ok_start, ok_len);
                    ok_type[ok_len] = '\0';
                    memcpy(err_type, src + err_start, err_len);
                    err_type[err_len] = '\0';
                    
                    /* Skip built-in result types (those are already declared in cc_result.cch) */
                    if (strcmp(err_type, "CCError") != 0) {
                        cc__cg_add_result_type(ok_type, ok_len, err_type, err_len, ok_type, err_type);
                    }
                }
            }
            
            i = j;
            continue;
        }
        
        i++;
    }
}

/* Rewrite result types: T!E -> CCResult_T_E, also collect pairs for declaration emission */
static char* cc__rewrite_result_types_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    /* First, scan for any existing CCResult_T_E patterns (preprocessor may have already rewritten) */
    cc__scan_for_existing_result_types(src, n);

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect T!E pattern: type followed by '!' followed by error type (not '!=') */
        if (c == '!' && c2 != '=') {
            if (i > 0) {
                char prev = src[i - 1];
                /* Valid type-ending chars: identifier char, ')', ']', '>' */
                if (cc__is_ident_char_local(prev) || prev == ')' || prev == ']' || prev == '>') {
                    /* Check what follows the '!' - must be an identifier (error type) */
                    size_t j = i + 1;
                    while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
                    if (j < n && cc__is_ident_start_local2(src[j])) {
                        size_t err_start = j;
                        while (j < n && cc__is_ident_char_local(src[j])) j++;
                        size_t err_end = j;
                        
                        size_t ty_start = cc__scan_back_to_type_start(src, i);
                        if (ty_start < i) {
                            size_t ty_len = i - ty_start;
                            size_t err_len = err_end - err_start;
                            
                            char mangled_ok[256];
                            char mangled_err[256];
                            cc__mangle_type_name(src + ty_start, ty_len, mangled_ok, sizeof(mangled_ok));
                            cc__mangle_type_name(src + err_start, err_len, mangled_err, sizeof(mangled_err));
                            
                            if (mangled_ok[0] && mangled_err[0]) {
                                /* Collect this result type pair for declaration */
                                cc__cg_add_result_type(src + ty_start, ty_len, 
                                                       src + err_start, err_len,
                                                       mangled_ok, mangled_err);
                                
                                cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, "CCResult_");
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, mangled_ok);
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, "_");
                                cc__sb_append_cstr_local(&out, &out_len, &out_cap, mangled_err);
                                last_emit = err_end;
                                i = err_end;
                                continue;
                            }
                        }
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite try expressions: try expr -> cc_try(expr) */
static char* cc__rewrite_try_exprs_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    size_t i = 0;
    size_t last_emit = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Detect 'try' keyword followed by space and not followed by '{' (try-block form, handled elsewhere) */
        if (c == 't' && i + 3 < n && src[i+1] == 'r' && src[i+2] == 'y') {
            /* Check word boundary before */
            int word_start = (i == 0) || !cc__is_ident_char_local(src[i-1]);
            if (word_start) {
                size_t after_try = i + 3;
                /* Skip whitespace */
                while (after_try < n && (src[after_try] == ' ' || src[after_try] == '\t')) after_try++;
                
                /* Check it's not try { block } form */
                if (after_try < n && src[after_try] != '{' && cc__is_ident_char_local(src[after_try]) == 0 && src[after_try] != '(') {
                    /* Not a try-block or try identifier, skip */
                } else if (after_try < n && src[after_try] == '{') {
                    /* try { ... } block form - skip, not handled here */
                } else if (after_try < n && (cc__is_ident_start_local2(src[after_try]) || src[after_try] == '(')) {
                    /* 'try expr' form - need to find end of expression */
                    size_t expr_start = after_try;
                    size_t expr_end = expr_start;
                    
                    /* Scan expression with balanced parens/braces */
                    int paren = 0, brace = 0, bracket = 0;
                    int in_s = 0, in_c = 0;
                    while (expr_end < n) {
                        char ec = src[expr_end];
                        char ec2 = (expr_end + 1 < n) ? src[expr_end + 1] : 0;
                        
                        if (in_s) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '"') in_s = 0; expr_end++; continue; }
                        if (in_c) { if (ec == '\\' && expr_end + 1 < n) { expr_end += 2; continue; } if (ec == '\'') in_c = 0; expr_end++; continue; }
                        if (ec == '"') { in_s = 1; expr_end++; continue; }
                        if (ec == '\'') { in_c = 1; expr_end++; continue; }
                        
                        if (ec == '(' ) { paren++; expr_end++; continue; }
                        if (ec == ')' ) { if (paren > 0) { paren--; expr_end++; continue; } else break; }
                        if (ec == '{' ) { brace++; expr_end++; continue; }
                        if (ec == '}' ) { if (brace > 0) { brace--; expr_end++; continue; } else break; }
                        if (ec == '[' ) { bracket++; expr_end++; continue; }
                        if (ec == ']' ) { if (bracket > 0) { bracket--; expr_end++; continue; } else break; }
                        
                        /* End expression at ';', ',', or unbalanced ')' */
                        if (paren == 0 && brace == 0 && bracket == 0) {
                            if (ec == ';' || ec == ',') break;
                        }
                        
                        expr_end++;
                    }
                    
                    /* Only rewrite if we found a valid expression */
                    if (expr_end > expr_start) {
                        /* Emit: everything up to 'try', then 'cc_try(', then expr, then ')' */
                        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, i - last_emit);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_try(");
                        cc__sb_append_local(&out, &out_len, &out_cap, src + expr_start, expr_end - expr_start);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, ")");
                        last_emit = expr_end;
                        i = expr_end;
                        continue;
                    }
                }
            }
        }
        
        i++;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* Rewrite *opt -> cc_unwrap_opt(opt) for variables declared with CCOptional_* type.
   Two-pass approach:
   1. Scan for CCOptional_<T> <varname> declarations
   2. Rewrite *varname to cc_unwrap_opt(varname)
*/
static char* cc__rewrite_optional_unwrap_text(const CCVisitorCtx* ctx, const char* src, size_t n) {
    (void)ctx;
    if (!src || n == 0) return NULL;
    
    /* Pass 1: Collect optional variable names */
    #define MAX_OPT_VARS_LOCAL 256
    char* opt_vars[MAX_OPT_VARS_LOCAL];
    int opt_var_count = 0;
    
    size_t i = 0;
    int in_line_comment = 0, in_block_comment = 0, in_str = 0, in_chr = 0;
    
    while (i < n && opt_var_count < MAX_OPT_VARS_LOCAL) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for CCOptional_ type declarations */
        if (c == 'C' && i + 10 < n && strncmp(src + i, "CCOptional_", 11) == 0) {
            /* Skip to end of type name */
            i += 11;
            while (i < n && cc__is_ident_char_local(src[i])) i++;
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) i++;
            /* Check for variable name (not function) */
            if (i < n && cc__is_ident_start_local2(src[i])) {
                size_t var_start = i;
                while (i < n && cc__is_ident_char_local(src[i])) i++;
                size_t var_len = i - var_start;
                /* Skip whitespace */
                while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                /* If followed by '=' or ';', it's a variable declaration */
                if (i < n && (src[i] == '=' || src[i] == ';' || src[i] == ',')) {
                    char* varname = (char*)malloc(var_len + 1);
                    if (varname) {
                        memcpy(varname, src + var_start, var_len);
                        varname[var_len] = 0;
                        opt_vars[opt_var_count++] = varname;
                    }
                }
            }
            continue;
        }
        
        i++;
    }
    
    /* If no optional vars found, nothing to rewrite */
    if (opt_var_count == 0) return NULL;
    
    /* Pass 2: Rewrite *varname to cc_unwrap_opt(varname) */
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;
    
    i = 0;
    size_t last_emit = 0;
    in_line_comment = 0; in_block_comment = 0; in_str = 0; in_chr = 0;
    
    while (i < n) {
        char c = src[i];
        char c2 = (i + 1 < n) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; i++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i += 2; continue; } i++; continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '"') in_str = 0; i++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i += 2; continue; } if (c == '\'') in_chr = 0; i++; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i += 2; continue; }
        if (c == '"') { in_str = 1; i++; continue; }
        if (c == '\'') { in_chr = 1; i++; continue; }
        
        /* Look for * followed by an optional variable name */
        if (c == '*') {
            size_t star_pos = i;
            i++;
            /* Skip whitespace */
            while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
            /* Check for identifier */
            if (i < n && cc__is_ident_start_local2(src[i])) {
                size_t var_start = i;
                while (i < n && cc__is_ident_char_local(src[i])) i++;
                size_t var_len = i - var_start;
                
                /* Check if this identifier is in our opt_vars list */
                int is_opt = 0;
                for (int j = 0; j < opt_var_count; j++) {
                    if (strlen(opt_vars[j]) == var_len && strncmp(opt_vars[j], src + var_start, var_len) == 0) {
                        is_opt = 1;
                        break;
                    }
                }
                
                if (is_opt) {
                    /* Rewrite *varname to cc_unwrap_opt(varname) */
                    cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, star_pos - last_emit);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, "cc_unwrap_opt(");
                    cc__sb_append_local(&out, &out_len, &out_cap, src + var_start, var_len);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, ")");
                    last_emit = i;
                }
            }
            continue;
        }
        
        i++;
    }
    
    /* Free opt_vars */
    for (int j = 0; j < opt_var_count; j++) {
        free(opt_vars[j]);
    }
    
    if (last_emit == 0) {
        /* No rewrites done */
        return NULL;
    }
    
    if (last_emit < n) cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, n - last_emit);
    return out;
}

/* AST-driven async lowering (implemented in `cc/src/visitor/async_ast.c`). */
int cc_async_rewrite_state_machine_ast(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len);

/* Legacy closure scan/lowering helpers removed - now handled by AST-span passes. */

/* Strip CC decl markers so output is valid C. This is used regardless of whether
   TCC extensions are available, because the output C is compiled by the host compiler. */
/* cc__read_entire_file / cc__write_temp_c_file are implemented in visitor_fileutil.c */

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

/* Return pointer to a stable suffix (last 2 path components) inside `path`.
   If `path` has fewer than 2 components, returns basename. */
static const char* cc__path_suffix2(const char* path) {
    if (!path) return NULL;
    const char* end = path + strlen(path);
    int seps = 0;
    for (const char* p = end; p > path; ) {
        p--;
        if (*p == '/' || *p == '\\') {
            seps++;
            if (seps == 2) return p + 1;
        }
    }
    return cc__basename(path);
}

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;

    const char* a_base = cc__basename(a);
    const char* b_base = cc__basename(b);
    if (!a_base || !b_base || strcmp(a_base, b_base) != 0) return 0;

    /* Prefer 2-component suffix match (handles duplicate basenames across dirs). */
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;

    /* Fallback: basename-only match. */
    return 1;
}

/* UFCS span rewrite lives in pass_ufcs.c now (cc__rewrite_ufcs_spans_with_nodes). */


int cc_visit_codegen(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
    if (!ctx || !ctx->symbols || !output_path) return EINVAL;
    const char* src_path = ctx->input_path ? ctx->input_path : "<cc_input>";
    FILE* out = fopen(output_path, "w");
    if (!out) return errno ? errno : -1;

    /* Optional: dump TCC stub nodes for debugging wiring. */
    if (root && root->nodes && root->node_count > 0) {
        const char* dump = getenv("CC_DUMP_TCC_STUB_AST");
        if (dump && dump[0] == '1') {
            typedef struct {
                int kind;
                int parent;
                const char* file;
                int line_start;
                int line_end;
                int col_start;
                int col_end;
                int aux1;
                int aux2;
                const char* aux_s1;
                const char* aux_s2;
            } CCASTStubNodeView;
            const CCASTStubNodeView* n = (const CCASTStubNodeView*)root->nodes;
            fprintf(stderr, "[cc] stub ast nodes: %d\n", root->node_count);
            int max_dump = root->node_count;
            if (max_dump > 4000) max_dump = 4000;
            for (int i = 0; i < max_dump; i++) {
                fprintf(stderr,
                        "  [%d] kind=%d parent=%d file=%s lines=%d..%d cols=%d..%d aux1=%d aux2=%d aux_s1=%s aux_s2=%s\n",
                        i,
                        n[i].kind,
                        n[i].parent,
                        n[i].file ? n[i].file : "<null>",
                        n[i].line_start,
                        n[i].line_end,
                        n[i].col_start,
                        n[i].col_end,
                        n[i].aux1,
                        n[i].aux2,
                        n[i].aux_s1 ? n[i].aux_s1 : "<null>",
                        n[i].aux_s2 ? n[i].aux_s2 : "<null>");
            }
            if (max_dump != root->node_count)
                fprintf(stderr, "  ... truncated (%d total)\n", root->node_count);
        }
    }

    /* For final codegen we read the original source and lower UFCS/@arena here.
       The preprocessor's temp file exists only to make TCC parsing succeed. */
    /* Read original source once; we may rewrite UFCS spans before @arena lowering. */
    char* src_all = NULL;
    size_t src_len = 0;
    if (ctx->input_path) {
#ifdef CC_TCC_EXT_AVAILABLE
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#else
        cc__read_entire_file(ctx->input_path, &src_all, &src_len);
#endif
    }

    char* src_ufcs = src_all;
    size_t src_ufcs_len = src_len;

    /* Rewrite `with_deadline(expr) { ... }` (not valid C) into CCDeadline scope syntax
       using @defer, so the rest of the pipeline sees valid parseable text. */
    if (src_all && src_len) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_with_deadline_syntax(src_all, src_len, &rewritten, &rewritten_len) == 0 && rewritten) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Produced by the closure-literal AST pass (emitted into the output TU). */
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;

#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Rewrite closure calls anywhere (including nested + multiline) using stub CALL nodes. */
#ifdef CC_TCC_EXT_AVAILABLE
    char* src_calls = NULL;
    size_t src_calls_len = 0;
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &src_calls, &src_calls_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = src_calls;
            src_ufcs_len = src_calls_len;
        }
    }
#endif

    /* Auto-blocking (first cut): inside @async functions, wrap statement-form calls to known
       non-@async/non-@noblock functions in cc_run_blocking_closure0(() => { ... }). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Normalize `await <expr>` used inside larger expressions into temp hoists so the
       text-based async state machine can lower it (AST-driven span rewrite). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            const char* p = strstr(src_ufcs, needle);
            if (!p) p = strstr(src_ufcs, "@async");
            if (p) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t off = (size_t)(p - src_ufcs);
                size_t take = 800;
                if (off + take > src_ufcs_len) take = src_ufcs_len - off;
                fwrite(p, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* Reparse the current TU source to get an up-to-date stub-AST for statement-level lowering
       (@arena/@nursery/spawn). These rewrites run before marker stripping to keep spans stable. */
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp_path[128];
        int pp_err = tmp_path ? cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path)) : EINVAL;
        const char* use_path = (pp_err == 0) ? pp_path : tmp_path;
        CCASTRoot* root3 = use_path ? cc_tcc_bridge_parse_to_ast(use_path, ctx->input_path, ctx->symbols) : NULL;
        if (pp_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp_path);
        if (tmp_path) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
            free(tmp_path);
        }
        if (!root3) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }

        /* Autoblock was already run on the initial AST (before reparse), so UFCS-rewritten `chan_*`
           calls should have been processed there. No need to re-run autoblock here. */

        /* Lower `channel_pair(&tx, &rx);` BEFORE channel type rewrite (it needs `[~]` patterns). */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                cc_tcc_bridge_free_ast(root3);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }

        /* Rewrite channel handle types BEFORE closure pass so captured CCChanTx/CCChanRx variables
           are correctly recognized. This rewrites `int[~4 >]` -> `CCChanTx`, etc. */
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew;
                src_ufcs_len = strlen(src_ufcs);
            }
        }

        /* 1) closure literals -> __cc_closure_make_N(...) + generated closure defs */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            char* protos = NULL;
            size_t protos_len = 0;
            char* defs = NULL;
            size_t defs_len = 0;
            int r = cc__rewrite_closure_literals_with_nodes(root3, ctx, src_ufcs, src_ufcs_len,
                                                           &rewritten, &rewritten_len,
                                                           &protos, &protos_len,
                                                           &defs, &defs_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root3);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            } else {
                free(rewritten);
            }
            if (protos) { free(closure_protos); closure_protos = protos; closure_protos_len = protos_len; }
            if (defs) { free(closure_defs); closure_defs = defs; closure_defs_len = defs_len; }
        }
        cc_tcc_bridge_free_ast(root3);

        /* Reparse after closure rewrite so spawn/nursery/arena spans are correct. */
        char* tmp2 = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp2[128];
        int pp2_err = tmp2 ? cc_preprocess_file(tmp2, pp2, sizeof(pp2)) : EINVAL;
        const char* use2 = (pp2_err == 0) ? pp2 : tmp2;
        CCASTRoot* root4 = use2 ? cc_tcc_bridge_parse_to_ast(use2, ctx->input_path, ctx->symbols) : NULL;
        if (pp2_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp2);
        if (tmp2) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp2);
            free(tmp2);
        }
        if (!root4) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 2) spawn(...) -> cc_nursery_spawn* (hard error if outside nursery). */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_spawn_stmts_with_nodes(root4, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root4);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        cc_tcc_bridge_free_ast(root4);

        /* Reparse after spawn rewrite so nursery/arena end braces are correct. */
        char* tmp3 = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp3[128];
        int pp3_err = tmp3 ? cc_preprocess_file(tmp3, pp3, sizeof(pp3)) : EINVAL;
        const char* use3 = (pp3_err == 0) ? pp3 : tmp3;
        CCASTRoot* root5 = use3 ? cc_tcc_bridge_parse_to_ast(use3, ctx->input_path, ctx->symbols) : NULL;
        if (pp3_err == 0 && !(getenv("CC_KEEP_REPARSE"))) unlink(pp3);
        if (tmp3) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp3);
            free(tmp3);
        }
        if (!root5) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }

        /* 3) @nursery { ... } -> CCNursery create/wait/free */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_nursery_blocks_with_nodes(root5, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r < 0) {
                cc_tcc_bridge_free_ast(root5);
                fclose(out);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                free(closure_protos);
                free(closure_defs);
                return EINVAL;
            }
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }

        /* 4) @arena(...) { ... } -> CCArena prologue/epilogue */
        {
            char* rewritten = NULL;
            size_t rewritten_len = 0;
            int r = cc__rewrite_arena_blocks_with_nodes(root5, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
            if (r > 0) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rewritten;
                src_ufcs_len = rewritten_len;
            }
        }
        cc_tcc_bridge_free_ast(root5);
    }

    /* Lower @defer (and hard-error on cancel) using a syntax-driven pass.
       IMPORTANT: this must run BEFORE async lowering so `@defer` can be made suspend-safe. */
    if (src_ufcs) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int r = cc__rewrite_defer_syntax(ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (r < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (r > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* AST-driven @async lowering (state machine).
       IMPORTANT: run AFTER CC statement-level lowering so @nursery/@arena/spawn/closures are real C. */
    if (src_ufcs && ctx && ctx->symbols) {
        char* tmp_path = cc__write_temp_c_file(src_ufcs, src_ufcs_len, ctx->input_path);
        char pp_path[128];
        int pp_err = tmp_path ? cc_preprocess_file(tmp_path, pp_path, sizeof(pp_path)) : EINVAL;
        const char* use_path = (pp_err == 0) ? pp_path : tmp_path;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: tmp=%s pp=%s pp_err=%d use=%s\n",
                    tmp_path ? tmp_path : "<null>",
                    (pp_err == 0) ? pp_path : "<n/a>",
                    pp_err,
                    use_path ? use_path : "<null>");
        }
        CCASTRoot* root2 = use_path ? cc_tcc_bridge_parse_to_ast(use_path, ctx->input_path, ctx->symbols) : NULL;
        if (!root2) {
            if (tmp_path) {
                if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
                free(tmp_path);
            }
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (pp_err == 0) root2->lowered_is_temp = 1;
        if (getenv("CC_DEBUG_REPARSE")) {
            fprintf(stderr, "CC: reparse: stub ast node_count=%d\n", root2->node_count);
        }

        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_ast(root2, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        cc_tcc_bridge_free_ast(root2);
        if (tmp_path) {
            if (!getenv("CC_KEEP_REPARSE")) unlink(tmp_path);
            free(tmp_path);
        }
        if (pp_err == 0 && !(getenv("CC_KEEP_REPARSE"))) {
            unlink(pp_path);
        }
        if (ar < 0) {
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            free(closure_protos);
            free(closure_defs);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Strip CC decl markers so output is valid C (run after async lowering so it can see `@async`). */
    if (src_ufcs) {
        char* stripped = NULL;
        size_t stripped_len = 0;
        if (cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = stripped;
            src_ufcs_len = stripped_len;
        }
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include \"cc_nursery.cch\"\n");
    fprintf(out, "#include \"cc_closure.cch\"\n");
    fprintf(out, "#include \"cc_slice.cch\"\n");
    fprintf(out, "#include \"cc_runtime.cch\"\n");
    fprintf(out, "#include \"std/task_intptr.cch\"\n");
    /* Helper alias: used for auto-blocking arg binding to avoid accidental hoisting of these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
    /* Spawn thunks are emitted later (after parsing source) as static fns in this TU. */
    fprintf(out, "\n");
    fprintf(out, "/* --- CC spawn lowering helpers (best-effort) --- */\n");
    fprintf(out, "typedef struct { void (*fn)(void); } __cc_spawn_void_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_void(void* p) {\n");
    fprintf(out, "  __cc_spawn_void_arg* a = (__cc_spawn_void_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn();\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "typedef struct { void (*fn)(int); int arg; } __cc_spawn_int_arg;\n");
    fprintf(out, "static void* __cc_spawn_thunk_int(void* p) {\n");
    fprintf(out, "  __cc_spawn_int_arg* a = (__cc_spawn_int_arg*)p;\n");
    fprintf(out, "  if (a && a->fn) a->fn(a->arg);\n");
    fprintf(out, "  free(a);\n");
    fprintf(out, "  return NULL;\n");
    fprintf(out, "}\n");
    fprintf(out, "/* --- end spawn helpers --- */\n\n");

    /* Captures are lowered via __cc_closure_make_N factories. */
    if (closure_protos && closure_protos_len > 0) {
        fputs("/* --- CC closure forward decls --- */\n", out);
        fwrite(closure_protos, 1, closure_protos_len, out);
        fputs("/* --- end closure forward decls --- */\n\n", out);
    }

    /* Preserve diagnostics mapping to the original input (repo-relative for readability). */
    {
        char rel[1024];
        fprintf(out, "#line 1 \"%s\"\n", cc_path_rel_to_repo(src_path, rel, sizeof(rel)));
    }

    if (src_ufcs) {
        /* Lower `channel_pair(&tx, &rx);` into `cc_chan_pair_create(...)` */
        {
            size_t rp_len = 0;
            char* rp = cc__rewrite_channel_pair_calls_text(ctx, src_ufcs, src_ufcs_len, &rp_len);
            if (!rp) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rp;
            src_ufcs_len = rp_len;
        }
        /* Final safety: ensure invalid surface syntax like `T[~ ... >]` does not reach the C compiler. */
        {
            char* rew_slice = cc__rewrite_slice_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew_slice) {
                fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew_slice;
            src_ufcs_len = strlen(src_ufcs);
        }
        {
            char* rew = cc__rewrite_chan_handle_types_text(ctx, src_ufcs, src_ufcs_len);
            if (!rew) {
            fclose(out);
                return EINVAL;
            }
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rew;
            src_ufcs_len = strlen(src_ufcs);
        }
        /* Rewrite T? -> CCOptional_T */
        {
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: calling cc__rewrite_optional_types_text, len=%zu\n", src_ufcs_len);
            char* rew_opt = cc__rewrite_optional_types_text(ctx, src_ufcs, src_ufcs_len);
            if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: rew_opt=%p\n", (void*)rew_opt);
            if (rew_opt) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_opt;
                src_ufcs_len = strlen(src_ufcs);
                if (getenv("CC_DEBUG_OPTIONAL")) fprintf(stderr, "CC: DEBUG: new len=%zu\n", src_ufcs_len);
            }
        }
        /* Rewrite T!E -> CCResult_T_E and collect result type pairs */
        {
            char* rew_res = cc__rewrite_result_types_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_res) {
            if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_res;
                src_ufcs_len = strlen(src_ufcs);
            }
            
            /* Insert result type declarations into source at the right position.
               We look for "int main(" or first function definition and insert before it. */
            if (cc__cg_result_type_count > 0) {
                const char* insert_pos = strstr(src_ufcs, "int main(");
                if (!insert_pos) insert_pos = strstr(src_ufcs, "void main(");
                if (!insert_pos) {
                    /* Try to find any function definition: "type name(" at start of line */
                    for (size_t k = 0; k < src_ufcs_len && !insert_pos; k++) {
                        if ((k == 0 || src_ufcs[k-1] == '\n') && 
                            (isalpha((unsigned char)src_ufcs[k]) || src_ufcs[k] == '_')) {
                            /* Scan to see if this looks like a function def */
                            size_t fn_start = k;
                            while (k < src_ufcs_len && src_ufcs[k] != '(' && src_ufcs[k] != ';' && src_ufcs[k] != '\n') k++;
                            if (k < src_ufcs_len && src_ufcs[k] == '(') {
                                insert_pos = src_ufcs + fn_start;
                            }
                        }
                    }
                }
                
                if (insert_pos) {
                    size_t insert_offset = (size_t)(insert_pos - src_ufcs);
                    
                    /* Build declaration string */
                    char* decls = NULL;
                    size_t decls_len = 0, decls_cap = 0;
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, 
                        "/* --- CC result type declarations (auto-generated) --- */\n");
                    for (size_t ri = 0; ri < cc__cg_result_type_count; ri++) {
                        CCCodegenResultTypePair* p = &cc__cg_result_types[ri];
                        char line[512];
                        snprintf(line, sizeof(line), "CC_DECL_RESULT_SPEC(CCResult_%s_%s, %s, %s)\n",
                                 p->mangled_ok, p->mangled_err, p->ok_type, p->err_type);
                        cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, line);
                    }
                    cc__sb_append_cstr_local(&decls, &decls_len, &decls_cap, 
                        "/* --- end result type declarations --- */\n\n");
                    
                    /* Build new source: prefix + decls + suffix */
                    char* new_src = NULL;
                    size_t new_len = 0, new_cap = 0;
                    cc__sb_append_local(&new_src, &new_len, &new_cap, src_ufcs, insert_offset);
                    cc__sb_append_local(&new_src, &new_len, &new_cap, decls, decls_len);
                    cc__sb_append_local(&new_src, &new_len, &new_cap, 
                                        src_ufcs + insert_offset, src_ufcs_len - insert_offset);
                    
                    free(decls);
                    if (src_ufcs != src_all) free(src_ufcs);
                    src_ufcs = new_src;
                    src_ufcs_len = new_len;
                }
            }
        }
        /* Rewrite try expr -> cc_try(expr) */
        {
            char* rew_try = cc__rewrite_try_exprs_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_try) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_try;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        /* Rewrite *opt -> cc_unwrap_opt(opt) for optional variables */
        {
            char* rew_unwrap = cc__rewrite_optional_unwrap_text(ctx, src_ufcs, src_ufcs_len);
            if (rew_unwrap) {
                if (src_ufcs != src_all) free(src_ufcs);
                src_ufcs = rew_unwrap;
                src_ufcs_len = strlen(src_ufcs);
            }
        }
        fwrite(src_ufcs, 1, src_ufcs_len, out);
        if (src_ufcs_len == 0 || src_ufcs[src_ufcs_len - 1] != '\n') fputc('\n', out);

        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Emit closure definitions at end-of-file so global names are in scope. */
            fputs("\n#line 1 \"<cc-generated:closures>\"\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
        }
        free(closure_defs);
        if (src_ufcs != src_all) free(src_ufcs);
        free(src_all);
    } else {
        // Fallback stub when input is unavailable.
        fprintf(out,
                "#include \"std/prelude.cch\"\n"
                "int main(void) {\n"
                "  CCArena a = cc_heap_arena(kilobytes(1));\n"
                "  CCString s = cc_string_new(&a, 0);\n"
                "  cc_string_append_cstr(&a, &s, \"Hello, \");\n"
                "  cc_string_append_cstr(&a, &s, \"Concurrent-C via UFCS!\\n\");\n"
                "  cc_std_out_write(cc_string_as_slice(&s));\n"
                "  return 0;\n"
                "}\n");
    }

    fclose(out);
    return 0;
}

