#include "script_oneliner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "template_scan.h"
#include "util/text.h"

/* Text of the injected predeclarations, in one place: three sites emit them
 * (byte accounting, the file-.shcc emitter, the one-liner emitter) and they
 * have to agree.
 *
 * The implicit `io` binds an arena of its own rather than the script's `a`.
 * Predecls are emitted above the script body, so an `io` bound to `a` broke
 * every script that declared `a` itself — the reference sat above the
 * declaration. A private arena decouples the two orders entirely.
 *
 * Sized to match the arena `io` was bound to before, because cc_arena_heap
 * takes a single fixed block: the size is a cap, and read_all/read_line
 * allocate the input into it. */
#define CC_OL_PREDECL_A "CCArena a = @create(megabytes(1)) @destroy;\n"
#define CC_OL_PREDECL_IO_ARENA \
    "CCArena __cc_io_arena = @create(megabytes(1)) @destroy;\n"
#define CC_OL_PREDECL_IO "CCStdio io = @create(&__cc_io_arena) @destroy;\n"
#define CC_OL_PREDECL_IN "char[:] in = io.read_all() !>;\n"
/* Injected as erased CCSlice — same shape legacy lowers `char *[:]` to.
 * Keep the sugar out of the magic text so native shadow does not need it. */
#define CC_OL_PREDECL_ARGS                                                 \
    "CCSlice args = { .ptr = (char *)(argv + 1), "                         \
    ".len = (size_t)(argc > 1 ? argc - 1 : 0) };\n"

static void cc__ol_apply_implications(CCScriptOnelinerPredecls* p) {
    if (!p) return;
    if (p->want_line || p->want_nr) {
        p->want_line = 1;
        p->want_nr = 1;
        p->want_io = 1;
    }
    if (p->want_in) p->want_io = 1;
    if (p->want_io) p->want_a = 1;
}

typedef struct {
    void (*on_ident)(const char* src, size_t len, size_t i, void* ctx);
    void (*on_punct)(char c, void* ctx);
    void* ctx;
    int recurse_holes; /* 1: predecls (${in} counts); 0: task_exists skip tpl */
} CCOlCodeWalk;

/* Walk [a,b) as Concurrent-C code: skip C strings/chars/comments; on backtick
 * templates, treat literal/verbatim text as inert. When recurse_holes, walk
 * ${…} hole bodies as code (canonical lexer via template_scan). */
static void cc__ol_walk_code(const char* src, size_t a, size_t b,
                             const CCOlCodeWalk* walk) {
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;
    size_t i;
    if (!src || a >= b || !walk) return;

    for (i = a; i < b; i++) {
        char c = src[i];
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < b && src[i + 1] == '/') {
                in_block = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < b) {
                i++;
                continue;
            }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < b) {
                i++;
                continue;
            }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && i + 1 < b && src[i + 1] == '/') {
            in_line = 1;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < b && src[i + 1] == '*') {
            in_block = 1;
            i++;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            continue;
        }
        if (c == '\'') {
            in_chr = 1;
            continue;
        }
        if (c == '`') {
            size_t tick_end = 0;
            if (cc_tpl_scan_literal(src, b, i, &tick_end) != 0) return;
            if (tick_end > b) tick_end = b;
            if (walk->recurse_holes) {
                size_t pos = i + 1;
                while (pos < tick_end) {
                    CCTemplatePiece piece;
                    int r = cc_template_next_piece(src, b, i + 1, tick_end, &pos,
                                                   &piece);
                    if (r < 0) return;
                    if (r == 0) break;
                    if (piece.kind == CC_TPL_PIECE_SLOT ||
                        piece.kind == CC_TPL_PIECE_TAGGED_SLOT) {
                        cc__ol_walk_code(src, piece.expr_off,
                                         piece.expr_off + piece.expr_len, walk);
                    }
                }
            }
            i = tick_end;
            continue;
        }
        if (walk->on_punct &&
            (c == '{' || c == '}' || c == '(' || c == ')' || c == '[' ||
             c == ']')) {
            walk->on_punct(c, walk->ctx);
        }
        if (!walk->on_ident || !cc_is_ident_start(c)) continue;
        walk->on_ident(src, b, i, walk->ctx);
        while (i + 1 < b && cc_is_ident_char(src[i + 1])) i++;
    }
}

static void cc__ol_predecl_on_ident(const char* src, size_t len, size_t i,
                                    void* ctx) {
    CCScriptOnelinerPredecls* out = (CCScriptOnelinerPredecls*)ctx;
    (void)len;
    if (cc_match_ident_kw(src, len, i, "args")) {
        out->want_args = 1;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "line")) {
        out->want_line = 1;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "nr")) {
        out->want_nr = 1;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "io")) {
        out->want_io = 1;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "in")) {
        out->want_in = 1;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "a")) {
        out->want_a = 1;
    }
}

/* Which implicit names the source mentions, before any implication between
 * them. Callers that suppress a name must do so against these primaries:
 * implications turn `line`/`nr` into `io` into `a`, and clearing a name after
 * that leaves its consequences standing. */
static void cc__ol_scan_primaries(const char* src, size_t len,
                                  CCScriptOnelinerPredecls* out) {
    CCOlCodeWalk walk;
    if (out) memset(out, 0, sizeof(*out));
    if (!src || !out) return;
    walk.on_ident = cc__ol_predecl_on_ident;
    walk.on_punct = NULL;
    walk.ctx = out;
    walk.recurse_holes = 1;
    cc__ol_walk_code(src, 0, len, &walk);
}

void cc_script_oneliner_scan_predecls(const char* src, size_t len,
                                      CCScriptOnelinerPredecls* out) {
    cc__ol_scan_primaries(src, len, out);
    cc__ol_apply_implications(out);
}

static size_t cc__ol_predecl_bytes(const CCScriptOnelinerPredecls* p);

typedef struct {
    int saw_CCArena;
    int saw_CCStdio;
    int saw_CCSlice;
    int decl_a;
    int decl_io;
    int decl_in;
    int decl_args;
} CCOlDeclScan;

static void cc__ol_decl_on_ident(const char* src, size_t len, size_t i,
                                 void* ctx) {
    CCOlDeclScan* d = (CCOlDeclScan*)ctx;
    (void)len;
    if (cc_match_ident_kw(src, len, i, "CCArena")) {
        d->saw_CCArena = 1;
        d->saw_CCStdio = 0;
        d->saw_CCSlice = 0;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "CCStdio")) {
        d->saw_CCStdio = 1;
        d->saw_CCArena = 0;
        d->saw_CCSlice = 0;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "CCSlice")) {
        d->saw_CCSlice = 1;
        d->saw_CCArena = 0;
        d->saw_CCStdio = 0;
        return;
    }
    if (d->saw_CCArena && cc_match_ident_kw(src, len, i, "a")) {
        d->decl_a = 1;
        d->saw_CCArena = 0;
        return;
    }
    if (d->saw_CCStdio && cc_match_ident_kw(src, len, i, "io")) {
        d->decl_io = 1;
        d->saw_CCStdio = 0;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "in")) {
        /* Heuristic: `char[:] in` / `char *[:] in` — treat any `in` decl
         * keyword use after `]` in a short window as a declaration. Cheap
         * check: previous non-space char is ']'. */
        size_t j = i;
        while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' ||
                         src[j - 1] == '\n' || src[j - 1] == '\r'))
            j--;
        if (j > 0 && src[j - 1] == ']') d->decl_in = 1;
        d->saw_CCArena = 0;
        d->saw_CCStdio = 0;
        d->saw_CCSlice = 0;
        return;
    }
    if (cc_match_ident_kw(src, len, i, "args")) {
        size_t j = i;
        while (j > 0 && (src[j - 1] == ' ' || src[j - 1] == '\t' ||
                         src[j - 1] == '\n' || src[j - 1] == '\r'))
            j--;
        /* `char *[:] args` (legacy surface) or `CCSlice args` (injected). */
        if (j > 0 && src[j - 1] == ']') d->decl_args = 1;
        if (d->saw_CCSlice) d->decl_args = 1;
        d->saw_CCArena = 0;
        d->saw_CCStdio = 0;
        d->saw_CCSlice = 0;
        return;
    }
    d->saw_CCArena = 0;
    d->saw_CCStdio = 0;
    d->saw_CCSlice = 0;
}

void cc_script_oneliner_suppress_existing_decls(const char* src, size_t len,
                                                CCScriptOnelinerPredecls* p) {
    CCOlDeclScan d;
    CCOlCodeWalk walk;
    if (!p || !src) return;
    memset(&d, 0, sizeof(d));
    walk.on_ident = cc__ol_decl_on_ident;
    walk.on_punct = NULL;
    walk.ctx = &d;
    cc__ol_walk_code(src, 0, len, &walk);
    if (d.decl_a) p->want_a = 0;
    if (d.decl_io) p->want_io = 0;
    if (d.decl_in) p->want_in = 0;
    if (d.decl_args) p->want_args = 0;
    /* If io remains wanted but a was an explicit decl, keep want_a cleared. */
}

char* cc_script_oneliner_predecls_for(const char* src, size_t len,
                                      int allow_args, const char* indent,
                                      size_t* out_len) {
    CCScriptOnelinerPredecls p;
    size_t ind_len = (indent && indent[0]) ? strlen(indent) : 0;
    size_t lines = 0;
    size_t need;
    size_t o = 0;
    char* out;
    if (!src) {
        src = "";
        len = 0;
    }
    /* Primaries first: `line`/`nr` are -n/-p loop locals only, never ambient in
     * a file .shcc, and dropping them has to happen before implications run.
     * Scanning post-implication left `int line = 1;` pulling in an injected
     * `CCStdio io = @create(&a) @destroy;` — ahead of the script's own arena. */
    cc__ol_scan_primaries(src, len, &p);
    p.want_line = 0;
    p.want_nr = 0;
    if (!allow_args) p.want_args = 0;
    cc_script_oneliner_suppress_existing_decls(src, len, &p);
    cc__ol_apply_implications(&p);
    /* Implications must not resurrect a name the script declares itself. */
    cc_script_oneliner_suppress_existing_decls(src, len, &p);
    p.want_line = 0;
    p.want_nr = 0;
    if (p.want_a) lines++;
    if (p.want_io) lines += 2; /* private arena + the handle bound to it */
    if (p.want_in) lines++;
    if (p.want_args) lines++;
    if (lines == 0) {
        out = (char*)malloc(1);
        if (!out) return NULL;
        out[0] = '\0';
        if (out_len) *out_len = 0;
        return out;
    }
    need = cc__ol_predecl_bytes(&p) + lines * ind_len + 1;
    out = (char*)malloc(need);
    if (!out) return NULL;
    if (p.want_a) {
        static const char s[] = CC_OL_PREDECL_A;
        if (ind_len) {
            memcpy(out + o, indent, ind_len);
            o += ind_len;
        }
        memcpy(out + o, s, sizeof(s) - 1);
        o += sizeof(s) - 1;
    }
    if (p.want_io) {
        static const char s_arena[] = CC_OL_PREDECL_IO_ARENA;
        static const char s_io[] = CC_OL_PREDECL_IO;
        if (ind_len) {
            memcpy(out + o, indent, ind_len);
            o += ind_len;
        }
        memcpy(out + o, s_arena, sizeof(s_arena) - 1);
        o += sizeof(s_arena) - 1;
        if (ind_len) {
            memcpy(out + o, indent, ind_len);
            o += ind_len;
        }
        memcpy(out + o, s_io, sizeof(s_io) - 1);
        o += sizeof(s_io) - 1;
    }
    if (p.want_in) {
        static const char s[] = CC_OL_PREDECL_IN;
        if (ind_len) {
            memcpy(out + o, indent, ind_len);
            o += ind_len;
        }
        memcpy(out + o, s, sizeof(s) - 1);
        o += sizeof(s) - 1;
    }
    if (p.want_args) {
        static const char s[] = CC_OL_PREDECL_ARGS;
        if (ind_len) {
            memcpy(out + o, indent, ind_len);
            o += ind_len;
        }
        memcpy(out + o, s, sizeof(s) - 1);
        o += sizeof(s) - 1;
    }
    if (o + 1 < need) {
        out[o++] = '\n';
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

static size_t cc__ol_trim_expr(const char* src, size_t len, size_t* out_a,
                               size_t* out_b) {
    size_t a = 0, b = len;
    while (a < b && (src[a] == ' ' || src[a] == '\t' || src[a] == '\n' ||
                     src[a] == '\r'))
        a++;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t' ||
                     src[b - 1] == '\n' || src[b - 1] == '\r'))
        b--;
    if (b > a && src[b - 1] == ';') b--;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t')) b--;
    if (out_a) *out_a = a;
    if (out_b) *out_b = b;
    return b - a;
}

static int cc__ol_span_has_char(const char* src, size_t a, size_t b, char ch) {
    size_t i;
    for (i = a; i < b; i++) {
        if (src[i] == ch) return 1;
    }
    return 0;
}

static char* cc__ol_wrap_expr_print(const char* src, size_t len, size_t* out_len) {
    size_t a, b, elen, need, o;
    char* out;
    int direct;
    /* If EXPR already contains a template (backtick) or @string, print it
     * directly — nesting inside @string(`${…}`) breaks the scanner.
     * Otherwise stringify via @string(`${EXPR}`) so -E '1+2' still works. */
    elen = cc__ol_trim_expr(src, len, &a, &b);
    direct = cc__ol_span_has_char(src, a, a + elen, '`') ||
             (elen >= 7 && memcmp(src + a, "@string", 7) == 0);
    if (direct) {
        need = sizeof("cc_println(") - 1 + elen + sizeof(") !>;\n") - 1 + 1;
    } else {
        need = sizeof("cc_println(@string(`${") - 1 + elen +
               sizeof("}`)) !>;\n") - 1 + 1;
    }
    out = (char*)malloc(need);
    if (!out) return NULL;
    o = 0;
    if (direct) {
        memcpy(out + o, "cc_println(", sizeof("cc_println(") - 1);
        o += sizeof("cc_println(") - 1;
        if (elen) {
            memcpy(out + o, src + a, elen);
            o += elen;
        }
        memcpy(out + o, ") !>;\n", sizeof(") !>;\n") - 1);
        o += sizeof(") !>;\n") - 1;
    } else {
        memcpy(out + o, "cc_println(@string(`${", sizeof("cc_println(@string(`${") - 1);
        o += sizeof("cc_println(@string(`${") - 1;
        if (elen) {
            memcpy(out + o, src + a, elen);
            o += elen;
        }
        memcpy(out + o, "}`)) !>;\n", sizeof("}`)) !>;\n") - 1);
        o += sizeof("}`)) !>;\n") - 1;
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

static char* cc__ol_wrap_line_loop(const char* body, size_t body_len, int line_print,
                                   size_t* out_len) {
    static const char head[] =
        "size_t nr = 0;\n"
        "char[:] line;\n"
        "for (;;) {\n"
        "    bool __cc_more = io.read_line(&line) !>;\n"
        "    if (!__cc_more) break;\n"
        "    nr += 1;\n";
    static const char print[] = "    line.println() !>;\n";
    static const char tail[] = "}\n";
    size_t need, o, i;
    char* out;
    int at_line;

    need = sizeof(head) - 1 + body_len * 2 + sizeof(print) - 1 + sizeof(tail) - 1 +
           64;
    out = (char*)malloc(need);
    if (!out) return NULL;
    o = 0;
    memcpy(out + o, head, sizeof(head) - 1);
    o += sizeof(head) - 1;

    at_line = 1;
    for (i = 0; i < body_len; i++) {
        if (at_line) {
            if (o + 4 >= need) {
                size_t nneed = need * 2 + 64;
                char* grown = (char*)realloc(out, nneed);
                if (!grown) {
                    free(out);
                    return NULL;
                }
                out = grown;
                need = nneed;
            }
            memcpy(out + o, "    ", 4);
            o += 4;
            at_line = 0;
        }
        if (o + 2 >= need) {
            size_t nneed = need * 2 + 64;
            char* grown = (char*)realloc(out, nneed);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
            need = nneed;
        }
        out[o++] = body[i];
        if (body[i] == '\n') at_line = 1;
    }
    if (body_len > 0 && body[body_len - 1] != '\n') {
        out[o++] = '\n';
        at_line = 1;
    }
    if (line_print) {
        if (o + sizeof(print) >= need) {
            size_t nneed = o + sizeof(print) + 16;
            char* grown = (char*)realloc(out, nneed);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
            need = nneed;
        }
        memcpy(out + o, print, sizeof(print) - 1);
        o += sizeof(print) - 1;
    }
    if (o + sizeof(tail) >= need) {
        size_t nneed = o + sizeof(tail) + 8;
        char* grown = (char*)realloc(out, nneed);
        if (!grown) {
            free(out);
            return NULL;
        }
        out = grown;
        need = nneed;
    }
    memcpy(out + o, tail, sizeof(tail) - 1);
    o += sizeof(tail) - 1;
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

static size_t cc__ol_predecl_bytes(const CCScriptOnelinerPredecls* p) {
    size_t n = 0;
    if (!p) return 0;
    if (p->want_a)
        n += sizeof(CC_OL_PREDECL_A) - 1;
    if (p->want_io)
        n += sizeof(CC_OL_PREDECL_IO_ARENA) - 1 + sizeof(CC_OL_PREDECL_IO) - 1;
    if (p->want_in)
        n += sizeof(CC_OL_PREDECL_IN) - 1;
    if (p->want_args)
        n += sizeof(CC_OL_PREDECL_ARGS) - 1;
    /* line/nr are declared inside the -n loop, not as ambient predecls. */
    if (n) n += 1;
    return n;
}

static size_t cc__ol_append_predecls(char* out, size_t o, size_t cap,
                                     const CCScriptOnelinerPredecls* p) {
    if (!out || !p) return o;
    if (p->want_a) {
        static const char s[] = CC_OL_PREDECL_A;
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_io) {
        static const char s_arena[] = CC_OL_PREDECL_IO_ARENA;
        static const char s_io[] = CC_OL_PREDECL_IO;
        size_t n = sizeof(s_arena) - 1;
        if (o + n < cap) memcpy(out + o, s_arena, n);
        o += n;
        n = sizeof(s_io) - 1;
        if (o + n < cap) memcpy(out + o, s_io, n);
        o += n;
    }
    if (p->want_in) {
        static const char s[] = CC_OL_PREDECL_IN;
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_args) {
        static const char s[] = CC_OL_PREDECL_ARGS;
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_a || p->want_io || p->want_in || p->want_args) {
        if (o + 1 < cap) out[o] = '\n';
        o += 1;
    }
    return o;
}

char* cc_script_oneliner_lower(const char* src, size_t len,
                               const CCScriptOnelinerOpts* opts,
                               size_t* out_len) {
    CCScriptOnelinerOpts ozero;
    const CCScriptOnelinerOpts* o = opts;
    char* cur = NULL;
    size_t cur_len = 0;
    char* next = NULL;
    size_t next_len = 0;
    CCScriptOnelinerPredecls p;
    size_t need, pos;
    char* out;

    if (!src) {
        src = "";
        len = 0;
    }
    if (!o) {
        memset(&ozero, 0, sizeof(ozero));
        o = &ozero;
    }

    if (o->expr_print) {
        cur = cc__ol_wrap_expr_print(src, len, &cur_len);
        if (!cur) return NULL;
    } else {
        cur = (char*)malloc(len + 1);
        if (!cur) return NULL;
        if (len) memcpy(cur, src, len);
        cur[len] = '\0';
        cur_len = len;
    }

    if (o->line_loop || o->line_print) {
        next = cc__ol_wrap_line_loop(cur, cur_len, o->line_print, &next_len);
        free(cur);
        if (!next) return NULL;
        cur = next;
        cur_len = next_len;
    }

    cc_script_oneliner_scan_predecls(cur, cur_len, &p);
    if (o->expr_print || o->line_loop || o->line_print) {
        p.want_io = 1;
        cc__ol_apply_implications(&p);
    }

    need = cc__ol_predecl_bytes(&p) + cur_len + 1;
    out = (char*)malloc(need);
    if (!out) {
        free(cur);
        return NULL;
    }
    pos = cc__ol_append_predecls(out, 0, need, &p);
    if (cur_len) memcpy(out + pos, cur, cur_len);
    pos += cur_len;
    out[pos] = '\0';
    if (out_len) *out_len = pos;
    free(cur);
    return out;
}

char* cc_script_oneliner_inject_predecls(const char* src, size_t len,
                                         size_t* out_len) {
    return cc_script_oneliner_lower(src, len, NULL, out_len);
}

char* cc_script_oneliner_first_line(const char* src, size_t len) {
    size_t i = 0, a, b, n;
    char* out;
    if (!src) src = "";
    while (i < len && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' ||
                       src[i] == '\r'))
        i++;
    a = i;
    while (i < len && src[i] != '\n' && src[i] != '\r') i++;
    b = i;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == '\t')) b--;
    n = b - a;
    out = (char*)malloc(n + 1);
    if (!out) return NULL;
    if (n) memcpy(out, src + a, n);
    out[n] = '\0';
    return out;
}

int cc_script_oneliner_is_ident(const char* name) {
    /* Keywords that make `static int NAME(...)` uncompilable or reserved. */
    static const char* const kws[] = {
        "auto",    "break",  "case",   "char",   "const",   "continue",
        "default", "do",     "double", "else",   "enum",    "extern",
        "float",   "for",    "goto",   "if",     "inline",  "int",
        "long",    "main",   "register", "restrict", "return", "short",
        "signed",  "sizeof", "static", "struct", "switch",  "typedef",
        "union",   "unsigned", "void", "volatile", "while",  "_Bool",
        "_Complex", "_Imaginary", NULL
    };
    size_t i;
    if (!name || !name[0]) return 0;
    if (!cc_is_ident_start(name[0])) return 0;
    for (i = 1; name[i]; i++) {
        if (!cc_is_ident_char(name[i])) return 0;
    }
    for (i = 0; kws[i]; i++) {
        if (strcmp(name, kws[i]) == 0) return 0;
    }
    return 1;
}

typedef struct {
    const char* name;
    size_t nlen;
    int brace;
    int paren;
    int brack;
    int found;
} CCOlTaskExistsCtx;

static void cc__ol_exists_on_punct(char c, void* ctx) {
    CCOlTaskExistsCtx* e = (CCOlTaskExistsCtx*)ctx;
    if (c == '{') e->brace++;
    else if (c == '}') {
        if (e->brace) e->brace--;
    } else if (c == '(') e->paren++;
    else if (c == ')') {
        if (e->paren) e->paren--;
    } else if (c == '[') e->brack++;
    else if (c == ']') {
        if (e->brack) e->brack--;
    }
}

static void cc__ol_exists_on_ident(const char* src, size_t len, size_t i,
                                   void* ctx) {
    CCOlTaskExistsCtx* e = (CCOlTaskExistsCtx*)ctx;
    size_t j;
    if (e->found) return;
    if (e->brace != 0 || e->paren != 0 || e->brack != 0) return;
    if (!cc_match_ident_kw(src, len, i, e->name)) return;
    j = cc_skip_ws_and_comments(src, len, i + e->nlen);
    if (j < len && src[j] == '(') e->found = 1;
}

int cc_script_oneliner_task_exists(const char* toolbox, size_t len,
                                   const char* name) {
    CCOlTaskExistsCtx e;
    CCOlCodeWalk walk;
    if (!toolbox || !name || !name[0]) return 0;
    memset(&e, 0, sizeof(e));
    e.name = name;
    e.nlen = strlen(name);
    walk.on_ident = cc__ol_exists_on_ident;
    walk.on_punct = cc__ol_exists_on_punct;
    walk.ctx = &e;
    walk.recurse_holes = 0; /* skip whole templates; names are never inside */
    cc__ol_walk_code(toolbox, 0, len, &walk);
    return e.found;
}

/* Escape summary text for embedding in a CCDoc block (no comment delimiters). */
static char* cc__ol_escape_doc_summary(const char* summary, size_t* out_len) {
    size_t i, o, need;
    char* out;
    if (!summary) summary = "";
    need = 1;
    for (i = 0; summary[i]; i++) {
        if (summary[i] == '*' && summary[i + 1] == '/') need += 2; /* * / */
        else if (summary[i] == '/' && summary[i + 1] == '*') need += 2; /* / * */
        else if (summary[i] == '\n' || summary[i] == '\r') need += 1; /* space */
        else need += 1;
    }
    out = (char*)malloc(need);
    if (!out) return NULL;
    o = 0;
    for (i = 0; summary[i]; i++) {
        if (summary[i] == '*' && summary[i + 1] == '/') {
            out[o++] = '*';
            out[o++] = ' ';
            out[o++] = '/';
            i++;
        } else if (summary[i] == '/' && summary[i + 1] == '*') {
            out[o++] = '/';
            out[o++] = ' ';
            out[o++] = '*';
            i++;
        } else if (summary[i] == '\n' || summary[i] == '\r') {
            if (summary[i] == '\r' && summary[i + 1] == '\n') i++;
            out[o++] = ' ';
        } else {
            out[o++] = summary[i];
        }
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

static size_t cc__ol_indent_body_len(const char* body, size_t body_len) {
    size_t i = 0, o = 0;
    int at_line = 1;
    while (i < body_len) {
        if (at_line) {
            o += 4;
            at_line = 0;
        }
        o++;
        if (body[i] == '\n') at_line = 1;
        i++;
    }
    if (body_len > 0 && body[body_len - 1] != '\n') o++;
    return o;
}

static size_t cc__ol_indent_body(char* out, size_t o, size_t cap,
                                 const char* body, size_t body_len) {
    size_t i = 0;
    int at_line = 1;
    while (i < body_len) {
        if (at_line) {
            if (o + 4 > cap) return (size_t)-1;
            memcpy(out + o, "    ", 4);
            o += 4;
            at_line = 0;
        }
        if (o + 1 > cap) return (size_t)-1;
        out[o++] = body[i];
        if (body[i] == '\n') at_line = 1;
        i++;
    }
    if (body_len > 0 && body[body_len - 1] != '\n') {
        if (o + 1 > cap) return (size_t)-1;
        out[o++] = '\n';
    }
    return o;
}

char* cc_script_oneliner_format_task(const char* name, const char* doc,
                                     const char* program, size_t program_len,
                                     const CCScriptOnelinerOpts* opts,
                                     size_t* out_len) {
    char* injected = NULL;
    size_t inj_len = 0;
    char* auto_doc = NULL;
    char* safe_doc = NULL;
    size_t safe_doc_len = 0;
    const char* summary;
    size_t need, o, indented_need;
    char* out;
    int hdr_n;
    static const char tail[] = "    return 0;\n}\n";

    if (!name) return NULL;
    if (!program) {
        program = "";
        program_len = 0;
    }

    injected = cc_script_oneliner_lower(program, program_len, opts, &inj_len);
    if (!injected) return NULL;

    summary = doc;
    if (!summary || !summary[0]) {
        auto_doc = cc_script_oneliner_first_line(program, program_len);
        summary = auto_doc ? auto_doc : "";
    }
    safe_doc = cc__ol_escape_doc_summary(summary, &safe_doc_len);
    if (!safe_doc) {
        free(injected);
        free(auto_doc);
        return NULL;
    }

    indented_need = cc__ol_indent_body_len(injected, inj_len);
    need = 64 + strlen(name) + safe_doc_len + indented_need + sizeof(tail) + 8;
    out = (char*)malloc(need);
    if (!out) {
        free(injected);
        free(auto_doc);
        free(safe_doc);
        return NULL;
    }
    o = 0;
    hdr_n = snprintf(out, need,
                     "/**\n"
                     " * @task %s\n"
                     " */\n"
                     "static int %s(int argc, char **argv) {\n",
                     safe_doc[0] ? safe_doc : name, name);
    if (hdr_n < 0 || (size_t)hdr_n >= need) {
        free(out);
        free(injected);
        free(auto_doc);
        free(safe_doc);
        return NULL;
    }
    o = (size_t)hdr_n;
    o = cc__ol_indent_body(out, o, need - sizeof(tail), injected, inj_len);
    if (o == (size_t)-1) {
        free(out);
        free(injected);
        free(auto_doc);
        free(safe_doc);
        return NULL;
    }
    {
        size_t n = sizeof(tail) - 1;
        if (o + n + 1 > need) {
            free(out);
            free(injected);
            free(auto_doc);
            free(safe_doc);
            return NULL;
        }
        memcpy(out + o, tail, n);
        o += n;
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    free(injected);
    free(auto_doc);
    free(safe_doc);
    return out;
}
