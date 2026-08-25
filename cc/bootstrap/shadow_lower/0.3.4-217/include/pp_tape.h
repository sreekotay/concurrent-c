/* Stage 1: tokens, FileTape cache, file:line diagnostics.
 * Requires @grammar(rules) PpTok in the including TU. See README.md. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <ccc/std/vec.h>

extern size_t cc_unit_header_skip(const char* src, size_t n);

/* Blank a leading `#!ccc` / ccc shebang so line numbers stay put. */
static void shadow_blank_unit_header(char* buf, size_t n) {
    size_t skip, blank;
    if (!buf || !n) return;
    skip = cc_unit_header_skip(buf, n);
    if (skip == 0 || skip > n) return;
    blank = skip;
    if (blank > 0 && buf[blank - 1] == '\n') blank--;
    memset(buf, ' ', blank);
}

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

/* One record per physical line start. Query is predecessor (`off <= tok_off`)
 * — a hash Map cannot do that; a sorted Vec + bsearch can. */
typedef struct {
    size_t off;
    int logic;
    int phys;
    const char* file; /* interned; tape path or a `#line` / CC_LN spelling */
} TapeLineRec;

typedef char* TapeInternPath;

CC_VEC_DECL_HEAP(TapeLineRec, TapeLineVec);
CC_VEC_DECL_HEAP(TapeInternPath, TapePathVec);

typedef struct {
    char* path;          /* owned */
    char* bytes;         /* owned, line-spliced; Token spells borrow here */
    size_t len;
    Token* toks;         /* owned array; spells borrow bytes */
    int ntoks;
    int file_id;
    TapeLineVec lines;   /* owned; built once at load */
    TapePathVec paths;   /* owned interned `#line` / CC_LN paths */
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

static const char* tape_intern_path(FileTape* ft, const char* p, size_t n) {
    char* copy;
    size_t i, np;
    if (!ft) return "";
    if (!p || n == 0) return ft->path ? ft->path : "";
    if (ft->path && strlen(ft->path) == n && memcmp(ft->path, p, n) == 0)
        return ft->path;
    np = TapePathVec_len(&ft->paths);
    for (i = 0; i < np; i++) {
        char* e = ft->paths.data[i];
        if (e && strlen(e) == n && memcmp(e, p, n) == 0) return e;
    }
    copy = (char*)malloc(n + 1);
    if (!copy) {
        fprintf(stderr, "error: tape line path intern failed for '%s'\n",
                ft->path ? ft->path : "<input>");
        return NULL;
    }
    memcpy(copy, p, n);
    copy[n] = 0;
    if (TapePathVec_push(&ft->paths, copy) != 0) {
        free(copy);
        fprintf(stderr, "error: tape line path table grow failed for '%s'\n",
                ft->path ? ft->path : "<input>");
        return NULL;
    }
    return copy;
}

static int tape_line_push(FileTape* ft, size_t off, int logic, int phys,
                          const char* file) {
    TapeLineRec r;
    r.off = off;
    r.logic = logic;
    r.phys = phys;
    r.file = file ? file : (ft && ft->path ? ft->path : "");
    if (!ft || TapeLineVec_push(&ft->lines, r) != 0) {
        fprintf(stderr, "error: tape line index grow failed for '%s'\n",
                ft && ft->path ? ft->path : "<input>");
        return 0;
    }
    return 1;
}

/* One walk: record logical line + sticky path at each physical line start. */
static int tape_lines_build(FileTape* ft) {
    const char* bytes;
    size_t len, i;
    int phys = 1, logic = 1;
    const char* cur_file;
    if (!ft || !ft->bytes) {
        fprintf(stderr, "error: tape line index build with no bytes\n");
        return 0;
    }
    bytes = ft->bytes;
    len = ft->len;
    ft->lines = TapeLineVec_init();
    ft->paths = TapePathVec_init();
    cur_file = ft->path ? ft->path : "";
    if (TapeLineVec_reserve(&ft->lines, len / 32 + 8) != 0) {
        fprintf(stderr, "error: tape line index reserve failed for '%s'\n",
                ft->path ? ft->path : "<input>");
        return 0;
    }
    if (!tape_line_push(ft, 0, logic, phys, cur_file)) return 0;
    i = 0;
    while (i < len) {
        int at_bol = (i == 0 || bytes[i - 1] == '\n');
        if (at_bol && i + 5 < len && bytes[i] == '#' &&
            strncmp(bytes + i, "#line", 5) == 0 &&
            (bytes[i + 5] == ' ' || bytes[i + 5] == '\t')) {
            size_t j = i + 5;
            int n = 0;
            while (j < len && (bytes[j] == ' ' || bytes[j] == '\t')) j++;
            while (j < len && bytes[j] >= '0' && bytes[j] <= '9') {
                n = n * 10 + (bytes[j] - '0');
                j++;
            }
            while (j < len && (bytes[j] == ' ' || bytes[j] == '\t')) j++;
            if (j < len && bytes[j] == '"') {
                size_t k = ++j;
                while (k < len && bytes[k] != '"' && bytes[k] != '\n') k++;
                if (k < len && bytes[k] == '"' && k > j) {
                    const char* interned = tape_intern_path(ft, bytes + j, k - j);
                    if (!interned) return 0;
                    cur_file = interned;
                }
            }
            logic = n;
            while (i < len && bytes[i] != '\n') i++;
            if (i < len && bytes[i] == '\n') {
                i++;
                phys++;
                if (!tape_line_push(ft, i, logic, phys, cur_file)) return 0;
            }
            continue;
        }
        if (at_bol) {
            size_t ss = i;
            while (ss < len && (bytes[ss] == ' ' || bytes[ss] == '\t')) ss++;
            if (ss + 8 < len && memcmp(bytes + ss, "/*CC_LN ", 8) == 0) {
                size_t j = ss + 8;
                int n = 0;
                while (j < len && bytes[j] >= '0' && bytes[j] <= '9') {
                    n = n * 10 + (bytes[j] - '0');
                    j++;
                }
                if (j < len && bytes[j] == ' ') {
                    size_t k = j + 1;
                    size_t pe = k;
                    while (pe + 1 < len &&
                           !(bytes[pe] == '*' && bytes[pe + 1] == '/'))
                        pe++;
                    if (pe + 1 < len && bytes[pe] == '*') {
                        size_t nlen = pe - k;
                        const char* interned;
                        while (nlen && bytes[k + nlen - 1] == ' ') nlen--;
                        interned = tape_intern_path(ft, bytes + k, nlen);
                        if (!interned) return 0;
                        cur_file = interned;
                        logic = n;
                        while (i < len && bytes[i] != '\n') i++;
                        if (i < len && bytes[i] == '\n') {
                            i++;
                            phys++;
                            if (!tape_line_push(ft, i, logic, phys, cur_file))
                                return 0;
                        }
                        continue;
                    }
                }
            }
        }
        if (bytes[i] == '\n') {
            phys++;
            logic++;
            i++;
            if (!tape_line_push(ft, i, logic, phys, cur_file)) return 0;
            continue;
        }
        i++;
    }
    return 1;
}

static const TapeLineRec* tape_line_at(const FileTape* ft, size_t off) {
    const TapeLineRec* recs;
    size_t n, lo, hi, q;
    if (!ft) {
        fprintf(stderr, "error: tape line index query with no tape\n");
        return NULL;
    }
    recs = ft->lines.data;
    n = ft->lines.len;
    if (!recs || n == 0) {
        fprintf(stderr, "error: tape line index empty for '%s'\n",
                ft->path ? ft->path : "<input>");
        return NULL;
    }
    q = (ft->len && off > ft->len) ? ft->len : off;
    if (q < recs[0].off) return &recs[0];
    lo = 0;
    hi = n;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (recs[mid].off <= q) lo = mid;
        else hi = mid;
    }
    return &recs[lo];
}

static void offset_to_linecol(const FileTape* ft, size_t off, int* out_line,
                              int* out_col) {
    const TapeLineRec* rec;
    size_t q;
    if (!out_line || !out_col) return;
    rec = tape_line_at(ft, off);
    if (!rec) {
        *out_line = 1;
        *out_col = 1;
        return;
    }
    q = (ft && ft->len && off > ft->len) ? ft->len : off;
    *out_line = rec->phys;
    *out_col = (int)(q - rec->off) + 1;
    if (*out_col < 1) *out_col = 1;
}

/* Driver wraps (`unit_native` / `shcc_native`) are not a user locus. */
static int tape_path_is_cache_wrap(const char* p) {
    if (!p || !p[0]) return 0;
    return strstr(p, "/unit_native/") != NULL ||
           strstr(p, "/shcc_native/") != NULL;
}

/* Leading `#line N "path"` on a wrap — the file the user wrote. */
static int tape_lead_line_file(const char* bytes, size_t len, char* dst,
                               size_t cap) {
    size_t i = 0;
    size_t k;
    size_t nlen;
    if (!bytes || !dst || cap < 2) return 0;
    dst[0] = 0;
    if (len >= 3 && (unsigned char)bytes[0] == 0xef &&
        (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf)
        i = 3;
    if (i + 5 >= len || strncmp(bytes + i, "#line", 5) != 0) return 0;
    i += 5;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    while (i < len && bytes[i] >= '0' && bytes[i] <= '9') i++;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    if (i >= len || bytes[i] != '"') return 0;
    i++;
    k = i;
    while (k < len && bytes[k] != '"' && bytes[k] != '\n') k++;
    if (k >= len || bytes[k] != '"') return 0;
    nlen = k - i;
    if (nlen == 0 || nlen >= cap) return 0;
    memcpy(dst, bytes + i, nlen);
    dst[nlen] = 0;
    return 1;
}

/* Logical file:line at byte offset, honoring `#line` / CC_LN (from the index).
 * A cache wrap path is never reported — use the stamped user file. */
static void tape_logical_at(const FileTape* ft, size_t off, char* file,
                            size_t fcap, int* line_out) {
    const TapeLineRec* rec;
    const char* cur;
    if (!line_out) return;
    rec = tape_line_at(ft, off);
    if (!rec) {
        *line_out = 1;
        if (file && fcap)
            snprintf(file, fcap, "%s", ft && ft->path ? ft->path : "");
    } else {
        *line_out = rec->logic > 0 ? rec->logic : rec->phys;
        if (file && fcap) {
            cur = rec->file ? rec->file : (ft && ft->path ? ft->path : "");
            snprintf(file, fcap, "%s", cur);
        }
    }
    if (file && fcap && tape_path_is_cache_wrap(file) && ft && ft->bytes) {
        char lead[1024];
        if (tape_lead_line_file(ft->bytes, ft->len, lead, sizeof(lead)))
            snprintf(file, fcap, "%s", lead);
    }
}

/* Path for a diagnostic: logical `#line` file, never the driver wrap. */
static const char* tape_diag_file(const FileTape* ft, size_t off, char* buf,
                                 size_t cap) {
    int line = 1;
    if (!buf || !cap) return ft && ft->path && ft->path[0] ? ft->path : "<input>";
    buf[0] = 0;
    tape_logical_at(ft, off, buf, cap, &line);
    if (buf[0]) return buf;
    return ft && ft->path && ft->path[0] ? ft->path : "<input>";
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
    char lfile[1024];
    offset_to_linecol(ft, t.offset, &line, &col);
    tape_logical_at(ft, t.offset, lfile, sizeof(lfile), &line);
    fprintf(stderr, "%s:%d:%d: error: %s\n", tape_diag_file(ft, t.offset, lfile,
                                                           sizeof(lfile)),
            line, col, msg);
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
            offset_to_linecol(b->building, off, &line, &col);
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

static void file_tape_free(FileTape* ft) {
    size_t i, n;
    if (!ft) return;
    n = TapePathVec_len(&ft->paths);
    for (i = 0; i < n; i++) free(ft->paths.data[i]);
    TapePathVec_free(&ft->paths);
    TapeLineVec_free(&ft->lines);
    free(ft->path);
    free(ft->bytes);
    free(ft->toks);
    free(ft);
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
    shadow_blank_unit_header(spliced, m);
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
        shadow_blank_unit_header(spliced, m);
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
        file_tape_free(ft);
        return NULL;
    }
    if (!tape_lines_build(ft)) {
        file_tape_free(ft);
        return NULL;
    }

    TokBuild b = {.toks = ft->toks, .n = 0, .cap = TOK_BUILD_CAP, .building = ft};
    if (!PpTok_collect(ft->bytes, ft->len, CC__ARENA_HANDLE(arena),
                       tok_build_push, &b)) {
        fprintf(stderr,
                "error: tokenize failed for '%s' (often arena exhaustion)\n",
                path ? path : "<null>");
        file_tape_free(ft);
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
    for (int i = 0; i < c->n; i++) file_tape_free(c->items[i]);
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

/* If `bytes` start with `#line N "path"`, write dirname(path) to dst.
 * Unit-header wraps stamp this so quoted includes resolve from the original
 * unit directory, not the cache copy. */
static int shadow_quote_dir_from_bytes(const char* bytes, size_t len,
                                       char* dst, size_t cap) {
    size_t i = 0;
    size_t k;
    size_t nlen;
    char path[512];
    char* slash;
    if (!bytes || !dst || cap < 2) return 0;
    dst[0] = 0;
    if (len >= 3 && (unsigned char)bytes[0] == 0xef &&
        (unsigned char)bytes[1] == 0xbb && (unsigned char)bytes[2] == 0xbf)
        i = 3;
    if (i + 5 >= len || strncmp(bytes + i, "#line", 5) != 0) return 0;
    i += 5;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    while (i < len && bytes[i] >= '0' && bytes[i] <= '9') i++;
    while (i < len && (bytes[i] == ' ' || bytes[i] == '\t')) i++;
    if (i >= len || bytes[i] != '"') return 0;
    i++;
    k = i;
    while (k < len && bytes[k] != '"' && bytes[k] != '\n') k++;
    if (k >= len || bytes[k] != '"') return 0;
    nlen = k - i;
    if (nlen == 0 || nlen >= sizeof(path)) return 0;
    memcpy(path, bytes + i, nlen);
    path[nlen] = 0;
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dst, cap, ".");
        return 1;
    }
    if (slash == path) {
        snprintf(dst, cap, "/");
        return 1;
    }
    *slash = 0;
    snprintf(dst, cap, "%s", path);
    return 1;
}

/* Quoted-include root for a path: SHADOW_QUOTE_DIR, else #line dirname,
 * else dirname(path). Always writes dst. */
static void shadow_fill_quote_dir(const char* path, char* dst, size_t cap) {
    const char* env;
    FILE* f;
    char buf[2048];
    size_t n;
    if (!dst || cap < 2) return;
    dst[0] = 0;
    env = getenv("SHADOW_QUOTE_DIR");
    if (env && env[0]) {
        snprintf(dst, cap, "%s", env);
        return;
    }
    if (path && path[0]) {
        f = fopen(path, "rb");
        if (f) {
            n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[n] = 0;
            if (shadow_quote_dir_from_bytes(buf, n, dst, cap))
                return;
        }
        snprintf(dst, cap, "%s", path);
        {
            char* slash = strrchr(dst, '/');
            if (!slash) {
                snprintf(dst, cap, ".");
                return;
            }
            if (slash == dst) {
                dst[1] = 0;
                return;
            }
            *slash = 0;
        }
        return;
    }
    snprintf(dst, cap, ".");
}

