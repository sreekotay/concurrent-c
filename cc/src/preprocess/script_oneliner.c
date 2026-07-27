#include "script_oneliner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

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

void cc_script_oneliner_scan_predecls(const char* src, size_t len,
                                      CCScriptOnelinerPredecls* out) {
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!src || !out) return;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < len && src[i + 1] == '/') {
                in_block = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            in_line = 1;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
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
        if (!cc_is_ident_start(c)) continue;
        if (cc_match_ident_kw(src, len, i, "args")) {
            out->want_args = 1;
            i += 3;
            continue;
        }
        if (cc_match_ident_kw(src, len, i, "line")) {
            out->want_line = 1;
            i += 3;
            continue;
        }
        if (cc_match_ident_kw(src, len, i, "nr")) {
            out->want_nr = 1;
            i += 1;
            continue;
        }
        if (cc_match_ident_kw(src, len, i, "io")) {
            out->want_io = 1;
            i += 1;
            continue;
        }
        if (cc_match_ident_kw(src, len, i, "in")) {
            out->want_in = 1;
            i += 1;
            continue;
        }
        if (cc_match_ident_kw(src, len, i, "a")) {
            out->want_a = 1;
            continue;
        }
        while (i + 1 < len && cc_is_ident_char(src[i + 1])) i++;
    }
    cc__ol_apply_implications(out);
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

static char* cc__ol_wrap_expr_print(const char* src, size_t len, size_t* out_len) {
    size_t a, b, elen, need, o;
    char* out;
    elen = cc__ol_trim_expr(src, len, &a, &b);
    /* io.println(@string(`${EXPR}`)) !>;\n */
    need = sizeof("io.println(@string(`${") - 1 + elen +
           sizeof("}`)) !>;\n") - 1 + 1;
    out = (char*)malloc(need);
    if (!out) return NULL;
    o = 0;
    memcpy(out + o, "io.println(@string(`${", 22);
    o += 22;
    if (elen) {
        memcpy(out + o, src + a, elen);
        o += elen;
    }
    memcpy(out + o, "}`)) !>;\n", sizeof("}`)) !>;\n") - 1);
    o += sizeof("}`)) !>;\n") - 1;
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
    static const char print[] = "    io.println(line) !>;\n";
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
        n += sizeof("CCArena a = @create(megabytes(1)) @destroy;\n") - 1;
    if (p->want_io)
        n += sizeof("CCStdio io = @create(&a) @destroy;\n") - 1;
    if (p->want_in)
        n += sizeof("char[:] in = io.read_all() !>;\n") - 1;
    if (p->want_args)
        n += sizeof("char *[:] args = { .ptr = (char *)(argv + 1), "
                    ".len = (size_t)(argc > 1 ? argc - 1 : 0) };\n") -
             1;
    /* line/nr are declared inside the -n loop, not as ambient predecls. */
    if (n) n += 1;
    return n;
}

static size_t cc__ol_append_predecls(char* out, size_t o, size_t cap,
                                     const CCScriptOnelinerPredecls* p) {
    if (!out || !p) return o;
    if (p->want_a) {
        static const char s[] = "CCArena a = @create(megabytes(1)) @destroy;\n";
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_io) {
        static const char s[] = "CCStdio io = @create(&a) @destroy;\n";
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_in) {
        static const char s[] = "char[:] in = io.read_all() !>;\n";
        size_t n = sizeof(s) - 1;
        if (o + n < cap) memcpy(out + o, s, n);
        o += n;
    }
    if (p->want_args) {
        static const char s[] =
            "char *[:] args = { .ptr = (char *)(argv + 1), "
            ".len = (size_t)(argc > 1 ? argc - 1 : 0) };\n";
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
    size_t i;
    if (!name || !name[0]) return 0;
    if (!cc_is_ident_start(name[0])) return 0;
    for (i = 1; name[i]; i++) {
        if (!cc_is_ident_char(name[i])) return 0;
    }
    return 1;
}

int cc_script_oneliner_task_exists(const char* toolbox, size_t len,
                                   const char* name) {
    size_t nlen;
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;
    int brace = 0, paren = 0, brack = 0;
    if (!toolbox || !name || !name[0]) return 0;
    nlen = strlen(name);
    for (size_t i = 0; i < len; i++) {
        char c = toolbox[i];
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < len && toolbox[i + 1] == '/') {
                in_block = 0;
                i++;
            }
            continue;
        }
        if (in_str) {
            if (c == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (c == '"') in_str = 0;
            continue;
        }
        if (in_chr) {
            if (c == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (c == '\'') in_chr = 0;
            continue;
        }
        if (c == '/' && i + 1 < len && toolbox[i + 1] == '/') {
            in_line = 1;
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && toolbox[i + 1] == '*') {
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
        if (c == '{') {
            brace++;
            continue;
        }
        if (c == '}') {
            if (brace) brace--;
            continue;
        }
        if (c == '(') {
            paren++;
            continue;
        }
        if (c == ')') {
            if (paren) paren--;
            continue;
        }
        if (c == '[') {
            brack++;
            continue;
        }
        if (c == ']') {
            if (brack) brack--;
            continue;
        }
        if (brace != 0 || paren != 0 || brack != 0) continue;
        if (!cc_match_ident_kw(toolbox, len, i, name)) continue;
        {
            size_t j = i + nlen;
            j = cc_skip_ws_and_comments(toolbox, len, j);
            if (j < len && toolbox[j] == '(') return 1;
        }
        i += nlen - 1;
    }
    return 0;
}

static size_t cc__ol_indent_body(char* out, size_t o, size_t cap,
                                 const char* body, size_t body_len) {
    size_t i = 0;
    int at_line = 1;
    while (i < body_len) {
        if (at_line) {
            if (o + 4 < cap) memcpy(out + o, "    ", 4);
            o += 4;
            at_line = 0;
        }
        if (o + 1 < cap) out[o] = body[i];
        o++;
        if (body[i] == '\n') at_line = 1;
        i++;
    }
    if (body_len > 0 && body[body_len - 1] != '\n') {
        if (o + 1 < cap) out[o] = '\n';
        o++;
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
    const char* summary;
    size_t need, o;
    char* out;
    int hdr_n;

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

    need = 128 + strlen(name) + strlen(summary) + inj_len * 2 + 64;
    out = (char*)malloc(need);
    if (!out) {
        free(injected);
        free(auto_doc);
        return NULL;
    }
    o = 0;
    hdr_n = snprintf(out, need,
                     "/**\n"
                     " * @task %s\n"
                     " */\n"
                     "static int %s(int argc, char **argv) {\n",
                     summary[0] ? summary : name, name);
    if (hdr_n < 0 || (size_t)hdr_n >= need) {
        free(out);
        free(injected);
        free(auto_doc);
        return NULL;
    }
    o = (size_t)hdr_n;
    o = cc__ol_indent_body(out, o, need, injected, inj_len);
    {
        static const char tail[] = "    return 0;\n}\n";
        size_t n = sizeof(tail) - 1;
        if (o + n + 1 > need) {
            size_t nneed = o + n + 1;
            char* grown = (char*)realloc(out, nneed);
            if (!grown) {
                free(out);
                free(injected);
                free(auto_doc);
                return NULL;
            }
            out = grown;
            need = nneed;
        }
        memcpy(out + o, tail, n);
        o += n;
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    free(injected);
    free(auto_doc);
    return out;
}
