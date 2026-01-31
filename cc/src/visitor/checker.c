#include "checker.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "util/path.h"
#include "util/text.h"

// Slice flag tracking scaffold. As the parser starts emitting CC AST nodes,
// populate flags on expressions and enforce send_take eligibility.

typedef enum {
    CC_SLICE_UNKNOWN = 0,
    CC_SLICE_UNIQUE = 1 << 0,
    CC_SLICE_TRANSFERABLE = 1 << 1,
    CC_SLICE_SUBSLICE = 1 << 2,
} CCSliceFlags;

typedef struct {
    const char* name; /* borrowed */
    int is_slice;
    int is_array;
    int is_stack_slice_view;
    int move_only;
    int moved;
    int pending_move;
    int decl_line;
    int decl_col;
} CCSliceVar;

typedef struct {
    CCSliceVar* vars;
    int vars_len;
    int vars_cap;
} CCScope;

static int cc__same_source_file(const char* a, const char* b) {
    if (!a || !b) return 0;
    if (strcmp(a, b) == 0) return 1;
    /* Prefer repo-relative normalization when possible. */
    {
        char ra[1024], rb[1024];
        const char* pa = cc_path_rel_to_repo(a, ra, sizeof(ra));
        const char* pb = cc_path_rel_to_repo(b, rb, sizeof(rb));
        if (pa && pb && strcmp(pa, pb) == 0) return 1;
    }
    /* Fallback: basename match (best-effort). */
    const char* a_base = a;
    const char* b_base = b;
    for (const char* p = a; *p; p++) if (*p == '/' || *p == '\\') a_base = p + 1;
    for (const char* p = b; *p; p++) if (*p == '/' || *p == '\\') b_base = p + 1;
    return (a_base && b_base && strcmp(a_base, b_base) == 0);
}

static CCSliceVar* cc__scope_find(CCScope* sc, const char* name) {
    if (!sc || !name) return NULL;
    for (int i = 0; i < sc->vars_len; i++) {
        if (sc->vars[i].name && strcmp(sc->vars[i].name, name) == 0)
            return &sc->vars[i];
    }
    return NULL;
}

static CCSliceVar* cc__scopes_lookup(CCScope* scopes, int n, const char* name) {
    for (int i = n - 1; i >= 0; i--) {
        CCSliceVar* v = cc__scope_find(&scopes[i], name);
        if (v) return v;
    }
    return NULL;
}

static CCSliceVar* cc__scope_add(CCScope* sc, const char* name) {
    if (!sc || !name) return NULL;
    CCSliceVar* ex = cc__scope_find(sc, name);
    if (ex) return ex;
    if (sc->vars_len == sc->vars_cap) {
        int nc = sc->vars_cap ? sc->vars_cap * 2 : 32;
        CCSliceVar* nv = (CCSliceVar*)realloc(sc->vars, (size_t)nc * sizeof(CCSliceVar));
        if (!nv) return NULL;
        sc->vars = nv;
        sc->vars_cap = nc;
    }
    CCSliceVar* v = &sc->vars[sc->vars_len++];
    memset(v, 0, sizeof(*v));
    v->name = name;
    return v;
}

static void cc__scope_free(CCScope* sc) {
    if (!sc) return;
    free(sc->vars);
    sc->vars = NULL;
    sc->vars_len = 0;
    sc->vars_cap = 0;
}

static void cc__commit_pending_moves(CCScope* scopes, int scope_n) {
    if (!scopes || scope_n <= 0) return;
    for (int i = 0; i < scope_n; i++) {
        CCScope* sc = &scopes[i];
        for (int j = 0; j < sc->vars_len; j++) {
            if (sc->vars[j].pending_move) {
                sc->vars[j].moved = 1;
                sc->vars[j].pending_move = 0;
            }
        }
    }
}

typedef struct StubNodeView {
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
} StubNodeView;

enum {
    CC_STUB_DECL = 1,
    CC_STUB_BLOCK = 2,
    CC_STUB_STMT = 3,
    CC_STUB_ARENA = 4,
    CC_STUB_CALL = 5,
    CC_STUB_AWAIT = 6,
    CC_STUB_SEND_TAKE = 7,
    CC_STUB_SUBSLICE = 8,
    CC_STUB_CLOSURE = 9,
    CC_STUB_IDENT = 10,
    CC_STUB_CONST = 11,
    CC_STUB_DECL_ITEM = 12,
    CC_STUB_MEMBER = 13,
    CC_STUB_ASSIGN = 14,
    CC_STUB_RETURN = 15,
    CC_STUB_PARAM = 16,
};

enum {
    CC_FN_ATTR_ASYNC = 1u << 0,
    CC_FN_ATTR_NOBLOCK = 1u << 1,
    CC_FN_ATTR_LATENCY_SENSITIVE = 1u << 2,
};

typedef struct {
    int* child;
    int len;
    int cap;
} ChildList;

static void cc__child_push(ChildList* cl, int idx) {
    if (!cl) return;
    if (cl->len == cl->cap) {
        int nc = cl->cap ? cl->cap * 2 : 8;
        int* nv = (int*)realloc(cl->child, (size_t)nc * sizeof(int));
        if (!nv) return;
        cl->child = nv;
        cl->cap = nc;
    }
    cl->child[cl->len++] = idx;
}

static void cc__emit_err(const CCCheckerCtx* ctx, const StubNodeView* n, const char* msg) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: error: %s\n", path, line, col, msg);
}

static void cc__emit_err_fmt(const CCCheckerCtx* ctx, const StubNodeView* n, const char* fmt, ...) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: error: ", path, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void cc__emit_warn(const CCCheckerCtx* ctx, const StubNodeView* n, const char* msg) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: warning: %s\n", path, line, col, msg);
}

static void cc__emit_note(const CCCheckerCtx* ctx, const StubNodeView* n, const char* msg) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: note: %s\n", path, line, col, msg);
}

static void cc__emit_note_fmt(const CCCheckerCtx* ctx, const StubNodeView* n, const char* fmt, ...) {
    const char* path = (ctx && ctx->input_path) ? ctx->input_path : (n && n->file ? n->file : "<src>");
    int line = n ? n->line_start : 0;
    int col = n ? n->col_start : 0;
    if (col <= 0) col = 1;
    fprintf(stderr, "%s:%d:%d: note: ", path, line, col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static int cc__deadlock_warn_as_error(void) {
    const char* s = getenv("CC_STRICT_DEADLOCK");
    return s && s[0] == '1';
}

/* Local aliases for the shared helpers */
#define cc__is_ident_start_ch cc_is_ident_start
#define cc__is_ident_ch cc_is_ident_char

static int cc__line_has_deadlock_recv_until_close(const char* line, size_t len, const char* chname) {
    if (!line || len == 0 || !chname || !chname[0]) return 0;

    /* Cheap filters. */
    if (!memmem(line, len, "while", 5)) return 0;
    /* Accept either the raw runtime API or the ergonomic macro form. */
    if (!memmem(line, len, "cc_chan_recv", 12) && !memmem(line, len, "chan_recv", 9)) return 0;
    if (!memmem(line, len, chname, strlen(chname))) return 0;

    /* Normalize by removing whitespace so we can match many formatting variants. */
    char tmp[1024];
    size_t tn = 0;
    size_t cap = sizeof(tmp) - 1;
    for (size_t i = 0; i < len && tn < cap; i++) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        tmp[tn++] = c;
    }
    tmp[tn] = '\0';

    /* Catch (old errno-style API):
       - while(cc_chan_recv(ch,...)==0)
       - while((cc_chan_recv(ch,...)==0))
       - while(!cc_chan_recv(ch,...))
       - while(chan_recv(ch,...)==0)
       - while(!chan_recv(ch,...))
       
       Catch (new bool !>(E) API):
       - while(cc_io_avail(chan_recv(ch,...)))
       - while(cc_io_avail(cc_chan_recv(ch,...)))
    */
    char pat1[256];
    snprintf(pat1, sizeof(pat1), "while(cc_chan_recv(%s", chname);
    char pat2[256];
    snprintf(pat2, sizeof(pat2), "while(!cc_chan_recv(%s", chname);
    char pat3[256];
    snprintf(pat3, sizeof(pat3), "while(chan_recv(%s", chname);
    char pat4[256];
    snprintf(pat4, sizeof(pat4), "while(!chan_recv(%s", chname);
    /* New patterns for cc_io_avail */
    char pat5[256];
    snprintf(pat5, sizeof(pat5), "while(cc_io_avail(chan_recv(%s", chname);
    char pat6[256];
    snprintf(pat6, sizeof(pat6), "while(cc_io_avail(cc_chan_recv(%s", chname);
    
    if (strstr(tmp, pat1) && strstr(tmp, "==0")) return 1;
    if (strstr(tmp, pat2)) return 1;
    if (strstr(tmp, pat3) && strstr(tmp, "==0")) return 1;
    if (strstr(tmp, pat4)) return 1;
    /* New patterns */
    if (strstr(tmp, pat5)) return 1;
    if (strstr(tmp, pat6)) return 1;
    return 0;
}

static int cc__body_has_loop_keyword(const char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] == 'w' && i + 4 <= len && memcmp(buf + i, "while", 5) == 0) {
            if ((i == 0 || !cc__is_ident_ch(buf[i - 1])) &&
                (i + 5 >= len || !cc__is_ident_ch(buf[i + 5]))) return 1;
        }
        if (buf[i] == 'f' && i + 2 <= len && memcmp(buf + i, "for", 3) == 0) {
            if ((i == 0 || !cc__is_ident_ch(buf[i - 1])) &&
                (i + 3 >= len || !cc__is_ident_ch(buf[i + 3]))) return 1;
        }
        if (buf[i] == 'd' && i + 1 <= len && memcmp(buf + i, "do", 2) == 0) {
            if ((i == 0 || !cc__is_ident_ch(buf[i - 1])) &&
                (i + 2 >= len || !cc__is_ident_ch(buf[i + 2]))) return 1;
        }
    }
    return 0;
}

static int cc__body_has_await_recv(const char* buf, size_t len, const char* chname, const char* rxname) {
    if (!buf || len == 0) return 0;
    if (!memmem(buf, len, "await", 5)) return 0;
    if (!memmem(buf, len, "recv", 4)) return 0;
    if (!memmem(buf, len, chname, strlen(chname)) &&
        !(rxname && rxname[0] && memmem(buf, len, rxname, strlen(rxname)))) return 0;
    return 1;
}

static void cc__emit_deadlock_diag(const CCCheckerCtx* ctx, const StubNodeView* sn,
                                   const char* warn_msg, const char* note_msg) {
    if (cc__deadlock_warn_as_error()) {
        cc__emit_err(ctx, sn, warn_msg);
        if (note_msg) cc__emit_note(ctx, sn, note_msg);
        if (ctx) ((CCCheckerCtx*)ctx)->errors++;
    } else {
        cc__emit_warn(ctx, sn, warn_msg);
        if (note_msg) cc__emit_note(ctx, sn, note_msg);
        if (ctx) ((CCCheckerCtx*)ctx)->warnings++;
    }
}

static size_t cc__find_block_end_naive(const char* buf, size_t n, size_t start_brace) {
    if (!buf || start_brace >= n || buf[start_brace] != '{') return 0;
    int depth = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    for (size_t i = start_brace; i < n; i++) {
        char c = buf[i];
        if (c == '\n') { in_line_comment = 0; }
        if (in_line_comment) continue;
        if (in_block_comment) {
            if (c == '*' && i + 1 < n && buf[i + 1] == '/') { in_block_comment = 0; i++; }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < n) { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < n) { i++; continue; }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && i + 1 < n) {
            if (buf[i + 1] == '/') { in_line_comment = 1; i++; continue; }
            if (buf[i + 1] == '*') { in_block_comment = 1; i++; continue; }
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }

        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return 0;
}

__attribute__((unused))
static int cc__find_next_spawn_body(const char* buf, size_t n, size_t start,
                                    size_t* out_body_s, size_t* out_body_e) {
    if (!buf || !out_body_s || !out_body_e) return 0;
    for (size_t i = start; i + 4 < n; i++) {
        if (buf[i] == 's' && memcmp(buf + i, "spawn", 5) == 0) {
            if ((i > 0 && cc__is_ident_ch(buf[i - 1])) ||
                (i + 5 < n && cc__is_ident_ch(buf[i + 5]))) {
                continue;
            }
            /* Find the first '{' after spawn(...) */
            size_t j = i + 5;
            while (j < n && buf[j] != '{') j++;
            if (j >= n || buf[j] != '{') continue;
            size_t end = cc__find_block_end_naive(buf, n, j);
            if (end == 0) continue;
            *out_body_s = j;
            *out_body_e = end;
            return 1;
        }
    }
    return 0;
}

static int cc__ends_with(const char* s, const char* suf) {
    if (!s || !suf) return 0;
    size_t sl = strlen(s);
    size_t su = strlen(suf);
    if (su > sl) return 0;
    return memcmp(s + (sl - su), suf, su) == 0;
}

/* NOTE: We removed overly-broad heuristics for cc_block_on inside spawn/nursery.
 * These had too many false positives - cc_block_on is often fine if the task
 * doesn't wait on peers. We rely on runtime deadlock detection for fuzzy cases.
 * 
 * The ONLY compile-time error we keep is the 100% guaranteed deadlock:
 * @nursery closing(ch) + recv-until-close inside the same nursery.
 */

/* Placeholder to keep line numbers stable */
static void cc__check_spawn_block_on_text(CCCheckerCtx* ctx, const char* buf, size_t n) {
    (void)ctx; (void)buf; (void)n;
    /* Removed: too many false positives. Runtime detection handles real deadlocks. */
}

static void cc__check_nursery_block_on_text(CCCheckerCtx* ctx, const char* buf, size_t n) {
    (void)ctx; (void)buf; (void)n;
    /* Removed: too many false positives. Runtime detection handles real deadlocks. */
}


/* Very small heuristic: detect the most common deadlock footgun:
     @nursery closing(ch) { spawn(() => { while (cc_chan_recv(ch, ...) == 0) { ... } }); }
   Under the spec, closing(...) happens after children exit, so "recv until close" inside the same nursery can wait forever. */
static void cc__check_nursery_closing_deadlock_text(CCCheckerCtx* ctx, const char* buf, size_t n) {
    if (!ctx || !ctx->input_path || !buf || n == 0) return;
    if (getenv("CC_ALLOW_NURSERY_CLOSING_DRAIN")) return; /* escape hatch */

    /* NOTE: We removed the broken "fast-path" heuristic that searched for the pattern anywhere
       after the first `@nursery`. It incorrectly flagged nested nursery patterns where the consumer
       is in an outer nursery and `closing(ch)` is on an inner one. We now rely solely on the
       more careful per-nursery scan below. */

    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    int in_chr = 0;
    int line = 1;
    int col = 1;

    for (size_t i = 0; i + 8 < n; i++) {
        char c = buf[i];
        char c2 = (i + 1 < n) ? buf[i + 1] : 0;

        if (in_line_comment) { if (c == '\n') { in_line_comment = 0; line++; col = 1; } else col++; continue; }
        if (in_block_comment) { if (c == '*' && c2 == '/') { in_block_comment = 0; i++; col += 2; } else { if (c == '\n') { line++; col = 1; } else col++; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { i++; col += 2; continue; } if (c == '"') in_str = 0; if (c == '\n') { line++; col = 1; } else col++; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { i++; col += 2; continue; } if (c == '\'') in_chr = 0; if (c == '\n') { line++; col = 1; } else col++; continue; }

        if (c == '/' && c2 == '/') { in_line_comment = 1; i++; col += 2; continue; }
        if (c == '/' && c2 == '*') { in_block_comment = 1; i++; col += 2; continue; }
        if (c == '"') { in_str = 1; col++; continue; }
        if (c == '\'') { in_chr = 1; col++; continue; }

        if (c == '\n') { line++; col = 1; continue; }

        /* Find '@nursery'. "@nursery" is 8 chars: '@' + "nursery" (7 chars). */
        if (c == '@' && i + 8 <= n && memcmp(buf + i + 1, "nursery", 7) == 0) {
            int nur_line = line;
            int nur_col = col;

            size_t j = i + 8;  /* Skip past "@nursery" */
            while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r' || buf[j] == '\n')) j++;
            if (j + 7 >= n) { col++; continue; }
            if (memcmp(buf + j, "closing", 7) != 0) { col++; continue; }
            j += 7;
            while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r' || buf[j] == '\n')) j++;
            if (j >= n || buf[j] != '(') { col++; continue; }
            j++;
            while (j < n && (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\r' || buf[j] == '\n')) j++;
            if (j >= n || !cc__is_ident_start_ch(buf[j])) { col++; continue; }
            size_t s0 = j;
            j++;
            while (j < n && cc__is_ident_ch(buf[j])) j++;
            size_t sl = j - s0;
            if (sl == 0 || sl >= 64) { col++; continue; }
            char chname[64];
            memcpy(chname, buf + s0, sl);
            chname[sl] = '\0';

            /* Heuristic upgrade for tx/rx naming: if the closing handle ends with `_tx`,
               also scan for recv-until-close loops on the corresponding `_rx` name.
               This keeps the heuristic useful even when closing(...) accepts only tx handles. */
            char rxname[64];
            rxname[0] = '\0';
            if (cc__ends_with(chname, "_tx")) {
                size_t bl = strlen(chname);
                if (bl >= 3 && bl < sizeof(rxname)) {
                    memcpy(rxname, chname, bl);
                    rxname[bl - 3] = '\0';
                    strncat(rxname, "_rx", sizeof(rxname) - strlen(rxname) - 1);
                }
            }

            /* Find '{' for nursery body. */
            while (j < n && buf[j] != '{') j++;
            if (j >= n) { col++; continue; }
            size_t body_s = j;

            /* Find matching '}' (naive depth, ignores strings/comments; good enough for our limited heuristic). */
            int depth = 0;
            size_t k = body_s;
            for (; k < n; k++) {
                if (buf[k] == '{') depth++;
                else if (buf[k] == '}') {
                    depth--;
                    if (depth == 0) { k++; break; }
                }
            }
            size_t body_e = k;
            if (body_e <= body_s || body_e > n) { col++; continue; }

            /* If user explicitly closes the channel in the nursery, don't flag. */
            {
                char pat[96];
                snprintf(pat, sizeof(pat), "cc_chan_close(%s", chname);
                const char* hit = strstr(buf + body_s, pat);
                if (hit && (size_t)(hit - (buf + body_s)) < (body_e - body_s)) { col++; continue; }
                if (rxname[0]) {
                    snprintf(pat, sizeof(pat), "cc_chan_close(%s", rxname);
                    hit = strstr(buf + body_s, pat);
                    if (hit && (size_t)(hit - (buf + body_s)) < (body_e - body_s)) { col++; continue; }
                }
            }

            /* Hard-error only on the direct footgun form (catches the common case). */
            {
                int hit = 0;
                const char* cur = buf + body_s;
                const char* end = buf + body_e;
                while (cur < end) {
                    const char* nl = memchr(cur, '\n', (size_t)(end - cur));
                    size_t ll = nl ? (size_t)(nl - cur) : (size_t)(end - cur);
                    if (cc__line_has_deadlock_recv_until_close(cur, ll, chname)) { hit = 1; break; }
                    if (!hit && rxname[0] && cc__line_has_deadlock_recv_until_close(cur, ll, rxname)) { hit = 1; break; }
                    if (!nl) break;
                    cur = nl + 1;
                }
                if (hit) {
                    StubNodeView sn;
                    memset(&sn, 0, sizeof(sn));
                    sn.file = ctx->input_path;
                    sn.line_start = nur_line;
                    sn.col_start = nur_col;
                    cc__emit_err(ctx, &sn,
                                 "CC: deadlock: `@nursery closing(ch)` closes channels only after all children exit; "
                                 "`while (cc_chan_recv(ch, ...) == 0)` inside the same nursery can wait forever. "
                                 "Move the draining loop outside the nursery, or close explicitly / send a sentinel.");
                    cc__emit_note(ctx, &sn, "Set CC_ALLOW_NURSERY_CLOSING_DRAIN=1 to bypass this heuristic check.");
                    ctx->errors++;
                    return;
                }
            }

            /* Heuristic warning for await/recv loops inside the same closing nursery. */
            {
                int has_loop = cc__body_has_loop_keyword(buf + body_s, body_e - body_s);
                int has_await_recv = cc__body_has_await_recv(buf + body_s, body_e - body_s, chname, rxname);
                if (has_loop && has_await_recv) {
                    StubNodeView sn;
                    memset(&sn, 0, sizeof(sn));
                    sn.file = ctx->input_path;
                    sn.line_start = nur_line;
                    sn.col_start = nur_col;
                    cc__emit_deadlock_diag(ctx, &sn,
                                           "CC: warning: `@nursery closing(ch)` + await/recv in a loop may deadlock (closing happens after children exit). "
                                           "Prefer draining outside the nursery, or close explicitly / send a sentinel.",
                                           "Set CC_ALLOW_NURSERY_CLOSING_DRAIN=1 to bypass this heuristic check.");
                }
            }
        }

        col++;
    }
}

static int cc__call_has_unique_flag(const StubNodeView* nodes, const ChildList* kids, int call_idx) {
    if (!nodes || !kids) return 0;
    const ChildList* cl = &kids[call_idx];
    /* We care about the 2nd argument to cc_slice_make_id(alloc_id, unique, transferable, is_sub). */
    int arg_pos = 0;
    for (int i = 0; i < cl->len; i++) {
        const StubNodeView* c = &nodes[cl->child[i]];
        if (c->kind == CC_STUB_CONST && c->aux_s1) {
            arg_pos++;
            if (arg_pos == 2 && strcmp(c->aux_s1, "1") == 0) return 1;
        }
        if (c->kind == CC_STUB_IDENT && c->aux_s1) {
            arg_pos++;
            if (arg_pos == 2 && strcmp(c->aux_s1, "true") == 0) return 1;
            if (strcmp(c->aux_s1, "CC_SLICE_ID_UNIQUE") == 0) return 1;
        }
    }
    return 0;
}

static int cc__subtree_has_call_named(const StubNodeView* nodes,
                                      const ChildList* kids,
                                      int idx,
                                      const char* name) {
    if (!nodes || !kids || !name) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1 && strcmp(n->aux_s1, name) == 0) return 1;
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_has_call_named(nodes, kids, cl->child[i], name)) return 1;
    }
    return 0;
}

static int cc__subtree_find_first_ident_matching_scope(const StubNodeView* nodes,
                                                       const ChildList* kids,
                                                       int idx,
                                                       CCScope* scopes,
                                                       int scope_n,
                                                       const char* exclude_name,
                                                       const char** out_name) {
    if (!nodes || !kids || !scopes || !out_name) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_IDENT && n->aux_s1) {
        if (!exclude_name || strcmp(n->aux_s1, exclude_name) != 0) {
            CCSliceVar* v = cc__scopes_lookup(scopes, scope_n, n->aux_s1);
            if (v) { *out_name = n->aux_s1; return 1; }
        }
    }
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_find_first_ident_matching_scope(nodes, kids, cl->child[i], scopes, scope_n, exclude_name, out_name))
            return 1;
    }
    return 0;
}

static int cc__subtree_has_unique_make_id(const StubNodeView* nodes,
                                          const ChildList* kids,
                                          int idx) {
    if (!nodes || !kids) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1 && strcmp(n->aux_s1, "cc_slice_make_id") == 0) {
        return cc__call_has_unique_flag(nodes, kids, idx);
    }
    if (n->kind == CC_STUB_IDENT && n->aux_s1 && strcmp(n->aux_s1, "CC_SLICE_ID_UNIQUE") == 0) return 1;
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__subtree_has_unique_make_id(nodes, kids, cl->child[i])) return 1;
    }
    return 0;
}

static int cc__subtree_collect_call_names(const StubNodeView* nodes,
                                         const ChildList* kids,
                                         int idx,
                                         const char** out_names,
                                         int* io_n,
                                         int cap) {
    if (!nodes || !kids || !out_names || !io_n) return 0;
    const StubNodeView* n = &nodes[idx];
    if (n->kind == CC_STUB_CALL && n->aux_s1) {
        int seen = 0;
        for (int i = 0; i < *io_n; i++) if (strcmp(out_names[i], n->aux_s1) == 0) seen = 1;
        if (!seen && *io_n < cap) out_names[(*io_n)++] = n->aux_s1;
    }
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        cc__subtree_collect_call_names(nodes, kids, cl->child[i], out_names, io_n, cap);
    }
    return 1;
}

static int cc__closure_captures_stack_slice_view(int closure_idx,
                                                 const StubNodeView* nodes,
                                                 const ChildList* kids,
                                                 CCScope* scopes,
                                                 int scope_n) {
    /* Build set of local names declared inside the closure (decl items + params). */
    const char* locals[256];
    int locals_n = 0;
    int stack[512];
    int sp = 0;
    stack[sp++] = closure_idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if ((n->kind == CC_STUB_DECL_ITEM || n->kind == CC_STUB_PARAM) && n->aux_s1) {
            int seen = 0;
            for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], n->aux_s1) == 0) seen = 1;
            if (!seen && locals_n < (int)(sizeof(locals)/sizeof(locals[0]))) locals[locals_n++] = n->aux_s1;
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }

    /* Collect call names so we can skip callee identifier tokens. */
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, closure_idx, call_names, &call_n, 64);

    /* Scan ident uses in closure subtree. */
    sp = 0;
    stack[sp++] = closure_idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if (n->kind == CC_STUB_IDENT && n->aux_s1) {
            const char* nm = n->aux_s1;
            int is_call = 0;
            for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
            if (!is_call) {
                int is_local = 0;
                for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], nm) == 0) is_local = 1;
                if (!is_local) {
                    CCSliceVar* v = cc__scopes_lookup(scopes, scope_n, nm);
                    if (v && v->is_slice && v->is_stack_slice_view) return 1;
                }
            }
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }

    return 0;
}

static int cc__subtree_find_first_kind(const StubNodeView* nodes,
                                       const ChildList* kids,
                                       int idx,
                                       int kind,
                                       int* out_idx) {
    if (!nodes || !kids || !out_idx) return 0;
    int stack[512];
    int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        if (nodes[cur].kind == kind) { *out_idx = cur; return 1; }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) stack[sp++] = cl->child[i];
    }
    return 0;
}

static int cc__subtree_has_kind(const StubNodeView* nodes,
                                const ChildList* kids,
                                int idx,
                                int kind) {
    if (!nodes || !kids) return 0;
    int stack[512];
    int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        if (nodes[cur].kind == kind) return 1;
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) stack[sp++] = cl->child[i];
    }
    return 0;
}

static int cc__subtree_find_first_bound_ident(const StubNodeView* nodes,
                                              const ChildList* kids,
                                              int idx,
                                              const char** bound_names,
                                              const int* bound_closure_idx,
                                              int bound_n,
                                              int* out_closure_idx) {
    if (!nodes || !kids || !bound_names || !bound_closure_idx || !out_closure_idx) return 0;
    int stack[512];
    int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        if (nodes[cur].kind == CC_STUB_IDENT && nodes[cur].aux_s1) {
            const char* nm = nodes[cur].aux_s1;
            for (int i = 0; i < bound_n; i++) {
                if (bound_names[i] && strcmp(bound_names[i], nm) == 0) { *out_closure_idx = bound_closure_idx[i]; return 1; }
            }
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) stack[sp++] = cl->child[i];
    }
    return 0;
}

static int cc__closure_is_under_return(const StubNodeView* nodes, int closure_idx) {
    if (!nodes) return 0;
    int cur = nodes[closure_idx].parent;
    while (cur >= 0) {
        if (nodes[cur].kind == CC_STUB_STMT && nodes[cur].aux_s1 && strcmp(nodes[cur].aux_s1, "spawn") == 0)
            return 0; /* nursery spawn context */
        if (nodes[cur].kind == CC_STUB_RETURN)
            return 1;
        cur = nodes[cur].parent;
    }
    return 0;
}

static int cc__is_global_decl_item(const StubNodeView* nodes, int n, int idx) {
    if (!nodes || idx < 0 || idx >= n) return 0;
    if (nodes[idx].kind != CC_STUB_DECL_ITEM) return 0;
    int p = nodes[idx].parent;
    if (p < 0 || p >= n) return 0;
    if (nodes[p].kind != CC_STUB_DECL) return 0;
    return nodes[p].parent == -1;
}

static int cc__subtree_should_apply_slice_copy_rule(const StubNodeView* nodes,
                                                    const ChildList* kids,
                                                    int idx,
                                                    const char* lhs_name,
                                                    const char* rhs_name) {
    if (!nodes || !kids || !rhs_name) return 0;
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, idx, call_names, &call_n, 64);

    /* Count non-function identifier tokens in the subtree. If we see more than the rhs itself,
       this is likely a projection (e.g. `s.ptr`) rather than a slice copy. */
    int rhs_seen = 0;
    int other_ident = 0;
    int saw_member = 0;

    /* Simple DFS stack */
    int stack[256];
    int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        const StubNodeView* n = &nodes[cur];
        if (n->kind == CC_STUB_MEMBER) {
            saw_member = 1;
        }
        if (n->kind == CC_STUB_IDENT && n->aux_s1) {
            const char* nm = n->aux_s1;
            if (lhs_name && strcmp(nm, lhs_name) == 0) {
                /* ignore lhs */
            } else {
                int is_call = 0;
                for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
                if (!is_call && strcmp(nm, "true") != 0 && strcmp(nm, "false") != 0 && strcmp(nm, "NULL") != 0) {
                    if (strcmp(nm, rhs_name) == 0) rhs_seen = 1;
                    else other_ident = 1;
                }
            }
        }
        const ChildList* cl = &kids[cur];
        for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
            stack[sp++] = cl->child[i];
        }
    }
    return rhs_seen && !other_ident && !saw_member;
}

static int cc__walk(int idx,
                    const StubNodeView* nodes,
                    const ChildList* kids,
                    CCScope* scopes,
                    int* io_scope_n,
                    CCCheckerCtx* ctx);

static int cc__walk_call(int idx,
                         const StubNodeView* nodes,
                         const ChildList* kids,
                         CCScope* scopes,
                         int* io_scope_n,
                         CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    if (!n->aux_s1) return 0;

    /* Move markers (parse-only): cc__move_marker_impl(&x) */
    if (strcmp(n->aux_s1, "cc__move_marker_impl") == 0) {
        const ChildList* cl = &kids[idx];
        CCSliceVar* to_move[16];
        int to_move_n = 0;
        for (int i = 0; i < cl->len; i++) {
            const StubNodeView* c = &nodes[cl->child[i]];
            if (c->kind == CC_STUB_IDENT && c->aux_s1) {
                /* The recorder also emits the callee as an IDENT child; ignore that and
                   mark any slice variable args as moved. */
                if (strcmp(c->aux_s1, "cc__move_marker_impl") == 0) continue;
                CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, c->aux_s1);
                if (v && v->is_slice && to_move_n < (int)(sizeof(to_move)/sizeof(to_move[0]))) {
                    to_move[to_move_n++] = v;
                }
            }
        }

        /* Walk children first: `cc_move(x)` should not report use-after-move of `x` inside the same expression. */
        for (int i = 0; i < cl->len; i++) {
            if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
        }
        for (int i = 0; i < to_move_n; i++) {
            if (to_move[i]) to_move[i]->pending_move = 1;
        }
        return 0;
    }

    /* walk children */
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    return 0;
}

static int cc__walk_closure(int idx,
                            const StubNodeView* nodes,
                            const ChildList* kids,
                            CCScope* scopes,
                            int* io_scope_n,
                            CCCheckerCtx* ctx) {
    /* Walk closure body in a nested scope, collecting captures of move-only slices. */
    CCScope closure_scope = {0};
    scopes[(*io_scope_n)++] = closure_scope;

    /* Collect names declared inside the closure (decl items + params). */
    const char* locals[256];
    int locals_n = 0;
    const ChildList* cl0 = &kids[idx];
    for (int i = 0; i < cl0->len; i++) {
        const StubNodeView* c = &nodes[cl0->child[i]];
        if ((c->kind == CC_STUB_DECL_ITEM || c->kind == CC_STUB_PARAM) && c->aux_s1) {
            int seen = 0;
            for (int k = 0; k < locals_n; k++) if (strcmp(locals[k], c->aux_s1) == 0) seen = 1;
            if (!seen && locals_n < (int)(sizeof(locals)/sizeof(locals[0]))) locals[locals_n++] = c->aux_s1;
        }
    }

    /* Collect call names so we can skip callee identifier tokens. */
    const char* call_names[64];
    int call_n = 0;
    cc__subtree_collect_call_names(nodes, kids, idx, call_names, &call_n, 64);

    /* Collect identifier uses in the closure subtree (excluding locals/params and callees). */
    const char* used_names[256];
    int used_n = 0;
    {
        /* DFS over closure subtree */
        int stack[512];
        int sp = 0;
        stack[sp++] = idx;
        while (sp > 0) {
            int cur = stack[--sp];
            const StubNodeView* n = &nodes[cur];
            if (n->kind == CC_STUB_IDENT && n->aux_s1) {
                const char* nm = n->aux_s1;
                int is_call = 0;
                for (int i = 0; i < call_n; i++) if (strcmp(call_names[i], nm) == 0) is_call = 1;
                if (!is_call) {
                    int is_local = 0;
                    for (int i = 0; i < locals_n; i++) if (strcmp(locals[i], nm) == 0) is_local = 1;
                    if (!is_local) {
                        int seen = 0;
                        for (int k = 0; k < used_n; k++) if (strcmp(used_names[k], nm) == 0) seen = 1;
                        if (!seen && used_n < (int)(sizeof(used_names)/sizeof(used_names[0]))) used_names[used_n++] = nm;
                    }
                }
            }
            const ChildList* cl = &kids[cur];
            for (int i = 0; i < cl->len && sp < (int)(sizeof(stack)/sizeof(stack[0])); i++) {
                stack[sp++] = cl->child[i];
            }
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        int ch = cl->child[i];
        if (cc__walk(ch, nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }

    /* Apply implicit move for captured move-only slices (names used but not declared locally). */
    for (int i = 0; i < used_n; i++) {
        const char* nm = used_names[i];
        if (!nm) continue;
        CCSliceVar* local = cc__scope_find(&scopes[(*io_scope_n) - 1], nm);
        if (local) continue; /* local to closure */
        CCSliceVar* outer = cc__scopes_lookup(scopes, (*io_scope_n) - 1, nm);
        if (outer && outer->is_slice && outer->move_only) outer->moved = 1;
    }

    /* Pop closure scope */
    cc__scope_free(&scopes[(*io_scope_n) - 1]);
    (*io_scope_n)--;
    return 0;
}

static int cc__walk_assign(int idx,
                           const StubNodeView* nodes,
                           const ChildList* kids,
                           CCScope* scopes,
                           int* io_scope_n,
                           CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    const char* lhs = n->aux_s1; /* best-effort from TCC recorder */
    const char* rhs = NULL;

    (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, lhs, &rhs);

    if (lhs && rhs && strcmp(lhs, rhs) != 0) {
        CCSliceVar* lhs_v = cc__scopes_lookup(scopes, *io_scope_n, lhs);
        CCSliceVar* rhs_v = cc__scopes_lookup(scopes, *io_scope_n, rhs);
        int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
        int saw_member = cc__subtree_has_kind(nodes, kids, idx, CC_STUB_MEMBER);

        if (rhs_v && rhs_v->is_slice) {
            /* Overwrite clears moved-from status for lhs. */
            if (lhs_v) lhs_v->moved = 0;

            /* If we assign from a slice var, treat lhs as a slice var too. */
            if (lhs_v) lhs_v->is_slice = 1;

            /* Only treat as a slice copy/move when RHS isn't being projected via member access. */
            if (!saw_member) {
                if (rhs_v->move_only && !has_move_marker) {
                    cc__emit_err_fmt(ctx, n, "cannot copy unique slice '%s' (type T[:!])", rhs_v->name ? rhs_v->name : "?");
                    cc__emit_note(ctx, n, "unique slices have move-only semantics; use cc_move(x) to transfer ownership");
                    ctx->errors++;
                    return -1;
                }
                if (rhs_v->move_only && has_move_marker) {
                    /* cc_move(...) is handled by the move marker call; don't mark moved here,
                       otherwise we can falsely report use-after-move within the same expression. */
                    if (lhs_v) lhs_v->move_only = 1;
                } else if (lhs_v && !has_move_marker) {
                    lhs_v->move_only = 0;
                }
            }
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    /* Commit pending moves at full-expression boundary. */
    cc__commit_pending_moves(scopes, *io_scope_n);
    return 0;
}

static int cc__walk_return(int idx,
                           const StubNodeView* nodes,
                           const ChildList* kids,
                           CCScope* scopes,
                           int* io_scope_n,
                           CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];
    const char* name = NULL;
    (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, NULL, &name);
    if (name) {
        CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, name);
        if (v && v->is_slice) {
            int saw_member = cc__subtree_has_kind(nodes, kids, idx, CC_STUB_MEMBER);
            int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
            if (v->move_only && !has_move_marker && !saw_member) {
                cc__emit_err_fmt(ctx, n, "cannot return unique slice '%s' without move", name);
                cc__emit_note(ctx, n, "unique slices (T[:!]) require explicit ownership transfer; use: return cc_move(x)");
                ctx->errors++;
                return -1;
            }
            /* cc_move(...) is handled by the move marker call + commit at expression boundary. */
        }
    }

    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    /* Commit pending moves at full-expression boundary. */
    cc__commit_pending_moves(scopes, *io_scope_n);
    return 0;
}

static int cc__walk(int idx,
                    const StubNodeView* nodes,
                    const ChildList* kids,
                    CCScope* scopes,
                    int* io_scope_n,
                    CCCheckerCtx* ctx) {
    const StubNodeView* n = &nodes[idx];

    /* Only enforce semantic checks within the user's input file. We still recurse so
       that we can reach user-file nodes that are parented under include contexts. */
    if (ctx && ctx->input_path && n->file && !cc__same_source_file(n->file, ctx->input_path)) {
        const ChildList* cl = &kids[idx];
        for (int i = 0; i < cl->len; i++) {
            if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
        }
        return 0;
    }

    if (n->kind == CC_STUB_DECL_ITEM && n->aux_s1 && n->aux_s2) {
        CCScope* cur = &scopes[(*io_scope_n) - 1];
        CCSliceVar* v = cc__scope_add(cur, n->aux_s1);
        if (!v) return -1;
        v->decl_line = n->line_start;
        v->decl_col = n->col_start;
        v->is_slice = (strstr(n->aux_s2, "CCSlice") != NULL);
        if (strchr(n->aux_s2, '[') && strchr(n->aux_s2, ']')) {
            v->is_array = 1;
        }

        /* Determine move_only from initializer subtree */
        {
            const ChildList* cl = &kids[idx];
            const char* copy_from = NULL;
            int saw_slice_ctor = 0;

            for (int i = 0; i < cl->len; i++) {
                const StubNodeView* c = &nodes[cl->child[i]];
                if (c->kind == CC_STUB_CALL && c->aux_s1) {
                    if (strncmp(c->aux_s1, "cc_slice_", 9) == 0) saw_slice_ctor = 1;
                }
            }

            /* If initializer is a known slice constructor, treat as slice even if the type string
               prints as 'struct <anonymous>' (CCSlice is a typedef of an anonymous struct). */
            if (saw_slice_ctor) v->is_slice = 1;

            if (v->is_slice) {
                /* move-only by provenance: detect unique-id construction anywhere under initializer */
                if (cc__subtree_has_unique_make_id(nodes, kids, idx)) v->move_only = 1;

                /* Stack-slice view detection (best-effort): if init uses cc_slice_from_buffer/parts with a local array. */
                int uses_buf = cc__subtree_has_call_named(nodes, kids, idx, "cc_slice_from_buffer");
                int uses_parts = cc__subtree_has_call_named(nodes, kids, idx, "cc_slice_from_parts");
                if (uses_buf || uses_parts) {
                    int st[256];
                    int sp = 0;
                    st[sp++] = idx;
                    while (sp > 0) {
                        int curi = st[--sp];
                        const StubNodeView* nn = &nodes[curi];
                        if (nn->kind == CC_STUB_IDENT && nn->aux_s1) {
                            CCSliceVar* maybe = cc__scopes_lookup(scopes, *io_scope_n, nn->aux_s1);
                            if (maybe && maybe->is_array) {
                                v->is_stack_slice_view = 1;
                                break;
                            }
                        }
                        const ChildList* k = &kids[curi];
                        for (int j = 0; j < k->len && sp < (int)(sizeof(st)/sizeof(st[0])); j++)
                            st[sp++] = k->child[j];
                    }
                }
            }

            /* Find a candidate RHS identifier in the initializer (best-effort). */
            (void)cc__subtree_find_first_ident_matching_scope(nodes, kids, idx, scopes, *io_scope_n, v->name, &copy_from);

            /* Copy rule for decl initializers: `CCSlice t = s;` */
            if (copy_from && copy_from != v->name) {
                CCSliceVar* rhs = cc__scopes_lookup(scopes, *io_scope_n, copy_from);
                /* If we see assignment from an existing slice var, treat this decl as slice too
                   (CCSlice prints as 'struct <anonymous>' in type_to_str). */
                if (rhs && rhs->is_slice) v->is_slice = 1;
                int has_move_marker = cc__subtree_has_call_named(nodes, kids, idx, "cc__move_marker_impl");
                int is_simple_copy = cc__subtree_should_apply_slice_copy_rule(nodes, kids, idx, v->name, copy_from);
                if (rhs && rhs->is_slice && rhs->move_only && !has_move_marker && is_simple_copy) {
                    cc__emit_err_fmt(ctx, n, "cannot copy unique slice '%s' (type T[:!])", copy_from);
                    cc__emit_note(ctx, n, "unique slices have move-only semantics; use cc_move(x) to transfer ownership");
                    ctx->errors++;
                    return -1;
                }
                if (rhs && rhs->is_slice && rhs->move_only && has_move_marker) {
                    /* Moving a move-only slice produces a move-only slice value. */
                    v->move_only = 1;
                }
            }
        }
    }

    if (n->kind == CC_STUB_IDENT && n->aux_s1) {
        CCSliceVar* v = cc__scopes_lookup(scopes, *io_scope_n, n->aux_s1);
        if (v && v->is_slice && v->moved) {
            cc__emit_err_fmt(ctx, n, "use of moved slice '%s'", n->aux_s1);
            if (v->decl_line > 0) {
                StubNodeView decl_node = {0};
                decl_node.line_start = v->decl_line;
                decl_node.col_start = v->decl_col;
                cc__emit_note_fmt(ctx, &decl_node, "'%s' was declared here and has been moved", n->aux_s1);
            }
            cc__emit_note(ctx, n, "after cc_move(x), the source variable is no longer valid");
            ctx->errors++;
            return -1;
        }
    }

    if (n->kind == CC_STUB_CALL) {
        return cc__walk_call(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    if (n->kind == CC_STUB_CLOSURE) {
        if (cc__walk_closure(idx, nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
        if (cc__closure_is_under_return(nodes, idx)) {
            if (cc__closure_captures_stack_slice_view(idx, nodes, kids, scopes, *io_scope_n)) {
                cc__emit_err(ctx, n, "CC: cannot capture stack slice in escaping closure");
                ctx->errors++;
                return -1;
            }
        }
        return 0;
    }

    if (n->kind == CC_STUB_ASSIGN) {
        return cc__walk_assign(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    if (n->kind == CC_STUB_RETURN) {
        return cc__walk_return(idx, nodes, kids, scopes, io_scope_n, ctx);
    }

    /* default: recurse */
    const ChildList* cl = &kids[idx];
    for (int i = 0; i < cl->len; i++) {
        if (cc__walk(cl->child[i], nodes, kids, scopes, io_scope_n, ctx) != 0) return -1;
    }
    if (n->kind == CC_STUB_DECL_ITEM) {
        /* Commit pending moves at full-expression boundary of an initializer. */
        cc__commit_pending_moves(scopes, *io_scope_n);
    }
    return 0;
}

int cc_check_ast(const CCASTRoot* root, CCCheckerCtx* ctx) {
    if (!root || !ctx) return -1;
    ctx->errors = 0;


    /* `await` is allowed, but only inside @async functions.
       Ignore `await` in comments/strings so tests can mention it in prose. */
    if (ctx->input_path) {
        FILE* f = fopen(ctx->input_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (1 << 20)) {
                char* buf = (char*)malloc((size_t)n + 1);
                if (buf) {
                    size_t got = fread(buf, 1, (size_t)n, f);
                    buf[got] = 0;

                    int in_line_comment = 0;
                    int in_block_comment = 0;
                    int in_str = 0;
                    int in_chr = 0;

                    int saw_await = 0;
                    int saw_async = 0;
                    for (size_t i = 0; i + 5 <= got; i++) {
                        char c = buf[i];
                        char c2 = (i + 1 < got) ? buf[i + 1] : 0;
                        if (in_line_comment) {
                            if (c == '\n') in_line_comment = 0;
                            continue;
                        }
                        if (in_block_comment) {
                            if (c == '*' && c2 == '/') { in_block_comment = 0; i++; }
                            continue;
                        }
                        if (in_str) {
                            if (c == '\\' && i + 1 < got) { i++; continue; }
                            if (c == '"') in_str = 0;
                            continue;
                        }
                        if (in_chr) {
                            if (c == '\\' && i + 1 < got) { i++; continue; }
                            if (c == '\'') in_chr = 0;
                            continue;
                        }

                        if (c == '/' && c2 == '/') { in_line_comment = 1; i++; continue; }
                        if (c == '/' && c2 == '*') { in_block_comment = 1; i++; continue; }
                        if (c == '"') { in_str = 1; continue; }
                        if (c == '\'') { in_chr = 1; continue; }

                        /* Track presence of @async and await (outside comments/strings). */
                        if (c == '@' && i + 6 <= got && memcmp(buf + i + 1, "async", 5) == 0) {
                            char after = (i + 6 < got) ? buf[i + 6] : ' ';
                            if (!(isalnum((unsigned char)after) || after == '_')) saw_async = 1;
                        }
                        if (buf[i] == 'a' &&
                            buf[i + 1] == 'w' &&
                            buf[i + 2] == 'a' &&
                            buf[i + 3] == 'i' &&
                            buf[i + 4] == 't') {
                            char before = (i > 0) ? buf[i - 1] : ' ';
                            char after = (i + 5 < got) ? buf[i + 5] : ' ';
                            int before_ok = !(isalnum((unsigned char)before) || before == '_');
                            int after_ok = !(isalnum((unsigned char)after) || after == '_');
                            if (before_ok && after_ok) saw_await = 1;
                        }
                    }
                    if (saw_await && !saw_async) {
                        StubNodeView sn;
                        memset(&sn, 0, sizeof(sn));
                        sn.file = ctx->input_path;
                        sn.line_start = 1;
                        sn.col_start = 1;
                        cc__emit_err(ctx, &sn, "CC: await is only valid inside @async functions");
                        ctx->errors++;
                        free(buf);
                        fclose(f);
                        return -1;
                    }

                    /* Heuristic deadlock check for `@nursery closing(...)` misuse. */
                    cc__check_nursery_closing_deadlock_text(ctx, buf, got);
                    if (ctx->errors) {
                        free(buf);
                        fclose(f);
                        return -1;
                    }
                    /* Heuristic warning for spawn()+cc_block_on() footgun. */
                    cc__check_spawn_block_on_text(ctx, buf, got);
                    if (ctx->errors) {
                        free(buf);
                        fclose(f);
                        return -1;
                    }
                    /* Heuristic warning for cc_block_on inside nursery bodies. */
                    cc__check_nursery_block_on_text(ctx, buf, got);
                    if (ctx->errors) {
                        free(buf);
                        fclose(f);
                        return -1;
                    }
                    free(buf);
                }
            }
            fclose(f);
        }
    }

    /* Fallback: if stub-AST parse fails (node list empty), avoid passing through raw CC markers.
       NOTE: `await` is now allowed in-progress, so we no longer hard-error here. */
    if (!root->nodes || root->node_count <= 0) {
        if (ctx->input_path) {
            FILE* f = fopen(ctx->input_path, "rb");
            if (f) {
                int c;
                while ((c = fgetc(f)) != EOF) {
                    if (c == '\n') { continue; }
                    if (c == 'a') {
                        int w = fgetc(f);
                        int a2 = fgetc(f);
                        int i2 = fgetc(f);
                        int t2 = fgetc(f);
                        if (w == 'w' && a2 == 'a' && i2 == 'i' && t2 == 't') {
                            StubNodeView sn;
                            memset(&sn, 0, sizeof(sn));
                            sn.file = ctx->input_path;
                            sn.line_start = 1;
                            sn.col_start = 1;
                            cc__emit_err(ctx, &sn, "CC: await is only valid inside @async functions");
                            ctx->errors++;
                            fclose(f);
                            return -1;
                        }
                        /* push back in reverse order if not matched */
                        if (t2 != EOF) ungetc(t2, f);
                        if (i2 != EOF) ungetc(i2, f);
                        if (a2 != EOF) ungetc(a2, f);
                        if (w != EOF) ungetc(w, f);
                    }
                }
                fclose(f);
            }
        }
        /* Transitional: no stub nodes, skip other checks. */
        return 0;
    }

    /* (Unreachable now: handled above.) */
    const StubNodeView* nodes = (const StubNodeView*)root->nodes;
    int n = root->node_count;

    /* Performance/memory: stub AST can be huge due to headers (esp <std/prelude.cch>).
       For now, all checker semantics are TU-local; compact to nodes whose `file` matches input_path. */
    StubNodeView* owned_nodes = NULL;
    int* idx_map = NULL;
    if (ctx->input_path) {
        idx_map = (int*)malloc((size_t)n * sizeof(int));
        if (!idx_map) return -1;
        for (int i = 0; i < n; i++) idx_map[i] = -1;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const char* f = nodes[i].file;
            if (f && strcmp(f, ctx->input_path) != 0) continue;
            idx_map[i] = m++;
        }
        if (m > 0 && m < n) {
            owned_nodes = (StubNodeView*)calloc((size_t)m, sizeof(StubNodeView));
            if (!owned_nodes) { free(idx_map); return -1; }
            for (int i = 0; i < n; i++) {
                int ni = idx_map[i];
                if (ni < 0) continue;
                owned_nodes[ni] = nodes[i];
                int p = nodes[i].parent;
                if (p >= 0 && p < n) {
                    int np = idx_map[p];
                    owned_nodes[ni].parent = (np >= 0) ? np : -1;
                } else {
                    owned_nodes[ni].parent = -1;
                }
            }
            nodes = owned_nodes;
            n = m;
        }
    }

    /* Record function decl attrs from stub decl-items into symbols table (for future async/autoblocking).
       Note: store default attrs=0 too, so callers can distinguish "known sync" vs "unknown". */
    for (int i = 0; i < n; i++) {
        const StubNodeView* dn = &nodes[i];
        if (dn->kind != CC_STUB_DECL_ITEM) continue;
        if (!ctx->symbols || !dn->aux_s1 || !dn->aux_s2) continue;
        if (strchr(dn->aux_s2, '(')) {
            (void)cc_symbols_set_fn_attrs(ctx->symbols, dn->aux_s1, (unsigned int)dn->aux2);
        }
    }

    /* Enforce: `await` only inside @async functions (shape is handled by lowering). */
    for (int i = 0; i < n; i++) {
        const StubNodeView* an = &nodes[i];
        if (an->kind != CC_STUB_AWAIT) continue;
        int cur = an->parent;
        int ok = 0;
        while (cur >= 0 && cur < n) {
            const StubNodeView* pn = &nodes[cur];
            if (pn->kind == CC_STUB_DECL_ITEM && pn->aux_s1 && pn->aux_s2 && strchr(pn->aux_s2, '(')) {
                if (((unsigned int)pn->aux2 & CC_FN_ATTR_ASYNC) != 0) ok = 1;
                break;
            }
            cur = pn->parent;
        }
        if (!ok) {
            cc__emit_err(ctx, an, "CC: await is only valid inside @async functions");
            ctx->errors++;
            free(idx_map);
            free(owned_nodes);
            return -1;
        }
    }

    /* Channel ops in @async don't require explicit await - the autoblock pass wraps them automatically.
       This makes blocking channel ops cooperative without user effort. */

    /* Auto-blocking diagnostics (env-gated): identify direct calls to non-@async, non-@noblock
       functions inside @async functions. This is the classification backbone for spec auto-wrapping. */
    int dbg_autoblock = 0;
    {
        const char* e = getenv("CC_DEBUG_AUTOBLOCK");
        dbg_autoblock = (e && e[0] == '1') ? 1 : 0;
    }
    if (dbg_autoblock && ctx->symbols) {
        for (int i = 0; i < n; i++) {
            const StubNodeView* cn = &nodes[i];
            if (cn->kind != CC_STUB_CALL) continue;
            if (!cn->aux_s1) continue; /* callee name */
            /* Find containing @async function by walking parent chain to the nearest function decl-item. */
            const char* owner = NULL;
            int cur = cn->parent;
            while (cur >= 0 && cur < n) {
                const StubNodeView* pn = &nodes[cur];
                if (pn->kind == CC_STUB_DECL_ITEM && pn->aux_s1 && pn->aux_s2 && strchr(pn->aux_s2, '(')) {
                    unsigned int attrs = 0;
                    if (cc_symbols_lookup_fn_attrs(ctx->symbols, pn->aux_s1, &attrs) == 0 &&
                        (attrs & CC_FN_ATTR_ASYNC)) {
                        owner = pn->aux_s1;
                    }
                    break;
                }
                cur = pn->parent;
            }
            if (!owner) continue;

            unsigned int callee_attrs = 0;
            int has = (cc_symbols_lookup_fn_attrs(ctx->symbols, cn->aux_s1, &callee_attrs) == 0);
            if (has) {
                if (callee_attrs & CC_FN_ATTR_ASYNC) continue;
                if (callee_attrs & CC_FN_ATTR_NOBLOCK) continue;
            }

            /* Unknown callee => treat as non-@async (extern/FFI), but only note in debug mode. */
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "CC: auto-blocking candidate: call to '%s' inside @async '%s' would be wrapped in run_blocking",
                     cn->aux_s1, owner ? owner : "<async>");
            cc__emit_note(ctx, cn, msg);
        }
    }

    ChildList* kids = (ChildList*)calloc((size_t)n, sizeof(ChildList));
    if (!kids) { free(idx_map); free(owned_nodes); return -1; }
    for (int i = 0; i < n; i++) {
        int p = nodes[i].parent;
        if (p >= 0 && p < n) cc__child_push(&kids[p], i);
    }

    /* Closure escape-kind approximation for stack-slice captures:
       - Allow: nursery-scoped spawn (direct literal or via local variable)
       - Disallow: return / store to global / store through member lvalue / pass as arg */
    unsigned char* closure_spawned = (unsigned char*)calloc((size_t)n, 1);
    unsigned char* closure_escapes = (unsigned char*)calloc((size_t)n, 1);
    const char* bound_names[256];
    int bound_closure_idx[256];
    int bound_n = 0;
    const char* global_names[256];
    int global_n = 0;
    if (closure_spawned && closure_escapes) {
        /* Bind `name -> closure_idx` from decl initializers: `CCClosureN name = () => ...;` */
        for (int i = 0; i < n; i++) {
            if (nodes[i].kind != CC_STUB_DECL_ITEM) continue;
            if (!nodes[i].aux_s1) continue;
            if (cc__is_global_decl_item(nodes, n, i)) {
                if (global_n < (int)(sizeof(global_names)/sizeof(global_names[0]))) {
                    global_names[global_n++] = nodes[i].aux_s1;
                }
            }
            int cidx = -1;
            if (cc__subtree_find_first_kind(nodes, kids, i, CC_STUB_CLOSURE, &cidx)) {
                if (bound_n < (int)(sizeof(bound_names)/sizeof(bound_names[0]))) {
                    bound_names[bound_n] = nodes[i].aux_s1;
                    bound_closure_idx[bound_n] = cidx;
                    bound_n++;
                }
            }
        }
        /* Propagate through simple assigns: `c2 = c;` */
        for (int i = 0; i < n; i++) {
            if (nodes[i].kind != CC_STUB_ASSIGN) continue;
            if (!nodes[i].aux_s1) continue; /* lhs name */
            int cidx = -1;
            if (cc__subtree_find_first_bound_ident(nodes, kids, i, bound_names, bound_closure_idx, bound_n, &cidx)) {
                if (cidx >= 0 && bound_n < (int)(sizeof(bound_names)/sizeof(bound_names[0]))) {
                    bound_names[bound_n] = nodes[i].aux_s1;
                    bound_closure_idx[bound_n] = cidx;
                    bound_n++;
                }
            }
        }
        /* Mark nursery spawned closures: `spawn ( <closure> )` or `spawn ( ident )` */
        for (int i = 0; i < n; i++) {
            if (nodes[i].kind != CC_STUB_STMT) continue;
            if (!nodes[i].aux_s1 || strcmp(nodes[i].aux_s1, "spawn") != 0) continue;
            int cidx = -1;
            if (cc__subtree_find_first_kind(nodes, kids, i, CC_STUB_CLOSURE, &cidx)) {
                if (cidx >= 0 && cidx < n) closure_spawned[cidx] = 1;
            } else if (cc__subtree_find_first_bound_ident(nodes, kids, i, bound_names, bound_closure_idx, bound_n, &cidx)) {
                if (cidx >= 0 && cidx < n) closure_spawned[cidx] = 1;
            }
        }
        /* Mark escaped closures:
           - return <closure-or-bound-ident>
           - assign to global
           - assign through member lvalue (obj.field = ...)
           - pass as arg to non-closure-call (foo(c) or foo(() => ...)) */
        for (int i = 0; i < n; i++) {
            if (nodes[i].kind == CC_STUB_RETURN) {
                int cidx = -1;
                if (cc__subtree_find_first_kind(nodes, kids, i, CC_STUB_CLOSURE, &cidx) ||
                    cc__subtree_find_first_bound_ident(nodes, kids, i, bound_names, bound_closure_idx, bound_n, &cidx)) {
                    if (cidx >= 0 && cidx < n) closure_escapes[cidx] = 1;
                }
            } else if (nodes[i].kind == CC_STUB_ASSIGN) {
                int cidx = -1;
                int rhs_has_closure = (cc__subtree_find_first_kind(nodes, kids, i, CC_STUB_CLOSURE, &cidx) ||
                                       cc__subtree_find_first_bound_ident(nodes, kids, i, bound_names, bound_closure_idx, bound_n, &cidx));
                if (!rhs_has_closure) continue;
                int escapes = 0;
                /* LHS global? */
                if (nodes[i].aux_s1) {
                    for (int g = 0; g < global_n; g++) {
                        if (global_names[g] && strcmp(global_names[g], nodes[i].aux_s1) == 0) { escapes = 1; break; }
                    }
                }
                /* Member lvalue? (best-effort) */
                if (!escapes && cc__subtree_has_kind(nodes, kids, i, CC_STUB_MEMBER)) escapes = 1;
                if (escapes && cidx >= 0 && cidx < n) closure_escapes[cidx] = 1;
            } else if (nodes[i].kind == CC_STUB_CALL) {
                /* If a closure is used as an argument to another call, treat as escaping.
                   Exclude immediate closure calls `c(...)` by checking call name equals bound name. */
                int cidx = -1;
                if (cc__subtree_find_first_kind(nodes, kids, i, CC_STUB_CLOSURE, &cidx)) {
                    if (cidx >= 0 && cidx < n) closure_escapes[cidx] = 1;
                } else {
                    int bcidx = -1;
                    if (cc__subtree_find_first_bound_ident(nodes, kids, i, bound_names, bound_closure_idx, bound_n, &bcidx)) {
                        /* If this CALL is itself the closure call (callee == var name), don't mark escape. */
                        int is_immediate = 0;
                        if (nodes[i].aux_s1) {
                            for (int b = 0; b < bound_n; b++) {
                                if (bound_closure_idx[b] == bcidx && bound_names[b] && strcmp(bound_names[b], nodes[i].aux_s1) == 0) {
                                    is_immediate = 1;
                                    break;
                                }
                            }
                        }
                        if (!is_immediate && bcidx >= 0 && bcidx < n) closure_escapes[bcidx] = 1;
                    }
                }
            }
        }
    }

    CCScope scopes[256];
    int scope_n = 0;
    memset(scopes, 0, sizeof(scopes));
    scopes[scope_n++] = (CCScope){0};

    for (int i = 0; i < n; i++) {
        if (nodes[i].parent != -1) continue;
        if (cc__walk(i, nodes, kids, scopes, &scope_n, ctx) != 0) break;
    }

    /* Post-check: stack-slice capture is illegal if the closure escapes (return/store/pass). */
    if (closure_escapes) {
        for (int i = 0; i < n; i++) {
            if (nodes[i].kind != CC_STUB_CLOSURE) continue;
            if (!closure_escapes[i]) continue;
            /* Nursery-spawn does not make escaping safe; once it escapes, forbid stack-slice capture. */
            if (cc__closure_captures_stack_slice_view(i, nodes, kids, scopes, scope_n)) {
                cc__emit_err(ctx, &nodes[i], "CC: cannot capture stack slice in escaping closure");
                ctx->errors++;
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) free(kids[i].child);
    free(kids);
    for (int i = 0; i < scope_n; i++) cc__scope_free(&scopes[i]);
    free(closure_spawned);
    free(closure_escapes);
    free(idx_map);
    free(owned_nodes);

    return ctx->errors ? -1 : 0;
}

