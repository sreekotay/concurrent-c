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
#include "visitor/pass_common.h"

/* Local aliases for shared helpers */
#define cc__sb_append_local cc_sb_append
#define cc__sb_append_cstr_local cc_sb_append_cstr
#define cc__is_ident_char_local2 cc_is_ident_char
#define cc__is_ident_start_local2 cc_is_ident_start
#define cc__skip_ws_local2 cc_skip_ws

/* Global counter for unique owned channel IDs */
static int g_owned_channel_id = 0;

/* Helper: wrap a closure with typed parameter for CCClosure1.
   Transforms: [captures](Type param) => body
   Into:       [captures](intptr_t __arg) => { Type param = (Type)__arg; body }
   
   If param is already intptr_t, returns the original unchanged. */
static void cc__wrap_typed_closure1_local(const char* closure, char* out, size_t out_cap) {
    if (!closure || !out || out_cap == 0) return;
    out[0] = 0;
    
    /* Find '[' (captures start) */
    const char* p = closure;
    while (*p && *p != '[') p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    /* Find '](' to get to params */
    const char* cap_start = p;
    while (*p && !(*p == ']' && *(p + 1) == '(')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    
    size_t cap_len = (size_t)(p - cap_start + 1);  /* include ] */
    p++;  /* skip ] */
    if (*p != '(') { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p++;  /* skip ( */
    
    /* Skip whitespace */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter type */
    const char* type_start = p;
    /* Scan type - handle pointers like "CCArena*" */
    while (*p && *p != ')' && *p != ' ' && *p != '\t') {
        if (*p == '*') { p++; break; }  /* pointer type ends at * */
        p++;
    }
    size_t type_len = (size_t)(p - type_start);
    
    /* Check if it's already intptr_t */
    if (type_len == 8 && strncmp(type_start, "intptr_t", 8) == 0) {
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Skip whitespace to parameter name */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Extract parameter name */
    const char* name_start = p;
    while (*p && *p != ')' && *p != ' ' && *p != '\t') p++;
    size_t name_len = (size_t)(p - name_start);
    
    if (name_len == 0) {
        /* No param name, type might BE the name (e.g., just "r") - don't wrap */
        strncpy(out, closure, out_cap - 1);
        out[out_cap - 1] = 0;
        return;
    }
    
    /* Find => and body */
    while (*p && !(*p == '=' && *(p + 1) == '>')) p++;
    if (!*p) { strncpy(out, closure, out_cap - 1); out[out_cap - 1] = 0; return; }
    p += 2;  /* skip => */
    while (*p && (*p == ' ' || *p == '\t')) p++;
    
    /* Rest is body */
    const char* body = p;
    
    /* Build wrapped closure:
       [captures](intptr_t __arg) => { Type name = (Type)__arg; body } */
    char type_buf[128], name_buf[64];
    if (type_len >= sizeof(type_buf)) type_len = sizeof(type_buf) - 1;
    if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
    memcpy(type_buf, type_start, type_len);
    type_buf[type_len] = 0;
    memcpy(name_buf, name_start, name_len);
    name_buf[name_len] = 0;
    
    /* Check if body is already a block */
    int body_is_block = (*body == '{');
    
    if (body_is_block) {
        /* Body is { ... }, insert declaration after { */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; %s",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body + 1);
    } else {
        /* Body is expression, wrap in block */
        snprintf(out, out_cap, "%.*s(intptr_t __arg) => { %s %s = (%s)__arg; return %s; }",
                 (int)cap_len, cap_start, type_buf, name_buf, type_buf, body);
    }
}

/* Scan for matching closing brace, accounting for nested braces, strings, and comments.
 * Returns the position of the matching '}' or (size_t)-1 if not found. */
static size_t cc__scan_matching_brace(const char* src, size_t len, size_t open_brace) {
    if (!src || open_brace >= len || src[open_brace] != '{') return (size_t)-1;
    int depth = 0;
    int in_str = 0, in_chr = 0, in_line_comment = 0, in_block_comment = 0;
    for (size_t i = open_brace; i < len; i++) {
        char c = src[i];
        char c2 = (i + 1 < len) ? src[i + 1] : 0;
        
        if (in_line_comment) { if (c == '\n') in_line_comment = 0; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < len) { i++; continue; } if (c == '"') in_str = 0; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < len) { i++; continue; } if (c == '\'') in_chr = 0; continue; }
        
        if (c == '/' && c2 == '/') { in_line_comment = 1; i++; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i;
        }
    }
    return (size_t)-1;
}

/* Parse the owned block to extract closure texts.
 * Expected format: { .create = <closure>, .destroy = <closure>, .reset = <closure> }
 * Returns 1 on success, 0 on failure. */
static int cc__parse_owned_block(const char* src, size_t start, size_t end,
                                  char* out_create, size_t create_cap,
                                  char* out_destroy, size_t destroy_cap,
                                  char* out_reset, size_t reset_cap) {
    if (!src || start >= end) return 0;
    out_create[0] = 0;
    out_destroy[0] = 0;
    out_reset[0] = 0;
    
    size_t i = start;
    while (i < end) {
        /* Skip whitespace */
        while (i < end && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r' || src[i] == ',')) i++;
        if (i >= end) break;
        
        /* Look for .field = */
        if (src[i] != '.') { i++; continue; }
        i++;
        
        /* Read field name */
        char field[32];
        size_t fn = 0;
        while (i < end && fn + 1 < sizeof(field) && cc_is_ident_char(src[i])) {
            field[fn++] = src[i++];
        }
        field[fn] = 0;
        
        /* Skip to '=' */
        while (i < end && (src[i] == ' ' || src[i] == '\t')) i++;
        if (i >= end || src[i] != '=') continue;
        i++;
        while (i < end && (src[i] == ' ' || src[i] == '\t')) i++;
        
        /* Find the closure: [captures](params) => body */
        size_t closure_start = i;
        
        /* Find '=>' to locate the closure body */
        size_t arrow = (size_t)-1;
        for (size_t j = i; j + 1 < end; j++) {
            if (src[j] == '=' && src[j + 1] == '>') { arrow = j; break; }
        }
        if (arrow == (size_t)-1) continue;
        
        /* Find the end of the closure body */
        size_t body_start = arrow + 2;
        while (body_start < end && (src[body_start] == ' ' || src[body_start] == '\t' || src[body_start] == '\n')) body_start++;
        
        size_t closure_end;
        if (body_start < end && src[body_start] == '{') {
            /* Block body - find matching } */
            size_t rbrace = cc__scan_matching_brace(src, end, body_start);
            if (rbrace == (size_t)-1) continue;
            closure_end = rbrace + 1;
        } else {
            /* Expression body - find comma or end of block */
            closure_end = body_start;
            int paren_depth = 0;
            while (closure_end < end) {
                char c = src[closure_end];
                if (c == '(' || c == '[') paren_depth++;
                else if (c == ')' || c == ']') paren_depth--;
                else if (paren_depth == 0 && (c == ',' || c == '}')) break;
                closure_end++;
            }
            /* Trim trailing whitespace */
            while (closure_end > body_start && (src[closure_end - 1] == ' ' || src[closure_end - 1] == '\t' || src[closure_end - 1] == '\n')) {
                closure_end--;
            }
        }
        
        /* Copy closure text to appropriate output */
        size_t closure_len = closure_end - closure_start;
        char* dest = NULL;
        size_t dest_cap = 0;
        if (strcmp(field, "create") == 0) { dest = out_create; dest_cap = create_cap; }
        else if (strcmp(field, "destroy") == 0) { dest = out_destroy; dest_cap = destroy_cap; }
        else if (strcmp(field, "reset") == 0) { dest = out_reset; dest_cap = reset_cap; }
        
        if (dest && closure_len < dest_cap) {
            memcpy(dest, src + closure_start, closure_len);
            dest[closure_len] = 0;
        }
        
        i = closure_end;
    }
    
    return (out_create[0] != 0 && out_destroy[0] != 0);  /* create and destroy are required */
}

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
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair expects '&tx, &rx'");
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
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair must be used as statement or expression");
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
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair could not find declarations for '%s' and '%s'", tx_name, rx_name);
                    fprintf(stderr, "  note: ensure both channel handles are declared before this call\n");
                    fprintf(stderr, "  hint: use 'T[~N >] %s; T[~N <] %s;' to declare send/recv handles\n", tx_name, rx_name);
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
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair requires send handle (>) first, then recv handle (<)");
                    fprintf(stderr, "  note: '%s' is %s, '%s' is %s\n",
                            tx_name, tx_is_tx ? "send (>)" : tx_is_rx ? "recv (<)" : "unknown",
                            rx_name, rx_is_rx ? "recv (<)" : rx_is_tx ? "send (>)" : "unknown");
                    fprintf(stderr, "  hint: use channel_pair(&tx, &rx) where tx is T[~N >] and rx is T[~N <]\n");
                    free(out);
                    return NULL;
                }

                if (tx_unknown || rx_unknown) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair unknown token in spec");
                    free(out);
                    return NULL;
                }
                
                if (tx_mode == -1) tx_mode = 0;
                if (rx_mode == -1) rx_mode = 0;
                if (tx_mode != rx_mode) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair mode mismatch (tx=%d, rx=%d)", tx_mode, rx_mode);
                    fprintf(stderr, "  hint: both handles must have the same mode specifier\n");
                    free(out);
                    return NULL;
                }
                
                if (tx_has_topo != rx_has_topo || (tx_has_topo && strcmp(tx_topo, rx_topo) != 0)) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair topology mismatch (tx='%s', rx='%s')", tx_has_topo ? tx_topo : "(none)", rx_has_topo ? rx_topo : "(none)");
                    fprintf(stderr, "  hint: both handles must have the same topology (mpmc, spsc, etc.)\n");
                    free(out);
                    return NULL;
                }
                
                if (tx_bp == -1) tx_bp = 0;
                if (rx_bp == -1) rx_bp = 0;
                if (tx_bp != rx_bp) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel_pair backpressure mismatch (tx=%d, rx=%d)", tx_bp, rx_bp);
                    fprintf(stderr, "  hint: both handles must have the same backpressure setting\n");
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
                        cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                line, col, CC_ERR_CHANNEL, "channel_pair capacity mismatch");
                        free(out);
                        return NULL;
                    }
                    snprintf(cap_expr, sizeof(cap_expr), "%s", tx_cap_expr);
                } else if (tx_cap >= 1 && rx_cap >= 1 && tx_cap == rx_cap) {
                    snprintf(cap_expr, sizeof(cap_expr), "%lld", tx_cap);
                } else {
                    cc_pass_error_cat(ctx && ctx->input_path ? ctx->input_path : "<input>",
                            line, col, CC_ERR_CHANNEL,
                            "channel_pair capacity mismatch (tx=%lld, rx=%lld)", tx_cap, rx_cap);
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
                /* Check for 'owned' keyword */
                int is_owned = 0;
                size_t owned_brace = 0;
                size_t owned_end = 0;
                {
                    /* Scan capacity expression to find 'owned' keyword.
                       Capacity can be: digits, identifiers, operators, or parenthesized expressions like (cap + 2). */
                    size_t scan = j + 1;
                    while (scan < n && src[scan] != ']') {
                        char sc = src[scan];
                        /* Skip whitespace */
                        if (sc == ' ' || sc == '\t') { scan++; continue; }
                        /* Check for 'owned' keyword BEFORE processing identifiers */
                        if (scan + 5 <= n && memcmp(src + scan, "owned", 5) == 0 &&
                            (scan + 5 >= n || !cc__is_ident_char_local2(src[scan + 5]))) {
                            is_owned = 1;
                            scan += 5;
                            while (scan < n && (src[scan] == ' ' || src[scan] == '\t')) scan++;
                            if (scan < n && src[scan] == '{') {
                                owned_brace = scan;
                                owned_end = cc__scan_matching_brace(src, n, scan);
                                if (owned_end == (size_t)-1) {
                                    char rel[1024];
                                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                            line, col, CC_ERR_CHANNEL, "unterminated owned block");
                                    free(out);
                                    return NULL;
                                }
                            }
                            break;
                        }
                        /* Skip digits, identifiers, operators */
                        if ((sc >= '0' && sc <= '9') || sc == '_' ||
                            (sc >= 'a' && sc <= 'z') || (sc >= 'A' && sc <= 'Z') ||
                            sc == '+' || sc == '-' || sc == '*' || sc == '/') { scan++; continue; }
                        /* Skip parenthesized expressions */
                        if (sc == '(') {
                            int depth = 1;
                            scan++;
                            while (scan < n && depth > 0) {
                                if (src[scan] == '(') depth++;
                                else if (src[scan] == ')') depth--;
                                scan++;
                            }
                            continue;
                        }
                        /* Stop at direction markers or close bracket */
                        if (sc == '>' || sc == '<') break;
                        scan++;
                    }
                }
                
                size_t k;
                if (is_owned && owned_end != (size_t)-1 && owned_end != 0) {
                    /* Owned channel: find ] after the owned block */
                    k = owned_end + 1;
                    while (k < n && (src[k] == ' ' || src[k] == '\t')) k++;
                    if (k >= n || src[k] != ']') {
                        char rel[1024];
                        cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                line, col, CC_ERR_CHANNEL, "expected ']' after owned block");
                        free(out);
                        return NULL;
                    }
                } else {
                    /* Regular channel: find ] */
                    k = j + 1;
                    while (k < n && src[k] != ']' && src[k] != '\n') k++;
                }
                
                if (k >= n || src[k] != ']') {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "unterminated channel handle type");
                    free(out);
                    return NULL;
                }
                
                /* Handle owned channels FIRST (they don't need > or <) */
                if (is_owned && owned_brace != 0 && owned_end != (size_t)-1) {
                    /* Parse owned channel */
                    char create_closure[2048], destroy_closure[2048], reset_closure[2048];
                    if (!cc__parse_owned_block(src, owned_brace + 1, owned_end,
                                               create_closure, sizeof(create_closure),
                                               destroy_closure, sizeof(destroy_closure),
                                               reset_closure, sizeof(reset_closure))) {
                        char rel[1024];
                        cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                line, col, CC_ERR_CHANNEL, "owned block requires .create and .destroy");
                        free(out);
                        return NULL;
                    }
                    
                    /* Extract element type (before [~) */
                    size_t ty_start = i;
                    while (ty_start > 0) {
                        char p = src[ty_start - 1];
                        if (p == ';' || p == '{' || p == '}' || p == ',' || p == '(' || p == ')' || p == '\n') break;
                        ty_start--;
                    }
                    while (ty_start < i && (src[ty_start] == ' ' || src[ty_start] == '\t')) ty_start++;
                    
                    char elem_ty[256];
                    size_t elem_len = i - ty_start;
                    if (elem_len >= sizeof(elem_ty)) elem_len = sizeof(elem_ty) - 1;
                    memcpy(elem_ty, src + ty_start, elem_len);
                    elem_ty[elem_len] = 0;
                    while (elem_len > 0 && (elem_ty[elem_len - 1] == ' ' || elem_ty[elem_len - 1] == '\t')) elem_ty[--elem_len] = 0;
                    
                    /* Extract capacity (between ~ and 'owned' keyword).
                       Capacity can be expressions like (cap + 2), so we need to handle
                       parentheses and not stop at spaces. */
                    char cap_expr[128] = "0";
                    {
                        size_t cs = j + 1;  /* Start after ~ */
                        while (cs < n && (src[cs] == ' ' || src[cs] == '\t')) cs++;
                        size_t ce = cs;
                        /* Scan capacity expression, handling parentheses */
                        int paren_depth = 0;
                        while (ce < n) {
                            char sc = src[ce];
                            if (sc == '(') { paren_depth++; ce++; continue; }
                            if (sc == ')') { paren_depth--; ce++; continue; }
                            /* At depth 0, check for 'owned' keyword */
                            if (paren_depth == 0 && ce + 5 <= n && memcmp(src + ce, "owned", 5) == 0 &&
                                (ce + 5 >= n || !cc__is_ident_char_local2(src[ce + 5]))) {
                                break;  /* Found 'owned' keyword */
                            }
                            ce++;
                        }
                        /* Trim trailing whitespace from capacity */
                        while (ce > cs && (src[ce - 1] == ' ' || src[ce - 1] == '\t')) ce--;
                        if (ce > cs) {
                            size_t cl = ce - cs;
                            if (cl >= sizeof(cap_expr)) cl = sizeof(cap_expr) - 1;
                            memcpy(cap_expr, src + cs, cl);
                            cap_expr[cl] = 0;
                        }
                    }
                    
                    /* Find variable name (after ]) */
                    size_t var_start = k + 1;
                    while (var_start < n && (src[var_start] == ' ' || src[var_start] == '\t')) var_start++;
                    size_t var_end = var_start;
                    while (var_end < n && cc__is_ident_char_local2(src[var_end])) var_end++;
                    
                    char var_name[128];
                    size_t var_len = var_end - var_start;
                    if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
                    memcpy(var_name, src + var_start, var_len);
                    var_name[var_len] = 0;
                    
                    /* Find the semicolon */
                    size_t semi = var_end;
                    while (semi < n && src[semi] != ';') semi++;
                    if (semi >= n) {
                        char rel[1024];
                        cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                                line, col, CC_ERR_CHANNEL, "expected ';' after owned channel declaration");
                        free(out);
                        return NULL;
                    }
                    
                    int owned_id = g_owned_channel_id++;
                    
                    /* Generate output */
                    if (ty_start >= last_emit) {
                        cc__sb_append_local(&out, &out_len, &out_cap, src + last_emit, ty_start - last_emit);
                    }
                    
                    /* Wrap destroy/reset closures to handle typed parameters */
                    char destroy_wrapped[2048], reset_wrapped[2048];
                    cc__wrap_typed_closure1_local(destroy_closure, destroy_wrapped, sizeof(destroy_wrapped));
                    cc__wrap_typed_closure1_local(reset_closure, reset_wrapped, sizeof(reset_wrapped));
                    
                    /* Emit closure declarations */
                    char buf[4096];
                    snprintf(buf, sizeof(buf),
                             "/* owned channel %s */\n"
                             "CCClosure0 __cc_owned_%d_create = %s;\n"
                             "CCClosure1 __cc_owned_%d_destroy = %s;\n",
                             var_name, owned_id, create_closure,
                             owned_id, destroy_wrapped);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, buf);
                    
                    if (reset_closure[0]) {
                        snprintf(buf, sizeof(buf),
                                 "CCClosure1 __cc_owned_%d_reset = %s;\n",
                                 owned_id, reset_wrapped);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, buf);
                    } else {
                        snprintf(buf, sizeof(buf),
                                 "CCClosure1 __cc_owned_%d_reset = {0};\n",
                                 owned_id);
                        cc__sb_append_cstr_local(&out, &out_len, &out_cap, buf);
                    }
                    
                    /* Emit channel creation */
                    snprintf(buf, sizeof(buf),
                             "CCChan* %s = cc_chan_create_owned(%s, sizeof(%s), "
                             "__cc_owned_%d_create, __cc_owned_%d_destroy, __cc_owned_%d_reset)",
                             var_name, cap_expr, elem_ty,
                             owned_id, owned_id, owned_id);
                    cc__sb_append_cstr_local(&out, &out_len, &out_cap, buf);
                    
                    last_emit = semi;  /* Leave the ; to be emitted */
                    while (i <= semi) { if (src[i] == '\n') { line++; col = 1; } else col++; i++; }
                    continue;
                }
                
                int saw_gt = 0, saw_lt = 0;
                for (size_t t = j; t < k; t++) {
                    if (src[t] == '>') saw_gt = 1;
                    if (src[t] == '<') saw_lt = 1;
                }
                if (saw_gt && saw_lt) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel type cannot be both '>' and '<'");
                    free(out);
                    return NULL;
                }
                if (!saw_gt && !saw_lt) {
                    char rel[1024];
                    cc_pass_error_cat(cc_path_rel_to_repo(ctx && ctx->input_path ? ctx->input_path : "<input>", rel, sizeof(rel)),
                            line, col, CC_ERR_CHANNEL, "channel type requires '>' or '<'");
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
