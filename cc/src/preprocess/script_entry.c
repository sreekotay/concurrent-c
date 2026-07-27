#include "script_entry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

int cc_path_is_ccscript(const char* path) {
    if (!path) return 0;
    size_t n = strlen(path);
    return n >= 9 && strcmp(path + n - 9, ".ccscript") == 0;
}

/* True when a top-level `main` function definition is present. */
static int cc__script_has_toplevel_main(const char* s, size_t n) {
    int brace = 0;
    int paren = 0;
    int brack = 0;
    int in_str = 0;
    int in_chr = 0;
    int in_line = 0;
    int in_block = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (in_line) {
            if (c == '\n') in_line = 0;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < n && s[i + 1] == '/') {
                in_block = 0;
                i++;
            }
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
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { in_line = 1; i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { in_block = 1; i++; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '\'') { in_chr = 1; continue; }
        if (c == '{') { brace++; continue; }
        if (c == '}') { if (brace > 0) brace--; continue; }
        if (c == '(') { paren++; continue; }
        if (c == ')') { if (paren > 0) paren--; continue; }
        if (c == '[') { brack++; continue; }
        if (c == ']') { if (brack > 0) brack--; continue; }
        if (brace != 0 || paren != 0 || brack != 0) continue;
        if (c != 'm' || i + 4 > n || memcmp(s + i, "main", 4) != 0) continue;
        if (i > 0 && cc_is_ident_char(s[i - 1])) continue;
        if (i + 4 < n && cc_is_ident_char(s[i + 4])) continue;
        size_t j = cc_skip_ws_and_comments(s, n, i + 4);
        if (j < n && s[j] == '(') return 1;
    }
    return 0;
}

static size_t cc__script_skip_shebang(const char* s, size_t n) {
    if (n >= 2 && s[0] == '#' && s[1] == '!') {
        size_t i = 2;
        while (i < n && s[i] != '\n') i++;
        if (i < n) i++;
        return i;
    }
    return 0;
}

char* cc_script_rewrite_source(const char* path,
                               const char* src,
                               size_t len,
                               size_t* out_len) {
    if (!cc_path_is_ccscript(path) || !src) return NULL;

    size_t body_off = cc__script_skip_shebang(src, len);
    const char* body = src + body_off;
    size_t body_len = len - body_off;
    int has_main = cc__script_has_toplevel_main(body, body_len);

    /* Include only at TU scope. Default @errhandler lives inside main so
     * statement-start rewinds for `!>` / `@err` stay within the function
     * (file-scope handler + synthetic main confused pass_err_syntax). */
    static const char prelude[] =
        "/* .ccscript entry: auto prelude */\n"
        "#include <ccc/script/prelude.cch>\n"
        "\n";
    static const char default_eh[] =
        "    @errhandler(CCError e) {\n"
        "        fprintf(stderr, \"%s\\n\", e.message);\n"
        "        return 1;\n"
        "    }\n"
        "\n";

    size_t pre_len = sizeof(prelude) - 1;
    size_t eh_len = sizeof(default_eh) - 1;
    size_t need;
    char* out;

    if (has_main) {
        /* Explicit main: prelude only; user supplies @errhandler if needed. */
        need = pre_len + body_len + 1;
        out = (char*)malloc(need);
        if (!out) return NULL;
        memcpy(out, prelude, pre_len);
        memcpy(out + pre_len, body, body_len);
        out[pre_len + body_len] = '\0';
        if (out_len) *out_len = pre_len + body_len;
        return out;
    }

    static const char main_open[] =
        "int main(int argc, char **argv) {\n"
        "    (void)argc;\n"
        "    (void)argv;\n";
    static const char main_close[] =
        "\n    return 0;\n"
        "}\n";
    size_t open_len = sizeof(main_open) - 1;
    size_t close_len = sizeof(main_close) - 1;
    need = pre_len + open_len + eh_len + body_len + close_len + 1;
    out = (char*)malloc(need);
    if (!out) return NULL;
    size_t o = 0;
    memcpy(out + o, prelude, pre_len); o += pre_len;
    memcpy(out + o, main_open, open_len); o += open_len;
    memcpy(out + o, default_eh, eh_len); o += eh_len;
    memcpy(out + o, body, body_len); o += body_len;
    memcpy(out + o, main_close, close_len); o += close_len;
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}
