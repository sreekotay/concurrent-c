/* Stage 1: tokens, FileTape cache, file:line diagnostics.
 * Requires @grammar(rules) PpTok in the including TU. See README.md. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ---- tokens ------------------------------------------------------------- */

typedef enum {
    TK_IDENT = 1,
    TK_NUM,
    TK_STR,
    TK_CHR,
    TK_PUNCT,
    TK_EOF
} TokKind;

typedef struct {
    TokKind kind;
    CCSlice spell;   /* borrow into a pinned FileTape->bytes (or mint arena) */
    int file_id;
    size_t offset;
} Token;

static TokKind pptok_keep_kind(int id) {
    if (id == PpTok_KEEP_ident) return TK_IDENT;
    if (id == PpTok_KEEP_pp_number) return TK_NUM;
    if (id == PpTok_KEEP_string) return TK_STR;
    if (id == PpTok_KEEP_bt_string) return TK_STR; /* spell includes ticks */
    if (id == PpTok_KEEP_charlit) return TK_CHR;
    if (id == PpTok_KEEP_punct) return TK_PUNCT;
    return 0;
}

static int tok_eq(Token t, TokKind k, const char* lit) {
    size_t n = strlen(lit);
    return t.kind == k && t.spell.len == n && memcmp(t.spell.ptr, lit, n) == 0;
}

static int __attribute__((unused)) tok_is_ident(Token t, const char* lit) {
    return tok_eq(t, TK_IDENT, lit);
}

/* ---- stage-1 file tape cache -------------------------------------------- */

typedef struct {
    char* path;          /* owned */
    char* bytes;         /* owned, line-spliced; Token spells borrow here */
    size_t len;
    Token* toks;         /* owned array; spells borrow bytes */
    int ntoks;
    int file_id;
} FileTape;

/* Redis/pigz TUs warm many angle-includes into the stage-1 cache; 64
 * filled before nested `"redis_mem.h"` and looked like a missing file. */
enum { TAPE_CACHE_CAP = 512, TOK_BUILD_CAP = 1 << 16 };

typedef struct {
    FileTape* items[TAPE_CACHE_CAP];
    int n;
    int next_file_id;
} TapeCache;

/* ---- origins / loud diagnostics ----------------------------------------- */

static void offset_to_linecol(const char* bytes, size_t len, size_t off,
                              int* out_line, int* out_col) {
    int line = 1, col = 1;
    size_t n = off < len ? off : len;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] == '\n') { line++; col = 1; }
        else col++;
    }
    *out_line = line;
    *out_col = col;
}

/* Logical file:line at byte offset, honoring `#line` / CC_LN in tape bytes. */
static void tape_logical_at(const char* bytes, size_t len, size_t off,
                            const char* fallback_path, char* file, size_t fcap,
                            int* line_out) {
    int phys = 1;
    int logic = 1;
    char sticky[256];
    const char* cur_file = fallback_path ? fallback_path : "";
    size_t i = 0;
    size_t lim = off < len ? off : len;
    sticky[0] = 0;
    if (!file || !fcap || !line_out) return;
    file[0] = 0;
    *line_out = 1;
    if (!bytes) {
        snprintf(file, fcap, "%s", cur_file);
        return;
    }
    while (i < lim) {
        int at_bol = (i == 0 || bytes[i - 1] == '\n');
        if (at_bol && i + 5 < lim && bytes[i] == '#' &&
            strncmp(bytes + i, "#line", 5) == 0 &&
            (bytes[i + 5] == ' ' || bytes[i + 5] == '\t')) {
            size_t j = i + 5;
            int n = 0;
            while (j < lim && (bytes[j] == ' ' || bytes[j] == '\t')) j++;
            while (j < lim && bytes[j] >= '0' && bytes[j] <= '9') {
                n = n * 10 + (bytes[j] - '0');
                j++;
            }
            while (j < lim && (bytes[j] == ' ' || bytes[j] == '\t')) j++;
            if (j < lim && bytes[j] == '"') {
                size_t k = ++j;
                while (k < lim && bytes[k] != '"' && bytes[k] != '\n') k++;
                if (k < lim && bytes[k] == '"' && k > j) {
                    size_t nlen = k - j;
                    if (nlen >= sizeof(sticky)) nlen = sizeof(sticky) - 1;
                    memcpy(sticky, bytes + j, nlen);
                    sticky[nlen] = 0;
                    cur_file = sticky;
                }
            }
            logic = n;
            while (i < lim && bytes[i] != '\n') i++;
            if (i < lim && bytes[i] == '\n') {
                i++;
                phys++;
            }
            continue;
        }
        if (at_bol) {
            size_t ss = i;
            while (ss < lim && (bytes[ss] == ' ' || bytes[ss] == '\t')) ss++;
            if (ss + 8 < lim && memcmp(bytes + ss, "/*CC_LN ", 8) == 0) {
                size_t j = ss + 8;
                int n = 0;
                while (j < lim && bytes[j] >= '0' && bytes[j] <= '9') {
                    n = n * 10 + (bytes[j] - '0');
                    j++;
                }
                if (j < lim && bytes[j] == ' ') {
                    size_t k = j + 1;
                    size_t pe = k;
                    while (pe + 1 < lim &&
                           !(bytes[pe] == '*' && bytes[pe + 1] == '/'))
                        pe++;
                    if (pe + 1 < lim && bytes[pe] == '*') {
                        size_t nlen = pe - k;
                        while (nlen && bytes[k + nlen - 1] == ' ') nlen--;
                        if (nlen >= sizeof(sticky)) nlen = sizeof(sticky) - 1;
                        memcpy(sticky, bytes + k, nlen);
                        sticky[nlen] = 0;
                        cur_file = sticky;
                        logic = n;
                        while (i < lim && bytes[i] != '\n') i++;
                        if (i < lim && bytes[i] == '\n') {
                            i++;
                            phys++;
                        }
                        continue;
                    }
                }
            }
        }
        if (bytes[i] == '\n') {
            phys++;
            logic++;
        }
        i++;
    }
    snprintf(file, fcap, "%s", cur_file);
    *line_out = logic > 0 ? logic : phys;
}

static FileTape* tape_by_id(TapeCache* c, int file_id) {
    if (!c) return NULL;
    for (int i = 0; i < c->n; i++) {
        if (c->items[i]->file_id == file_id) return c->items[i];
    }
    return NULL;
}

/* Count of error diagnostics printed this process (reset per TU).  A cold
 * emit that printed errors must not be cached — warm would skip the diag. */
static int g_shadow_diag_errors = 0;

static void shadow_diag_errors_reset(void) { g_shadow_diag_errors = 0; }

static int shadow_diag_errors(void) { return g_shadow_diag_errors; }

static void diag_at(TapeCache* cache, Token t, const char* msg) {
    FileTape* ft = tape_by_id(cache, t.file_id);
    g_shadow_diag_errors++;
    if (!ft || !ft->bytes) {
        fprintf(stderr, "error: %s\n", msg);
        return;
    }
    int line = 1, col = 1;
    offset_to_linecol(ft->bytes, ft->len, t.offset, &line, &col);
    tape_logical_at(ft->bytes, ft->len, t.offset, ft->path, NULL, 0, &line);
    fprintf(stderr, "%s:%d:%d: error: %s\n", ft->path, line, col, msg);
}

typedef struct {
    Token* toks;
    int n;
    int cap;
    FileTape* building; /* file_id / bytes for offset calc during collect */
} TokBuild;

static size_t line_splice_copy(const char* in, size_t n, char* out) {
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        if (in[i] == '\\' && i + 1 < n && in[i + 1] == '\n') {
            i += 2;
            continue;
        }
        if (in[i] == '\\' && i + 2 < n && in[i + 1] == '\r' && in[i + 2] == '\n') {
            i += 3;
            continue;
        }
        out[o++] = in[i++];
    }
    return o;
}

static int tok_build_push(void* env, int id, CCSlice v) {
    TokBuild* b = (TokBuild*)env;
    TokKind k = pptok_keep_kind(id);
    size_t off = 0;
    if (!k) return 0;
    if (v.ptr >= b->building->bytes &&
        v.ptr < b->building->bytes + b->building->len) {
        off = (size_t)(v.ptr - b->building->bytes);
    }
    if (b->n >= b->cap) {
        int ncap = b->cap > 0 ? b->cap * 2 : 4096;
        Token* nt = (Token*)realloc(b->toks, sizeof(Token) * (size_t)ncap);
        if (!nt) {
            int line = 1, col = 1;
            offset_to_linecol(b->building->bytes, b->building->len, off, &line,
                              &col);
            fprintf(stderr,
                    "%s:%d:%d: error: stage1 token buffer realloc failed "
                    "(%d → %d tokens)\n",
                    b->building->path ? b->building->path : "<input>", line,
                    col, b->cap, ncap);
            return -1;
        }
        b->toks = nt;
        b->building->toks = nt;
        b->cap = ncap;
    }
    Token* t = &b->toks[b->n++];
    t->kind = k;
    t->spell = v;
    t->file_id = b->building->file_id;
    t->offset = off;
    return 0;
}

static char* xstrdup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

static int read_file(const char* path, char** out_buf, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    char* raw = (char*)malloc((size_t)n + 1);
    if (!raw) { fclose(f); return 0; }
    size_t rd = fread(raw, 1, (size_t)n, f);
    fclose(f);
    char* spliced = (char*)malloc(rd + 1);
    if (!spliced) { free(raw); return 0; }
    size_t m = line_splice_copy(raw, rd, spliced);
    free(raw);
    spliced[m] = 0;
    *out_buf = spliced;
    *out_len = m;
    return 1;
}

static FileTape* tape_cache_find(TapeCache* c, const char* path) {
    for (int i = 0; i < c->n; i++) {
        if (strcmp(c->items[i]->path, path) == 0) return c->items[i];
    }
    return NULL;
}

/* Load stage1 from owned `bytes` (takes ownership). `path` is the diagnostic
 * identity (usually the original input); may differ from the byte source when
 * comptime prepare+blank rewrites the buffer before whitelist parse.
 * Line-splices like `read_file` — PpTok expects spliced bytes. */
static FileTape* tape_cache_load_bytes(TapeCache* c, const char* path,
                                       char* bytes, size_t len, CCArena* arena) {
    FileTape* hit = tape_cache_find(c, path);
    if (hit) {
        free(bytes);
        return hit;
    }
    if (c->n >= TAPE_CACHE_CAP) {
        fprintf(stderr,
                "error: tape cache full (%d); cannot load '%s'\n",
                TAPE_CACHE_CAP, path ? path : "<null>");
        free(bytes);
        return NULL;
    }
    if (!bytes) return NULL;

    {
        char* spliced = (char*)malloc(len + 1);
        if (!spliced) {
            free(bytes);
            return NULL;
        }
        size_t m = line_splice_copy(bytes, len, spliced);
        free(bytes);
        spliced[m] = 0;
        bytes = spliced;
        len = m;
    }

    FileTape* ft = (FileTape*)calloc(1, sizeof(FileTape));
    if (!ft) {
        free(bytes);
        return NULL;
    }
    ft->path = xstrdup(path);
    ft->bytes = bytes;
    ft->len = len;
    ft->file_id = ++c->next_file_id;
    ft->toks = (Token*)malloc(sizeof(Token) * TOK_BUILD_CAP);
    if (!ft->path || !ft->toks) {
        free(ft->path);
        free(ft->bytes);
        free(ft->toks);
        free(ft);
        return NULL;
    }

    TokBuild b = {.toks = ft->toks, .n = 0, .cap = TOK_BUILD_CAP, .building = ft};
    if (!PpTok_collect(ft->bytes, ft->len, arena, tok_build_push, &b)) {
        fprintf(stderr,
                "error: tokenize failed for '%s' (often arena exhaustion)\n",
                path ? path : "<null>");
        free(ft->path);
        free(ft->bytes);
        free(ft->toks);
        free(ft);
        return NULL;
    }
    ft->ntoks = b.n;
    c->items[c->n++] = ft;
    (void)arena;
    return ft;
}

static FileTape* tape_cache_load(TapeCache* c, const char* path, CCArena* arena) {
    char* bytes = NULL;
    size_t len = 0;
    if (!read_file(path, &bytes, &len)) return NULL;
    return tape_cache_load_bytes(c, path, bytes, len, arena);
}

static void tape_cache_free(TapeCache* c) {
    for (int i = 0; i < c->n; i++) {
        FileTape* ft = c->items[i];
        free(ft->path);
        free(ft->bytes);
        free(ft->toks);
        free(ft);
    }
    c->n = 0;
}

/* `@typeview(Mode) Base` → IDENT `Base_Restrict_Mode`.
 * Leaves `@typeview Mode on Base {…}` alone. */
static int shadow_rewrite_restricted_type_sugar(Token* toks, int* pn) {
    int i = 0;
    int n;
    if (!toks || !pn) return 1;
    n = *pn;
    while (i + 5 < n) {
        Token at = toks[i];
        Token kw = toks[i + 1];
        Token lp = toks[i + 2];
        Token mode = toks[i + 3];
        Token rp = toks[i + 4];
        Token base = toks[i + 5];
        char* buf;
        size_t blen;
        int j;
        if (tok_eq(at, TK_PUNCT, "@") && kw.kind == TK_IDENT &&
            tok_eq(kw, TK_IDENT, "restricted")) {
            fprintf(stderr,
                    "error: '@restricted' was removed; use '@typeview'\n");
            return 0;
        }
        if (!(tok_eq(at, TK_PUNCT, "@") && kw.kind == TK_IDENT &&
              tok_eq(kw, TK_IDENT, "typeview") &&
              tok_eq(lp, TK_PUNCT, "(") && mode.kind == TK_IDENT &&
              tok_eq(rp, TK_PUNCT, ")") && base.kind == TK_IDENT)) {
            i++;
            continue;
        }
        blen = base.spell.len + mode.spell.len + strlen("_Restrict_") + 1;
        buf = (char*)malloc(blen);
        if (!buf) return 0;
        snprintf(buf, blen, "%.*s_Restrict_%.*s", (int)base.spell.len,
                 base.spell.ptr, (int)mode.spell.len, mode.spell.ptr);
        toks[i].kind = TK_IDENT;
        toks[i].spell.ptr = buf;
        toks[i].spell.len = strlen(buf);
        /* Keep file_id/offset of '@' for diags. */
        for (j = i + 1; j + 5 < n; j++) toks[j] = toks[j + 5];
        n -= 5;
        *pn = n;
        i++;
    }
    return 1;
}

