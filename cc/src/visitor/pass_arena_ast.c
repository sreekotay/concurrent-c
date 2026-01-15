#include "pass_arena_ast.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CC_TCC_EXT_AVAILABLE
#error "CC_TCC_EXT_AVAILABLE is required (patched TCC stub-AST required)."
#endif

/* Stub AST kinds from patched TCC (see third_party/tcc/tcc.h). */
enum { CC_AST_NODE_ARENA = 4, CC_AST_NODE_AWAIT = 6 };

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
} NodeView;

static const char* cc__basename(const char* path) {
    if (!path) return NULL;
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

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
    const char* a_suf = cc__path_suffix2(a);
    const char* b_suf = cc__path_suffix2(b);
    if (a_suf && b_suf && strcmp(a_suf, b_suf) == 0) return 1;
    return 1;
}

static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                        const CCVisitorCtx* ctx,
                                        const char* node_file) {
    if (!ctx || !ctx->input_path || !node_file) return 0;
    if (cc__same_source_file(ctx->input_path, node_file)) return 1;
    if (root && root->lowered_path && cc__same_source_file(root->lowered_path, node_file)) return 1;
    return 0;
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

static void cc__append_n(char** out, size_t* out_len, size_t* out_cap, const char* s, size_t n) {
    if (!out || !out_len || !out_cap || !s) return;
    if (*out_len + n + 1 > *out_cap) {
        size_t nc = *out_cap ? *out_cap * 2 : 1024;
        while (nc < *out_len + n + 1) nc *= 2;
        char* nb = (char*)realloc(*out, nc);
        if (!nb) return;
        *out = nb;
        *out_cap = nc;
    }
    memcpy(*out + *out_len, s, n);
    *out_len += n;
    (*out)[*out_len] = 0;
}

static void cc__append_str(char** out, size_t* out_len, size_t* out_cap, const char* s) {
    if (!s) return;
    cc__append_n(out, out_len, out_cap, s, strlen(s));
}

static int cc__find_substr_in_range(const char* s,
                                   size_t start,
                                   size_t end,
                                   const char* needle,
                                   size_t needle_len,
                                   size_t* out_pos) {
    if (!s || !needle || needle_len == 0) return 0;
    if (start > end) return 0;
    if (end - start < needle_len) return 0;
    for (size_t i = start; i + needle_len <= end; i++) {
        if (memcmp(s + i, needle, needle_len) == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

static size_t cc__find_matching_rbrace(const char* s, size_t len, size_t lbrace_off, size_t scan_end) {
    if (!s || lbrace_off >= len) return (size_t)-1;
    if (scan_end > len) scan_end = len;
    int br = 0;
    int in_str = 0;
    char qch = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    for (size_t i = lbrace_off; i < scan_end; i++) {
        char ch = s[i];
        if (in_line_comment) {
            if (ch == '\n') in_line_comment = 0;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && i + 1 < scan_end && s[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (ch == '\\' && i + 1 < scan_end) { i++; continue; }
            if (ch == qch) in_str = 0;
            continue;
        }
        if (ch == '/' && i + 1 < scan_end && s[i + 1] == '/') { in_line_comment = 1; i++; continue; }
        if (ch == '/' && i + 1 < scan_end && s[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        if (ch == '"' || ch == '\'') { in_str = 1; qch = ch; continue; }

        if (ch == '{') br++;
        else if (ch == '}') {
            br--;
            if (br == 0) return i;
        }
    }
    return (size_t)-1;
}

typedef struct {
    size_t start_off;  /* @arena start */
    size_t brace_off;  /* '{' position */
    size_t close_off;  /* matching '}' position */
    const char* name;
    const char* size_expr;
    int id;
    int node_idx;      /* index into stub-AST nodes */
    size_t indent_off;
    size_t indent_len;
} ArenaEdit;

int cc__rewrite_arena_blocks_with_nodes(const CCASTRoot* root,
                                       const CCVisitorCtx* ctx,
                                       const char* in_src,
                                       size_t in_len,
                                       char** out_src,
                                       size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const NodeView* n = (const NodeView*)root->nodes;

    ArenaEdit edits[256];
    int edit_n = 0;
    int next_id = 1;

    for (int i = 0; i < root->node_count && edit_n < (int)(sizeof(edits) / sizeof(edits[0])); i++) {
        if (n[i].kind != CC_AST_NODE_ARENA) continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
        if (n[i].line_start <= 0) continue;
        if (n[i].line_end <= 0) continue;

        size_t span_start = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t span_end = cc__offset_of_line_1based(in_src, in_len, n[i].line_end + 1);
        if (span_end > in_len) span_end = in_len;
        if (span_start >= in_len) continue;

        size_t start = 0;
        if (n[i].col_start > 0) start = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        else {
            /* Support both @arena(...) and @arena_init(...). */
            if (!cc__find_substr_in_range(in_src, span_start, span_end, "@arena_init", 10, &start) &&
                !cc__find_substr_in_range(in_src, span_start, span_end, "@arena", 6, &start)) continue;
        }

        size_t end = 0;
        if (n[i].col_end > 0) end = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        else end = span_end;
        if (start >= in_len) continue;
        if (end > in_len) end = in_len;
        if (end <= start) continue;

        /* Find the opening '{' within this arena span. */
        size_t brace = (size_t)-1;
        for (size_t p = start; p < end; p++) {
            if (in_src[p] == '{') { brace = p; break; }
        }
        if (brace == (size_t)-1) continue;

        /* Find the matching '}' for this arena block. */
        size_t close = cc__find_matching_rbrace(in_src, in_len, brace, end);
        /* Some stub-AST nodes (notably @arena_init) may have an imprecise line_end; fall back to
           scanning the remainder of the file to find the matching brace. */
        if (close == (size_t)-1) {
            close = cc__find_matching_rbrace(in_src, in_len, brace, in_len);
        }
        if (close == (size_t)-1 || close <= brace) continue;

        /* Indent = whitespace from line start to first non-ws. */
        size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t ind = line_off;
        while (ind < in_len && (in_src[ind] == ' ' || in_src[ind] == '\t')) ind++;

        edits[edit_n++] = (ArenaEdit){
            .start_off = start,
            .brace_off = brace,
            .close_off = close,
            .name = (n[i].aux_s1 && n[i].aux_s1[0]) ? n[i].aux_s1 : "arena",
            .size_expr = (n[i].aux_s2 && n[i].aux_s2[0]) ? n[i].aux_s2 : "kilobytes(4)",
            .id = next_id++,
            .node_idx = i,
            .indent_off = line_off,
            .indent_len = (ind > line_off) ? (ind - line_off) : 0,
        };
    }

    if (edit_n == 0) return 0;

    /* Hard error: @arena_init(buf, size) uses a user-provided backing buffer. If the block contains
       an `await`, the buffer may not remain valid across suspension (especially if stack-backed).
       Reject to avoid miscompiles/UB. */
    for (int ei = 0; ei < edit_n; ei++) {
        const ArenaEdit* e = &edits[ei];
        if (!(e->size_expr && strncmp(e->size_expr, "@buf:", 5) == 0)) continue;

        for (int j = 0; j < root->node_count; j++) {
            if (n[j].kind != CC_AST_NODE_AWAIT) continue;
            if (!cc__node_file_matches_this_tu(root, ctx, n[j].file)) continue;
            if (n[j].line_start <= 0) continue;

            size_t aw_off = (n[j].col_start > 0)
                                ? cc__offset_of_line_col_1based(in_src, in_len, n[j].line_start, n[j].col_start)
                                : cc__offset_of_line_1based(in_src, in_len, n[j].line_start);
            if (aw_off > in_len) aw_off = in_len;

            if (aw_off > e->brace_off && aw_off < e->close_off) {
                int col1 = n[e->node_idx].col_start > 0 ? n[e->node_idx].col_start : 1;
                int aw_col = n[j].col_start > 0 ? n[j].col_start : 1;
                fprintf(stderr,
                        "%s:%d:%d: error: CC: @arena_init(buf, size) block cannot contain 'await' (backing buffer may not be valid across suspension)\n"
                        "%s:%d:%d: note: 'await' occurs here\n"
                        "help: use @arena(name, size) for a heap-backed arena, or allocate the backing buffer on the heap and ensure it outlives all awaits\n",
                        ctx && ctx->input_path ? ctx->input_path : "<input>",
                        n[e->node_idx].line_start > 0 ? n[e->node_idx].line_start : 1,
                        col1,
                        ctx && ctx->input_path ? ctx->input_path : "<input>",
                        n[j].line_start,
                        aw_col);
                return -1;
            }
        }
    }

    /* Apply edits from last to first to keep offsets valid. */
    for (int i = 0; i < edit_n - 1; i++) {
        for (int j = i + 1; j < edit_n; j++) {
            if (edits[j].start_off > edits[i].start_off) {
                ArenaEdit t = edits[i];
                edits[i] = edits[j];
                edits[j] = t;
            }
        }
    }

    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return 0;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t cur_len = in_len;

    for (int ei = 0; ei < edit_n; ei++) {
        ArenaEdit e = edits[ei];
        if (e.close_off >= cur_len || e.brace_off >= cur_len || e.start_off >= cur_len) continue;
        if (!(e.start_off < e.brace_off && e.brace_off < e.close_off)) continue;

        const char* indent = cur + e.indent_off;
        size_t indent_len = e.indent_len;
        if (e.indent_off + indent_len > cur_len) { indent = ""; indent_len = 0; }

        char pro[512];
        char epi[256];
        int pn = 0;
        int en = 0;

        /* Check for @buf: prefix indicating 3-arg form: @arena(name, buf, size) */
        if (e.size_expr && strncmp(e.size_expr, "@buf:", 5) == 0) {
            /* Parse "buf_expr;size_expr" after the "@buf:" prefix */
            const char* rest = e.size_expr + 5;
            const char* semi = strchr(rest, ';');
            if (!semi) continue;

            char buf_expr[256];
            char size_expr[256];
            size_t buf_len = (size_t)(semi - rest);
            if (buf_len >= sizeof(buf_expr)) buf_len = sizeof(buf_expr) - 1;
            memcpy(buf_expr, rest, buf_len);
            buf_expr[buf_len] = 0;

            size_t size_len = strlen(semi + 1);
            if (size_len >= sizeof(size_expr)) size_len = sizeof(size_expr) - 1;
            memcpy(size_expr, semi + 1, size_len);
            size_expr[size_len] = 0;

            /* Stack-allocate the arena object, initialize with user's buffer */
            pn = snprintf(pro, sizeof(pro),
                          "%.*s{\n"
                          "%.*s  CCArena __cc_arena%d_obj;\n"
                          "%.*s  if (cc_arena_init(&__cc_arena%d_obj, %s, %s) != 0) abort();\n"
                          "%.*s  CCArena* %s = &__cc_arena%d_obj;\n",
                          (int)indent_len, indent,
                          (int)indent_len, indent, e.id,
                          (int)indent_len, indent, e.id, buf_expr, size_expr,
                          (int)indent_len, indent, e.name, e.id);

            /* No cleanup needed - arena uses user's buffer, not heap */
            en = snprintf(epi, sizeof(epi),
                          "%.*s  /* arena %s uses user buffer - no cleanup */\n",
                          (int)indent_len, indent, e.name);
        } else {
            /* Heap-allocate the arena object so it can be safely referenced across @async suspension
               (the pointer can be hoisted into the async frame). */
            pn = snprintf(pro, sizeof(pro),
                          "%.*s{\n"
                          "%.*s  CCArena* __cc_arena%d = (CCArena*)malloc(sizeof(CCArena));\n"
                          "%.*s  if (!__cc_arena%d) abort();\n"
                          "%.*s  *__cc_arena%d = cc_heap_arena(%s);\n"
                          "%.*s  CCArena* %s = __cc_arena%d;\n",
                          (int)indent_len, indent,
                          (int)indent_len, indent, e.id,
                          (int)indent_len, indent, e.id,
                          (int)indent_len, indent, e.id, e.size_expr,
                          (int)indent_len, indent, e.name, e.id);

            en = snprintf(epi, sizeof(epi),
                          "%.*s  cc_heap_arena_free(__cc_arena%d);\n"
                          "%.*s  free(__cc_arena%d);\n",
                          (int)indent_len, indent, e.id,
                          (int)indent_len, indent, e.id);
        }
        if (pn <= 0 || (size_t)pn >= sizeof(pro)) continue;
        if (en <= 0 || (size_t)en >= sizeof(epi)) continue;

        /* Replace [start_off, brace_off+1) with prologue. */
        size_t old_pre_len = (e.brace_off + 1) - e.start_off;
        size_t new_pre_len = (size_t)pn;
        size_t delta_pre = new_pre_len - old_pre_len;

        /* Insert epilogue before close_off. */
        size_t new_len = cur_len + delta_pre + (size_t)en;
        char* next = (char*)malloc(new_len + 1);
        if (!next) continue;

        /* Prefix before start_off */
        memcpy(next, cur, e.start_off);
        size_t w = e.start_off;
        memcpy(next + w, pro, (size_t)pn);
        w += (size_t)pn;

        /* Middle from brace_off+1 .. close_off */
        memcpy(next + w, cur + (e.brace_off + 1), e.close_off - (e.brace_off + 1));
        w += e.close_off - (e.brace_off + 1);

        /* Epilogue */
        memcpy(next + w, epi, (size_t)en);
        w += (size_t)en;

        /* Suffix from close_off .. end */
        memcpy(next + w, cur + e.close_off, cur_len - e.close_off);
        w += cur_len - e.close_off;
        next[w] = 0;

        free(cur);
        cur = next;
        cur_len = w;
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}

