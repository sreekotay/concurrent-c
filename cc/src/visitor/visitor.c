#include "visitor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "visitor/ufcs.h"

/* --- Closure scan/lowering helpers (best-effort, early) ---
   Goal: allow `spawn(() => { ... })` to lower to valid C by generating a
   top-level env+thunk and rewriting the spawn statement to use CCClosure0. */

typedef struct {
    int start_line;
    int end_line;
    int nursery_id;
    int id;
    char** cap_names;
    int cap_count;
    char* body; /* includes surrounding { ... } */
} CCClosureDesc;

static int cc__is_ident_start_char(char c) { return (c == '_' || isalpha((unsigned char)c)); }
static int cc__is_ident_char2(char c) { return (c == '_' || isalnum((unsigned char)c)); }

static int cc__is_keyword_tok(const char* s, size_t n) {
    static const char* kw[] = {
        "if","else","for","while","do","switch","case","default","break","continue","return",
        "sizeof","struct","union","enum","typedef","static","extern","const","volatile","restrict",
        "void","char","short","int","long","float","double","_Bool","signed","unsigned",
        "goto","auto","register","_Atomic","_Alignas","_Alignof","_Thread_local",
        "true","false","NULL"
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strlen(kw[i]) == n && strncmp(kw[i], s, n) == 0) return 1;
    }
    return 0;
}

static int cc__name_in_list(char** xs, int n, const char* s, size_t slen) {
    for (int i = 0; i < n; i++) {
        if (!xs[i]) continue;
        if (strlen(xs[i]) == slen && strncmp(xs[i], s, slen) == 0) return 1;
    }
    return 0;
}

static void cc__maybe_record_decl(char*** scope_names, int* scope_counts, int depth, const char* line) {
    if (!scope_names || !scope_counts || depth < 0 || depth >= 256 || !line) return;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0') return;
    if (!strchr(p, ';')) return;
    const char* types[] = {"int","char","size_t","ssize_t","bool","CCSlice","CCArena","CCChan","CCNursery","CCDeadline","CCFuture"};
    const char* after = NULL;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t tn = strlen(types[i]);
        if (strncmp(p, types[i], tn) == 0 && isspace((unsigned char)p[tn])) { after = p + tn; break; }
    }
    if (!after) return;
    p = after;
    while (*p == ' ' || *p == '\t') p++;
    while (*p == '*') { p++; while (*p == ' ' || *p == '\t') p++; }
    if (!cc__is_ident_start_char(*p)) return;
    const char* s = p++;
    while (cc__is_ident_char2(*p)) p++;
    size_t n = (size_t)(p - s);
    if (n == 0 || cc__is_keyword_tok(s, n)) return;
    int cur_n = scope_counts[depth];
    if (cc__name_in_list(scope_names[depth], cur_n, s, n)) return;
    char* name = (char*)malloc(n + 1);
    if (!name) return;
    memcpy(name, s, n);
    name[n] = '\0';
    char** next = (char**)realloc(scope_names[depth], (size_t)(cur_n + 1) * sizeof(char*));
    if (!next) { free(name); return; }
    scope_names[depth] = next;
    scope_names[depth][cur_n] = name;
    scope_counts[depth] = cur_n + 1;
}

static void cc__collect_caps_from_block(char*** scope_names,
                                        int* scope_counts,
                                        int max_depth,
                                        const char* block,
                                        char*** out_caps,
                                        int* out_cap_count) {
    if (!scope_names || !scope_counts || !block || !out_caps || !out_cap_count) return;
    const char* p = block;
    while (*p) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == q) { p++; break; }
                p++;
            }
            continue;
        }
        if (!cc__is_ident_start_char(*p)) { p++; continue; }
        const char* s = p++;
        while (cc__is_ident_char2(*p)) p++;
        size_t n = (size_t)(p - s);
        if (cc__is_keyword_tok(s, n)) continue;
        /* ignore member access */
        if (s > block && (s[-1] == '.' || (s[-1] == '>' && s > block + 1 && s[-2] == '-'))) continue;
        int found = 0;
        for (int d = max_depth; d >= 0 && !found; d--) {
            if (cc__name_in_list(scope_names[d], scope_counts[d], s, n)) found = 1;
        }
        if (!found) continue;
        if (cc__name_in_list(*out_caps, *out_cap_count, s, n)) continue;
        char* name = (char*)malloc(n + 1);
        if (!name) continue;
        memcpy(name, s, n);
        name[n] = '\0';
        char** next = (char**)realloc(*out_caps, (size_t)(*out_cap_count + 1) * sizeof(char*));
        if (!next) { free(name); continue; }
        *out_caps = next;
        (*out_caps)[*out_cap_count] = name;
        (*out_cap_count)++;
    }
}

static int cc__append_str(char** buf, size_t* len, size_t* cap, const char* s) {
    if (!buf || !len || !cap || !s) return 0;
    size_t n = strlen(s);
    size_t need = *len + n + 1;
    if (need > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < need) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

/* Scan `src` for spawn closures and generate top-level thunks.
   Outputs:
   - `*out_descs`, `*out_count`
   - `*out_line_map`: 1-based line -> (index+1) into desc array, 0 if none
   - `*out_line_cap`: number of lines allocated
   - `*out_defs`: C defs to emit before #line 1 */
static int cc__scan_spawn_closures(const char* src,
                                  size_t src_len,
                                  const char* src_path,
                                  CCClosureDesc** out_descs,
                                  int* out_count,
                                  int** out_line_map,
                                  int* out_line_cap,
                                  char** out_defs,
                                  size_t* out_defs_len) {
    if (!src || !out_descs || !out_count || !out_line_map || !out_line_cap || !out_defs || !out_defs_len) return 0;
    *out_descs = NULL;
    *out_count = 0;
    *out_line_map = NULL;
    *out_line_cap = 0;
    *out_defs = NULL;
    *out_defs_len = 0;

    int lines = 1;
    for (size_t i = 0; i < src_len; i++) if (src[i] == '\n') lines++;
    int* line_map = (int*)calloc((size_t)lines + 2, sizeof(int));
    if (!line_map) return 0;

    CCClosureDesc* descs = NULL;
    int count = 0, cap = 0;
    char* defs = NULL;
    size_t defs_len = 0, defs_cap = 0;

    char** scope_names[256];
    int scope_counts[256];
    for (int i = 0; i < 256; i++) { scope_names[i] = NULL; scope_counts[i] = 0; }
    int depth = 0;
    int nursery_stack[128];
    int nursery_depth[128];
    int nursery_top = -1;
    int nursery_counter = 0;

    const char* cur = src;
    int line_no = 1;
    while ((size_t)(cur - src) < src_len && *cur) {
        const char* line_start = cur;
        const char* nl = strchr(cur, '\n');
        const char* line_end = nl ? nl : (src + src_len);
        size_t line_len = (size_t)(line_end - line_start);

        char tmp_line[1024];
        size_t cp = line_len < sizeof(tmp_line) - 1 ? line_len : sizeof(tmp_line) - 1;
        memcpy(tmp_line, line_start, cp);
        tmp_line[cp] = '\0';
        cc__maybe_record_decl(scope_names, scope_counts, depth, tmp_line);

        /* nursery marker */
        const char* t = line_start;
        while (t < line_end && (*t == ' ' || *t == '\t')) t++;
        if ((line_end - t) >= 8 && strncmp(t, "@nursery", 8) == 0) {
            int nid = ++nursery_counter;
            if (nursery_top + 1 < 128) {
                nursery_stack[++nursery_top] = nid;
                nursery_depth[nursery_top] = -1;
            }
        }

        /* spawn closure */
        const char* sp = strstr(line_start, "spawn");
        if (sp && sp < line_end) {
            const char* p = sp + 5;
            while (p < line_end && (*p == ' ' || *p == '\t')) p++;
            if (p < line_end && *p == '(') {
                p++;
                while (p < line_end && (*p == ' ' || *p == '\t')) p++;
                if (p + 2 <= line_end && p[0] == '(' && p[1] == ')') {
                    const char* a = p + 2;
                    while (a < line_end && (*a == ' ' || *a == '\t')) a++;
                    if ((a + 2) <= line_end && a[0] == '=' && a[1] == '>') {
                        a += 2;
                        while (a < line_end && (*a == ' ' || *a == '\t')) a++;
                        if (a < line_end && *a == '{') {
                            const char* b = a;
                            int br = 0;
                            int in_str = 0;
                            char str_q = 0;
                            while ((size_t)(b - src) < src_len) {
                                char c = *b++;
                                if (in_str) {
                                    if (c == '\\' && (size_t)(b - src) < src_len) { b++; continue; }
                                    if (c == str_q) in_str = 0;
                                    continue;
                                }
                                if (c == '"' || c == '\'') { in_str = 1; str_q = c; continue; }
                                if (c == '{') br++;
                                else if (c == '}') { br--; if (br == 0) break; }
                            }
                            if (br == 0) {
                                const char* block_end = b;
                                int end_line = line_no;
                                for (const char* x = a; x < block_end; x++) if (*x == '\n') end_line++;
                                int nid = (nursery_top >= 0) ? nursery_stack[nursery_top] : 0;

                                char* body = (char*)malloc((size_t)(block_end - a) + 1);
                                if (!body) { free(line_map); return 0; }
                                memcpy(body, a, (size_t)(block_end - a));
                                body[(size_t)(block_end - a)] = '\0';

                                char** caps = NULL;
                                int cap_n = 0;
                                cc__collect_caps_from_block(scope_names, scope_counts, depth, body, &caps, &cap_n);

                                if (count == cap) {
                                    cap = cap ? cap * 2 : 16;
                                    CCClosureDesc* nd = (CCClosureDesc*)realloc(descs, (size_t)cap * sizeof(CCClosureDesc));
                                    if (!nd) { free(body); free(line_map); return 0; }
                                    descs = nd;
                                }
                                int id = count + 1;
                                descs[count++] = (CCClosureDesc){
                                    .start_line = line_no,
                                    .end_line = end_line,
                                    .nursery_id = nid,
                                    .id = id,
                                    .cap_names = caps,
                                    .cap_count = cap_n,
                                    .body = body,
                                };
                                if (line_no <= lines) line_map[line_no] = count; /* 1-based index */

                                char hdr[256];
                                snprintf(hdr, sizeof(hdr), "/* CC closure %d (from %s:%d) */\n", id, src_path ? src_path : "<src>", line_no);
                                cc__append_str(&defs, &defs_len, &defs_cap, hdr);
                                {
                                    char buf[8192];
                                    size_t off = 0;
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "typedef struct __cc_closure_env_%d {\n", id);
                                    for (int ci = 0; ci < cap_n; ci++) {
                                        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "  __typeof__(%s) %s;\n", caps[ci], caps[ci]);
                                    }
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "} __cc_closure_env_%d;\n", id);
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "static void* __cc_closure_entry_%d(void* __p) {\n", id);
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)__p;\n", id, id);
                                    for (int ci = 0; ci < cap_n; ci++) {
                                        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                                                "  const __typeof__(__env->%s) %s = __env->%s;\n",
                                                                caps[ci], caps[ci], caps[ci]);
                                    }
                                    /* Source mapping: make closure body diagnostics point to original .ccs. */
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "#line %d \"%s\"\n", line_no, src_path ? src_path : "<src>");
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "  %s\n", body);
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "  return NULL;\n}\n");
                                    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "static void __cc_closure_drop_%d(void* __p) { free(__p); }\n\n", id);
                                    cc__append_str(&defs, &defs_len, &defs_cap, buf);
                                }

                                /* advance cursor */
                                cur = block_end;
                                line_no = end_line;
                                if (*cur == '\n') { cur++; line_no++; }
                                continue;
                            }
                        }
                    }
                }
            }
        }

        /* brace depth */
        for (const char* x = line_start; x < line_end; x++) {
            if (*x == '{') {
                depth++;
                if (nursery_top >= 0 && nursery_depth[nursery_top] < 0) nursery_depth[nursery_top] = depth;
            } else if (*x == '}') {
                if (nursery_top >= 0 && nursery_depth[nursery_top] == depth) nursery_top--;
                if (depth > 0) depth--;
            }
        }

        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    /* scope names cleanup */
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
    }

    *out_descs = descs;
    *out_count = count;
    *out_line_map = line_map;
    *out_line_cap = lines + 2;
    *out_defs = defs;
    *out_defs_len = defs_len;
    return 1;
}

#ifndef CC_TCC_EXT_AVAILABLE
// Minimal fallbacks when TCC extensions are not available.
static int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 1;
}

static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    (void)root;
    if (!ctx || !ctx->input_path || !node_file) return 0;
    return strcmp(ctx->input_path, node_file) == 0;
}
#endif

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

#ifdef CC_TCC_EXT_AVAILABLE
struct CC__UFCSSpan {
    size_t start; /* inclusive */
    size_t end;   /* exclusive */
};

static int cc__read_entire_file(const char* path, char** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return 0;
    *out_buf = NULL;
    *out_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_buf = buf;
    *out_len = n;
    return 1;
}

static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no) {
    if (!s || line_no <= 1) return 0;
    int cur = 1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            cur++;
            if (cur == line_no) return i + 1;
        }
    }
    return len;
}

static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no) {
    if (!s) return 0;
    if (line_no <= 1 && col_no <= 1) return 0;
    if (col_no <= 1) return cc__offset_of_line_1based(s, len, line_no);
    size_t loff = cc__offset_of_line_1based(s, len, line_no);
    size_t off = loff + (size_t)(col_no - 1);
    if (off > len) off = len;
    return off;
}

static size_t cc__scan_receiver_start_left(const char* s, size_t range_start, size_t sep_pos) {
    if (!s) return range_start;
    size_t r_end = sep_pos;
    while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
    if (r_end <= range_start) return range_start;
    int par = 0, br = 0, brc = 0;
    size_t r = r_end;
    while (r > range_start) {
        char c = s[r - 1];
        if (c == ')') { par++; r--; continue; }
        if (c == ']') { br++; r--; continue; }
        if (c == '}') { brc++; r--; continue; }
        if (c == '(' && par > 0) { par--; r--; continue; }
        if (c == '[' && br > 0) { br--; r--; continue; }
        if (c == '{' && brc > 0) { brc--; r--; continue; }
        if (par || br || brc) { r--; continue; }
        if (c == ',' || c == ';' || c == '=' || c == '\n' ||
            c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
            c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
            c == '<' || c == '>' || c == '?' || c == ':' ) {
            break;
        }
        r--;
    }
    while (r < r_end && isspace((unsigned char)s[r])) r++;
    return r;
}

static int cc__span_from_anchor_and_end(const char* s,
                                       size_t range_start,
                                       size_t sep_pos,
                                       size_t end_pos_excl,
                                       struct CC__UFCSSpan* out_span) {
    if (!s || !out_span) return 0;
    if (sep_pos < range_start) return 0;
    if (end_pos_excl <= sep_pos) return 0;
    out_span->start = cc__scan_receiver_start_left(s, range_start, sep_pos);
    out_span->end = end_pos_excl;
    return out_span->start < out_span->end;
}

static int cc__find_ufcs_span_in_range(const char* s,
                                       size_t range_start,
                                       size_t range_end,
                                       const char* method,
                                       int occurrence_1based,
                                       struct CC__UFCSSpan* out_span) {
    if (!s || !method || !out_span) return 0;
    const size_t method_len = strlen(method);
    if (method_len == 0) return 0;
    if (occurrence_1based <= 0) occurrence_1based = 1;
    int seen = 0;

    /* Find ".method" or "->method" followed by optional whitespace then '(' */
    for (size_t i = range_start; i + method_len + 2 < range_end; i++) {
        int is_arrow = 0;
        size_t sep_pos = 0;
        if (s[i] == '.' ) { is_arrow = 0; sep_pos = i; }
        else if (s[i] == '-' && i + 1 < range_end && s[i + 1] == '>') { is_arrow = 1; sep_pos = i; }
        else continue;

        size_t mpos = sep_pos + (is_arrow ? 2 : 1);
        while (mpos < range_end && isspace((unsigned char)s[mpos])) mpos++;
        if (mpos + method_len >= range_end) continue;
        if (memcmp(s + mpos, method, method_len) != 0) continue;

        size_t after = mpos + method_len;
        while (after < range_end && isspace((unsigned char)s[after])) after++;
        if (after >= range_end || s[after] != '(') continue;

        /* Match Nth occurrence. */
        seen++;
        if (seen != occurrence_1based) continue;

        /* Receiver: allow non-trivial expressions like (foo()).bar, arr[i].m, (*p).m.
           Find the start by scanning left with bracket balancing until a delimiter. */
        size_t r_end = sep_pos;
        while (r_end > range_start && isspace((unsigned char)s[r_end - 1])) r_end--;
        if (r_end == range_start) continue;

        int par = 0, br = 0, brc = 0;
        size_t r = r_end;
        while (r > range_start) {
            char c = s[r - 1];
            if (c == ')') { par++; r--; continue; }
            if (c == ']') { br++; r--; continue; }
            if (c == '}') { brc++; r--; continue; }
            if (c == '(' && par > 0) { par--; r--; continue; }
            if (c == '[' && br > 0) { br--; r--; continue; }
            if (c == '{' && brc > 0) { brc--; r--; continue; }
            if (par || br || brc) { r--; continue; }

            /* At top-level: stop on likely expression delimiters. */
            if (c == ',' || c == ';' || c == '=' || c == '\n' ||
                c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
                c == '&' || c == '|' || c == '^' || c == '!' || c == '~' ||
                c == '<' || c == '>' || c == '?' || c == ':' ) {
                break;
            }
            /* Otherwise keep consuming (identifiers, dots, brackets, parens, spaces). */
            r--;
        }
        /* Trim any leading whitespace included in the backward scan. */
        while (r < r_end && isspace((unsigned char)s[r])) r++;
        if (r >= r_end) continue;

        /* Find matching ')' for the call, skipping strings/chars. */
        size_t p = after;
        int depth = 0;
        while (p < range_end) {
            char c = s[p++];
            if (c == '(') depth++;
            else if (c == ')') {
                depth--;
                if (depth == 0) {
                    out_span->start = r;
                    out_span->end = p;
                    return 1;
                }
            } else if (c == '"' || c == '\'') {
                char q = c;
                while (p < range_end) {
                    char d = s[p++];
                    if (d == '\\' && p < range_end) { p++; continue; }
                    if (d == q) break;
                }
            }
        }
        return 0;
    }
    return 0;
}

static int cc__rewrite_ufcs_spans_with_nodes(const CCASTRoot* root,
                                             const CCVisitorCtx* ctx,
                                             const char* in_src,
                                             size_t in_len,
                                             char** out_src,
                                             size_t* out_len) {
    if (!root || !ctx || !ctx->input_path || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    struct NodeView {
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
    };
    const struct NodeView* n = (const struct NodeView*)root->nodes;

    /* Collect UFCS call nodes (line spans + method), then rewrite each span in-place. */
    struct UFCSNode {
        int line_start;
        int line_end;
        int col_start;
        int col_end;
        const char* method;
        int occurrence_1based;
    };
    struct UFCSNode* nodes = NULL;
    int node_count = 0;
    int node_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue;         /* CALL */
        if (!n[i].aux_s1) continue;           /* only UFCS calls */
        if (!cc__same_source_file(ctx->input_path, n[i].file) &&
            !(root->lowered_path && cc__same_source_file(root->lowered_path, n[i].file)))
            continue;
        int ls = n[i].line_start;
        int le = n[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        if (node_count == node_cap) {
            node_cap = node_cap ? node_cap * 2 : 32;
            nodes = (struct UFCSNode*)realloc(nodes, (size_t)node_cap * sizeof(*nodes));
            if (!nodes) return 0;
        }
        int occ = (n[i].aux2 >> 8) & 0x00ffffff;
        if (occ <= 0) occ = 1;
        nodes[node_count++] = (struct UFCSNode){
            .line_start = ls,
            .line_end = le,
            .col_start = n[i].col_start,
            .col_end = n[i].col_end,
            .method = n[i].aux_s1,
            .occurrence_1based = occ,
        };
    }

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) { free(nodes); return 0; }
    memcpy(cur, in_src, in_len);
    cur[in_len] = '\0';
    size_t cur_len = in_len;

    /* Sort nodes by decreasing span length so outer rewrites happen before inner,
       then by increasing start line for determinism. */
    for (int i = 0; i < node_count; i++) {
        for (int j = i + 1; j < node_count; j++) {
            int li = nodes[i].line_end - nodes[i].line_start;
            int lj = nodes[j].line_end - nodes[j].line_start;
            int swap = 0;
            if (lj > li) swap = 1;
            else if (lj == li && nodes[j].line_start < nodes[i].line_start) swap = 1;
            if (swap) {
                struct UFCSNode tmp = nodes[i];
                nodes[i] = nodes[j];
                nodes[j] = tmp;
            }
        }
    }

    for (int i = 0; i < node_count; i++) {
        int ls = nodes[i].line_start;
        int le = nodes[i].line_end;
        if (ls <= 0) continue;
        if (le < ls) le = ls;
        size_t rs = cc__offset_of_line_1based(cur, cur_len, ls);
        size_t re = (le == ls) ? cc__offset_of_line_1based(cur, cur_len, le + 1) : cc__offset_of_line_1based(cur, cur_len, le + 1);
        if (re > cur_len) re = cur_len;
        if (rs >= re) continue;

        struct CC__UFCSSpan sp;
        if (nodes[i].col_start > 0 && nodes[i].col_end > 0 && nodes[i].line_end > 0) {
            size_t sep_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_start, nodes[i].col_start);
            size_t end_pos = cc__offset_of_line_col_1based(cur, cur_len, nodes[i].line_end, nodes[i].col_end);
            if (!cc__span_from_anchor_and_end(cur, rs, sep_pos, end_pos, &sp))
                continue;
        } else {
            if (!cc__find_ufcs_span_in_range(cur, rs, re, nodes[i].method, nodes[i].occurrence_1based, &sp))
                continue;
        }
        if (sp.end > cur_len || sp.start >= sp.end) continue;

        size_t expr_len = sp.end - sp.start;
        size_t out_cap = expr_len * 2 + 128;
        char* out_buf = (char*)malloc(out_cap);
        if (!out_buf) continue;
        char* expr = (char*)malloc(expr_len + 1);
        if (!expr) { free(out_buf); continue; }
        memcpy(expr, cur + sp.start, expr_len);
        expr[expr_len] = '\0';
        if (cc_ufcs_rewrite_line(expr, out_buf, out_cap) == 0) {
            size_t repl_len = strlen(out_buf);
            size_t new_len = cur_len - expr_len + repl_len;
            char* next = (char*)malloc(new_len + 1);
            if (next) {
                memcpy(next, cur, sp.start);
                memcpy(next + sp.start, out_buf, repl_len);
                memcpy(next + sp.start + repl_len, cur + sp.end, cur_len - sp.end);
                next[new_len] = '\0';
                free(cur);
                cur = next;
                cur_len = new_len;
            }
        }
        free(expr);
        free(out_buf);
    }

    free(nodes);
    *out_src = cur;
    *out_len = cur_len;
    return 1;
}
#endif

#ifdef CC_TCC_EXT_AVAILABLE
static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
}

static int cc__arena_args_for_line(const CCASTRoot* root,
                                   const char* src_path,
                                   int line_no,
                                   const char** out_name,
                                   const char** out_size_expr) {
    if (!root || !root->nodes || root->node_count <= 0 || !src_path || line_no <= 0)
        return 0;
    if (out_name) *out_name = NULL;
    if (out_size_expr) *out_size_expr = NULL;

    struct NodeView {
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
    };
    const struct NodeView* n = (const struct NodeView*)root->nodes;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 4) /* CC_AST_NODE_ARENA */
            continue;
        /* Prefer node file matching against input or lowered temp file. */
        if (!cc__same_source_file(src_path, n[i].file))
            continue;
        if (n[i].line_start != line_no)
            continue;

        if (out_name) *out_name = n[i].aux_s1;
        if (out_size_expr) *out_size_expr = n[i].aux_s2;
        return 1;
    }
    return 0;
}
#endif

#ifdef CC_TCC_EXT_AVAILABLE
static int cc__stmt_for_line(const CCASTRoot* root,
                             const CCVisitorCtx* ctx,
                             const char* src_path,
                             int line_no,
                             const char** out_kind,
                             int* out_end_line) {
    if (!root || !root->nodes || root->node_count <= 0 || !src_path || line_no <= 0)
        return 0;
    if (out_kind) *out_kind = NULL;
    if (out_end_line) *out_end_line = 0;

    struct NodeView {
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
    };
    const struct NodeView* n = (const struct NodeView*)root->nodes;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 3) /* CC_AST_NODE_STMT */
            continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file))
            continue;
        if (n[i].line_start != line_no)
            continue;
        if (out_kind) *out_kind = n[i].aux_s1;
        if (out_end_line) *out_end_line = n[i].line_end;
        return 1;
    }
    return 0;
}
#endif

int cc_visit(const CCASTRoot* root, CCVisitorCtx* ctx, const char* output_path) {
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
                        "  [%d] kind=%d parent=%d file=%s lines=%d..%d aux1=%d aux2=%d aux_s1=%s aux_s2=%s\n",
                        i,
                        n[i].kind,
                        n[i].parent,
                        n[i].file ? n[i].file : "<null>",
                        n[i].line_start,
                        n[i].line_end,
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
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_all && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_ufcs_spans_with_nodes(root, ctx, src_all, src_len, &rewritten, &rewritten_len)) {
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include \"cc_nursery.cch\"\n");
    fprintf(out, "#include \"cc_closure.cch\"\n");
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

    /* Pre-scan for spawn closures so we can emit valid top-level thunk defs. */
    CCClosureDesc* closure_descs = NULL;
    int closure_count = 0;
    int* closure_line_map = NULL; /* 1-based line -> (index+1) */
    int closure_line_cap = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;
    if (src_ufcs) {
        cc__scan_spawn_closures(src_ufcs, src_ufcs_len, src_path,
                                &closure_descs, &closure_count,
                                &closure_line_map, &closure_line_cap,
                                &closure_defs, &closure_defs_len);
    }
    if (closure_defs && closure_defs_len > 0) {
        fputs("/* --- CC generated closures --- */\n", out);
        fwrite(closure_defs, 1, closure_defs_len, out);
        fputs("/* --- end generated closures --- */\n\n", out);
    }

    /* Preserve diagnostics mapping to the original input where possible. */
    fprintf(out, "#line 1 \"%s\"\n", src_path);

    if (src_ufcs) {
        FILE* in = fmemopen(src_ufcs, src_ufcs_len, "r");
        if (!in) {
            /* Fallback to old path */
            in = fopen(ctx->input_path, "r");
        }
        /* Map of multiline UFCS call spans: start_line -> end_line (inclusive). */
        int* ufcs_ml_end = NULL;
        int ufcs_ml_cap = 0;
        unsigned char* ufcs_single = NULL;
        int ufcs_single_cap = 0;
        if (root && root->nodes && root->node_count > 0) {
            struct NodeView {
                int kind;
                int parent;
                const char* file;
                int line_start;
                int line_end;
                int aux1;
                int aux2;
                const char* aux_s1;
                const char* aux_s2;
            };
            const struct NodeView* n = (const struct NodeView*)root->nodes;
            int max_start = 0;
            for (int i = 0; i < root->node_count; i++) {
                if (n[i].kind != 5) continue;          /* CALL */
                if (!n[i].aux_s1) continue;            /* only UFCS-marked calls */
                if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                if (n[i].line_end > n[i].line_start && n[i].line_start > max_start)
                    max_start = n[i].line_start;
                if (n[i].line_start > ufcs_single_cap)
                    ufcs_single_cap = n[i].line_start;
            }
            if (max_start > 0) {
                ufcs_ml_cap = max_start + 1;
                ufcs_ml_end = (int*)calloc((size_t)ufcs_ml_cap, sizeof(int));
                if (ufcs_ml_end) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (n[i].kind != 5) continue;
                        if (!n[i].aux_s1) continue;
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].line_end > n[i].line_start &&
                            n[i].line_start > 0 &&
                            n[i].line_start < ufcs_ml_cap) {
                            int st = n[i].line_start;
                            if (n[i].line_end > ufcs_ml_end[st])
                                ufcs_ml_end[st] = n[i].line_end;
                        }
                    }
                }
            }
            if (ufcs_single_cap > 0) {
                ufcs_single = (unsigned char*)calloc((size_t)ufcs_single_cap + 1, 1);
                if (ufcs_single) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (n[i].kind != 5) continue;
                        if (!n[i].aux_s1) continue;
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].line_start > 0 && n[i].line_start <= ufcs_single_cap)
                            ufcs_single[n[i].line_start] = 1;
                    }
                }
            }
        }

        int arena_stack[128];
        int arena_top = -1;
        int arena_counter = 0;
        int nursery_depth_stack[128];
        int nursery_id_stack[128];
        int nursery_top = -1;
        int nursery_counter = 0;

        /* Basic scope tracking for @defer. This is a line-based best-effort implementation:
           - @defer stmt; registers stmt to run before the closing brace of the current scope.
           - @defer name: stmt; registers a named defer.
           - cancel name; disables a named defer.
           This does NOT support cross-line defers robustly yet, but unblocks correct-ish flow. */
        typedef struct {
            int depth;
            int active;
            int line_no;
            char name[64];   /* empty = unnamed */
            char stmt[512];  /* original stmt suffix */
        } CCDeferItem;
        CCDeferItem defers[512];
        int defer_count = 0;

        int brace_depth = 0;
        /* nursery id stack is used for spawn lowering */
        int src_line_no = 0;
        char line[512];
        char rewritten[1024];
        while (fgets(line, sizeof(line), in)) {
            src_line_no++;
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;

            /* cancel <name>; */
            if (strncmp(p, "cancel", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
                char nm[64] = {0};
                if (sscanf(p + 6, " %63[^; \t\r\n]", nm) == 1) {
                    for (int i = defer_count - 1; i >= 0; i--) {
                        if (defers[i].active && defers[i].name[0] && strcmp(defers[i].name, nm) == 0) {
                            defers[i].active = 0;
                            break;
                        }
                    }
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "/* TODO: cancel %s; */\n", nm[0] ? nm : "<unknown>");
                continue;
            }

            /* Lower @arena syntax marker into a plain C block. The preprocessor already injected
               the arena binding/free lines inside the block. */
            if (strncmp(p, "@arena", 6) == 0) {
                const char* name_tok = "arena";
                const char* size_tok = "kilobytes(4)";
#ifdef CC_TCC_EXT_AVAILABLE
                const char* rec_name = NULL;
                const char* rec_size = NULL;
                /* Try matching arena node against either input_path or lowered_path. */
                if (cc__arena_args_for_line(root, ctx->input_path, src_line_no, &rec_name, &rec_size) ||
                    (root && root->lowered_path &&
                     cc__arena_args_for_line(root, root->lowered_path, src_line_no, &rec_name, &rec_size))) {
                    if (rec_name && rec_name[0]) name_tok = rec_name;
                    if (rec_size && rec_size[0]) size_tok = rec_size;
                }
#endif

                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                int id = ++arena_counter;
                if (arena_top + 1 < (int)(sizeof(arena_stack) / sizeof(arena_stack[0])))
                    arena_stack[++arena_top] = id;

                /* Map generated prologue to the @arena source line for better diagnostics. */
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "%s{\n", indent);
                fprintf(out, "%s  CCArena __cc_arena%d = cc_heap_arena(%s);\n", indent, id, size_tok);
                fprintf(out, "%s  CCArena* %s = &__cc_arena%d;\n", indent, name_tok, id);
                brace_depth++; /* we emitted an opening brace */
                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                continue;
            }

            /* @defer [name:] stmt; */
            if (strncmp(p, "@defer", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
                char* rest = p + 6;
                while (*rest == ' ' || *rest == '\t') rest++;
                /* Parse optional name: */
                char nm[64] = {0};
                const char* stmt = rest;
                const char* colon = strchr(rest, ':');
                if (colon) {
                    /* treat as name: if name token is identifier-ish and ':' precedes a space */
                    size_t nlen = (size_t)(colon - rest);
                    if (nlen > 0 && nlen < sizeof(nm)) {
                        int ok = 1;
                        for (size_t i = 0; i < nlen; i++) {
                            char c = rest[i];
                            if (!(isalnum((unsigned char)c) || c == '_')) { ok = 0; break; }
                        }
                        if (ok) {
                            memcpy(nm, rest, nlen);
                            nm[nlen] = '\0';
                            stmt = colon + 1;
                            while (*stmt == ' ' || *stmt == '\t') stmt++;
                        }
                    }
                }
                if (defer_count < (int)(sizeof(defers) / sizeof(defers[0]))) {
                    CCDeferItem* d = &defers[defer_count++];
                    d->depth = brace_depth;
                    d->active = 1;
                    d->line_no = src_line_no;
                    d->name[0] = '\0';
                    if (nm[0]) strncpy(d->name, nm, sizeof(d->name) - 1);
                    d->stmt[0] = '\0';
                    strncpy(d->stmt, stmt, sizeof(d->stmt) - 1);
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "/* @defer recorded */\n");
                continue;
            }

            /* Lower @nursery marker into a runtime nursery scope. */
            if (strncmp(p, "@nursery", 8) == 0 && (p[8] == ' ' || p[8] == '\t' || p[8] == '\n' || p[8] == '\r' || p[8] == '{')) {
                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                int id = ++nursery_counter;
                if (nursery_top + 1 < (int)(sizeof(nursery_id_stack) / sizeof(nursery_id_stack[0]))) {
                    nursery_id_stack[++nursery_top] = id;
                    /* Will be set after we account for the '{' we emit below. */
                    nursery_depth_stack[nursery_top] = 0;
                }
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                /* Declare nursery in the surrounding scope, then emit a plain C block for the nursery body.
                   This keeps the nursery pointer in-scope even if epilogues are emitted later (best-effort). */
                fprintf(out, "%sCCNursery* __cc_nursery%d = cc_nursery_create();\n", indent, id);
                fprintf(out, "%sif (!__cc_nursery%d) abort();\n", indent, id);
                fprintf(out, "%s{\n", indent);
                brace_depth++; /* account for the '{' we emitted */
                if (nursery_top >= 0) nursery_depth_stack[nursery_top] = brace_depth;
                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                continue;
            }

            /* Lower spawn(...) inside a nursery to cc_nursery_spawn. Supports:
               - spawn (fn());
               - spawn (fn(<int literal>));
               Otherwise falls back to a plain call with a TODO. */
            if (strncmp(p, "spawn", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
                int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
                const char* s0 = p + 5;
                while (*s0 == ' ' || *s0 == '\t') s0++;
                if (*s0 == '(') {
                    s0++;
                    while (*s0 == ' ' || *s0 == '\t') s0++;

                    /* Closure literal: spawn(() => { ... }); uses pre-scan + top-level thunks. */
                    if (closure_line_map && src_line_no > 0 && src_line_no < closure_line_cap) {
                        int idx1 = closure_line_map[src_line_no];
                        if (idx1 > 0 && idx1 <= closure_count) {
                            CCClosureDesc* cd = &closure_descs[idx1 - 1];
                            fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                            fprintf(out, "{\n");
                            if (cd->cap_count > 0) {
                                fprintf(out, "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)malloc(sizeof(__cc_closure_env_%d));\n", cd->id, cd->id, cd->id);
                                fprintf(out, "  if (!__env) abort();\n");
                                for (int ci = 0; ci < cd->cap_count; ci++) {
                                    fprintf(out, "  __env->%s = %s;\n", cd->cap_names[ci], cd->cap_names[ci]);
                                }
                                fprintf(out, "  CCClosure0 __c = cc_closure0_make(__cc_closure_entry_%d, __env, __cc_closure_drop_%d);\n", cd->id, cd->id);
                            } else {
                                fprintf(out, "  CCClosure0 __c = cc_closure0_make(__cc_closure_entry_%d, NULL, NULL);\n", cd->id);
                            }
                            fprintf(out, "  cc_nursery_spawn_closure0(__cc_nursery%d, __c);\n", cur_nursery_id);
                            fprintf(out, "}\n");
                            /* Skip original closure text lines (multiline). */
                            while (src_line_no < cd->end_line && fgets(line, sizeof(line), in)) {
                                src_line_no++;
                            }
                            /* Resync source mapping after eliding original closure text. */
                            fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                            continue;
                        }
                    }

                    char fn[64] = {0};
                    long arg = 0;
                    int has_arg = 0;
                    if (sscanf(s0, "%63[_A-Za-z0-9]%n", fn, &(int){0}) >= 1) {
                        const char* lp = strchr(s0, '(');
                        const char* rp = lp ? strchr(lp, ')') : NULL;
                        if (lp && rp && lp < rp) {
                            /* check for single integer literal inside */
                            const char* inside = lp + 1;
                            while (*inside == ' ' || *inside == '\t') inside++;
                            if (*inside == '-' || isdigit((unsigned char)*inside)) {
                                char* endp = NULL;
                                arg = strtol(inside, &endp, 10);
                                if (endp) {
                                    while (*endp == ' ' || *endp == '\t') endp++;
                                    if (*endp == ')' ) has_arg = 1;
                                }
                            }
                            /* no-arg case */
                            if (!has_arg) {
                                const char* inside2 = lp + 1;
                                while (*inside2 == ' ' || *inside2 == '\t') inside2++;
                                if (*inside2 == ')' ) has_arg = 0;
                            }
                        }
                    }

                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                    if (cur_nursery_id == 0) {
                        fprintf(out, "/* TODO: spawn outside nursery */ %s", line);
                        continue;
                    }
                    if (fn[0] && !has_arg) {
                        fprintf(out, "{ __cc_spawn_void_arg* __a = (__cc_spawn_void_arg*)malloc(sizeof(__cc_spawn_void_arg));\n");
                        fprintf(out, "  if (!__a) abort();\n");
                        fprintf(out, "  __a->fn = %s;\n", fn);
                        fprintf(out, "  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_void, __a);\n", cur_nursery_id);
                        fprintf(out, "}\n");
                        continue;
                    }
                    if (fn[0] && has_arg) {
                        fprintf(out, "{ __cc_spawn_int_arg* __a = (__cc_spawn_int_arg*)malloc(sizeof(__cc_spawn_int_arg));\n");
                        fprintf(out, "  if (!__a) abort();\n");
                        fprintf(out, "  __a->fn = %s;\n", fn);
                        fprintf(out, "  __a->arg = (int)%ld;\n", arg);
                        fprintf(out, "  cc_nursery_spawn(__cc_nursery%d, __cc_spawn_thunk_int, __a);\n", cur_nursery_id);
                        fprintf(out, "}\n");
                        continue;
                    }
                    fprintf(out, "/* TODO: spawn lowering */ %s", line);
                    continue;
                }
            }
            if (arena_top >= 0 && p[0] == '}') {
                int id = arena_stack[arena_top--];
                size_t indent_len = (size_t)(p - line);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line, indent_len);
                indent[indent_len] = '\0';
                /* Map generated epilogue to the closing brace line for diagnostics. */
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                fprintf(out, "%s  cc_heap_arena_free(&__cc_arena%d);\n", indent, id);
                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
            }

            /* Before emitting a close brace, emit any @defer statements at this depth. */
            if (p[0] == '}') {
                /* If this brace closes an active nursery scope, emit nursery epilogue inside the scope. */
                if (nursery_top >= 0 && nursery_depth_stack[nursery_top] == brace_depth) {
                    size_t indent_len = (size_t)(p - line);
                    char indent[256];
                    if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                    memcpy(indent, line, indent_len);
                    indent[indent_len] = '\0';

                    int id = nursery_id_stack[nursery_top--];
                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                    fprintf(out, "%s  cc_nursery_wait(__cc_nursery%d);\n", indent, id);
                    fprintf(out, "%s  cc_nursery_free(__cc_nursery%d);\n", indent, id);
                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                }

                for (int i = defer_count - 1; i >= 0; i--) {
                    if (defers[i].active && defers[i].depth == brace_depth) {
                        fprintf(out, "#line %d \"%s\"\n", defers[i].line_no, src_path);
                        fprintf(out, "%s", defers[i].stmt);
                        /* Ensure newline */
                        size_t sl = strnlen(defers[i].stmt, sizeof(defers[i].stmt));
                        if (sl == 0 || defers[i].stmt[sl - 1] != '\n')
                            fprintf(out, "\n");
                        defers[i].active = 0;
                    }
                }
                /* The source brace closes the current depth. */
                if (brace_depth > 0) brace_depth--;
            }

            /* Update brace depth for opening braces on this line (best-effort). */
            for (char* q = line; *q; q++) {
                if (*q == '{') brace_depth++;
            }

            /* If this line starts a recorded multiline UFCS call, buffer until its end line and
               rewrite the whole chunk (handles multi-line argument lists). */
            if (ufcs_ml_end && src_line_no > 0 && src_line_no < ufcs_ml_cap && ufcs_ml_end[src_line_no] > src_line_no) {
                int end_line = ufcs_ml_end[src_line_no];
                size_t buf_cap = 1024;
                size_t buf_len = 0;
                char* buf = (char*)malloc(buf_cap);
                if (!buf) {
                    fputs(line, out);
                    continue;
                }
                buf[0] = '\0';
                size_t ll = strnlen(line, sizeof(line));
                if (buf_len + ll + 1 > buf_cap) {
                    while (buf_len + ll + 1 > buf_cap) buf_cap *= 2;
                    buf = (char*)realloc(buf, buf_cap);
                    if (!buf) continue;
                }
                memcpy(buf + buf_len, line, ll);
                buf_len += ll;
                buf[buf_len] = '\0';

                while (src_line_no < end_line && fgets(line, sizeof(line), in)) {
                    src_line_no++;
                    ll = strnlen(line, sizeof(line));
                    if (buf_len + ll + 1 > buf_cap) {
                        while (buf_len + ll + 1 > buf_cap) buf_cap *= 2;
                        buf = (char*)realloc(buf, buf_cap);
                        if (!buf) break;
                    }
                    memcpy(buf + buf_len, line, ll);
                    buf_len += ll;
                    buf[buf_len] = '\0';
                }

                size_t out_cap = buf_len * 2 + 128;
                char* out_buf = (char*)malloc(out_cap);
                if (out_buf && cc_ufcs_rewrite_line(buf, out_buf, out_cap) == 0) {
                    fputs(out_buf, out);
                } else {
                    fputs(buf, out);
                }
                free(out_buf);
                free(buf);
                continue;
            }

            /* Single-line UFCS lowering: only on lines where TCC recorded a UFCS-marked call. */
            if (ufcs_single && src_line_no > 0 && src_line_no <= ufcs_single_cap && ufcs_single[src_line_no]) {
                if (cc_ufcs_rewrite_line(line, rewritten, sizeof(rewritten)) != 0) {
                    strncpy(rewritten, line, sizeof(rewritten) - 1);
                    rewritten[sizeof(rewritten) - 1] = '\0';
                }
                fputs(rewritten, out);
            } else {
                fputs(line, out);
            }
        }
        free(ufcs_ml_end);
        free(ufcs_single);
        fclose(in);
        if (closure_descs) {
            for (int i = 0; i < closure_count; i++) {
                for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_names[j]);
                free(closure_descs[i].cap_names);
                free(closure_descs[i].body);
            }
            free(closure_descs);
        }
        free(closure_line_map);
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

