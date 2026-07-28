#include "script_entry.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_oneliner.h"
#include "util/text.h"

int cc_path_is_shcc(const char* path) {
    if (!path) return 0;
    size_t n = strlen(path);
    return n >= 5 && strcmp(path + n - 5, ".shcc") == 0;
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

/* ---- Top-level body split: TU chunks vs synthetic-main statements ---- */

typedef enum { CC__CHUNK_TU = 0, CC__CHUNK_MAIN = 1 } CC__ChunkKind;

typedef struct {
    size_t a, b; /* half-open range into body */
    CC__ChunkKind kind;
} CC__Chunk;

static int cc__starts_with_kw(const char* s, size_t n, size_t i, const char* kw) {
    return cc_match_ident_kw(s, n, i, kw);
}

/* Skip optional storage-class / function-specifier keywords. */
static size_t cc__skip_storage(const char* s, size_t n, size_t i) {
    for (;;) {
        i = cc_skip_ws_and_comments(s, n, i);
        if (cc__starts_with_kw(s, n, i, "static") ||
            cc__starts_with_kw(s, n, i, "extern") ||
            cc__starts_with_kw(s, n, i, "inline") ||
            cc__starts_with_kw(s, n, i, "constexpr")) {
            while (i < n && cc_is_ident_char(s[i])) i++;
            continue;
        }
        return i;
    }
}

/*
 * After a top-level `(...)`, skip optional throws clause `!>(...)`.
 * Bare `!>` (error propagate) is left alone.
 */
static size_t cc__skip_throws_clause(const char* s, size_t n, size_t i) {
    i = cc_skip_ws_and_comments(s, n, i);
    if (i + 1 < n && s[i] == '!' && s[i + 1] == '>') {
        size_t j = cc_skip_ws_and_comments(s, n, i + 2);
        if (j < n && s[j] == '(') {
            size_t r = 0;
            if (cc_find_matching_paren(s, n, j, &r))
                return r + 1;
        }
    }
    return i;
}

/*
 * `lpar` opens a top-level `(...)`. True when it looks like a function
 * declarator (return type + name), not a call / method call / @intrinsic.
 */
static int cc__looks_like_func_declarator(const char* s, size_t n,
                                          size_t item_start, size_t lpar) {
    size_t j;
    int paren = 0, brace = 0, brack = 0;
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;
    if (lpar == 0 || lpar > n) return 0;

    /* `int v = add(2, 3);` — depth-0 `=` means the `(...)` is a call in an
     * initializer, not a function declarator / prototype. */
    for (size_t i = item_start; i < lpar; i++) {
        char c = s[i];
        if (in_line) { if (c == '\n') in_line = 0; continue; }
        if (in_block) {
            if (c == '*' && i + 1 < n && s[i + 1] == '/') { in_block = 0; i++; }
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
        if (c == '(') { paren++; continue; }
        if (c == ')') { if (paren) paren--; continue; }
        if (c == '[') { brack++; continue; }
        if (c == ']') { if (brack) brack--; continue; }
        if (c == '{') { brace++; continue; }
        if (c == '}') { if (brace) brace--; continue; }
        if (c == '=' && paren == 0 && brace == 0 && brack == 0) return 0;
    }

    j = lpar;
    while (j > item_start && (s[j - 1] == ' ' || s[j - 1] == '\t' ||
                              s[j - 1] == '\n' || s[j - 1] == '\r'))
        j--;
    if (j == item_start || !cc_is_ident_char(s[j - 1])) return 0;
    while (j > item_start && cc_is_ident_char(s[j - 1])) j--;
    /* method / ufcs call: expr.name( or expr->name( */
    if (j > item_start && s[j - 1] == '.') return 0;
    if (j >= item_start + 2 && s[j - 2] == '-' && s[j - 1] == '>') return 0;
    /* @create( / @destroy( style */
    if (j > item_start && s[j - 1] == '@') return 0;
    /* Call with no return type: item begins at the callee name. */
    {
        size_t t = cc__skip_storage(s, n, item_start);
        t = cc_skip_ws_and_comments(s, n, t);
        if (t == j) return 0;
    }
    return 1;
}

/* Consume a fenced @grammar(...) Name {SENT ... SENT} declaration at `@`. */
static size_t cc__skip_grammar_decl(const char* s, size_t n, size_t at) {
    size_t p, s0, s1, b0, sent_len;
    char sent[64];
    if (at >= n || s[at] != '@') return at;
    if (!cc_match_ident_kw(s, n, at + 1, "grammar")) return at;
    p = cc_skip_ws_and_comments(s, n, at + 1 + 7);
    if (p >= n || s[p] != '(') return at;
    {
        size_t r = 0;
        if (!cc_find_matching_paren(s, n, p, &r)) return at;
        p = r + 1;
    }
    p = cc_skip_ws_and_comments(s, n, p);
    if (p >= n || !cc_is_ident_start(s[p])) return at;
    while (p < n && cc_is_ident_char(s[p])) p++;
    p = cc_skip_ws_and_comments(s, n, p);
    if (p >= n || s[p] != '{') return at;
    s0 = p + 1;
    s1 = s0;
    while (s1 < n && s[s1] != ' ' && s[s1] != '\t' &&
           s[s1] != '\n' && s[s1] != '\r')
        s1++;
    if (s1 == s0 || s1 - s0 >= sizeof(sent) - 1) return at;
    memcpy(sent, s + s0, s1 - s0);
    sent_len = s1 - s0;
    sent[sent_len] = '}';
    sent[sent_len + 1] = '\0';
    b0 = s1 + 1;
    if (s1 < n && s[s1] == '\r' && b0 < n && s[b0] == '\n') b0++;
    for (size_t i = b0; i + sent_len + 1 <= n; i++) {
        if (memcmp(s + i, sent, sent_len + 1) == 0)
            return i + sent_len + 1;
    }
    return at;
}

/* Balanced scan from `i` to the next `;` at paren/brace/bracket depth 0. */
static size_t cc__scan_to_semicolon(const char* s, size_t n, size_t i) {
    int paren = 0, brace = 0, brack = 0;
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;
    for (; i < n; i++) {
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
        if (c == '(') { paren++; continue; }
        if (c == ')') { if (paren) paren--; continue; }
        if (c == '[') { brack++; continue; }
        if (c == ']') { if (brack) brack--; continue; }
        if (c == '{') { brace++; continue; }
        if (c == '}') { if (brace) brace--; continue; }
        if (c == ';' && paren == 0 && brace == 0 && brack == 0)
            return i + 1;
    }
    return n;
}

/* Skip one statement (for control-structure bodies and top-level stmts). */
static size_t cc__skip_statement(const char* s, size_t n, size_t i);

static size_t cc__skip_paren_group(const char* s, size_t n, size_t i) {
    size_t r = 0;
    i = cc_skip_ws_and_comments(s, n, i);
    if (i >= n || s[i] != '(') return i;
    if (!cc_find_matching_paren(s, n, i, &r)) return n;
    return r + 1;
}

static size_t cc__skip_statement(const char* s, size_t n, size_t i) {
    size_t r;
    i = cc_skip_ws_and_comments(s, n, i);
    if (i >= n) return i;

    if (cc__starts_with_kw(s, n, i, "if")) {
        i += 2;
        i = cc__skip_paren_group(s, n, i);
        i = cc__skip_statement(s, n, i);
        {
            size_t j = cc_skip_ws_and_comments(s, n, i);
            if (cc__starts_with_kw(s, n, j, "else")) {
                j += 4;
                i = cc__skip_statement(s, n, j);
            }
        }
        return i;
    }
    if (cc__starts_with_kw(s, n, i, "for") ||
        cc__starts_with_kw(s, n, i, "while") ||
        cc__starts_with_kw(s, n, i, "switch")) {
        while (i < n && cc_is_ident_char(s[i])) i++;
        i = cc__skip_paren_group(s, n, i);
        return cc__skip_statement(s, n, i);
    }
    if (cc__starts_with_kw(s, n, i, "do")) {
        i += 2;
        i = cc__skip_statement(s, n, i);
        i = cc_skip_ws_and_comments(s, n, i);
        if (cc__starts_with_kw(s, n, i, "while")) {
            i += 5;
            i = cc__skip_paren_group(s, n, i);
            i = cc_skip_ws_and_comments(s, n, i);
            if (i < n && s[i] == ';') i++;
        }
        return i;
    }
    if (s[i] == '{') {
        if (!cc_find_matching_brace(s, n, i, &r)) return n;
        return r + 1;
    }
    return cc__scan_to_semicolon(s, n, i);
}

/*
 * Classify and consume one top-level item starting at `item` (first
 * non-trivia token). Sets *out_end past the item and *out_kind.
 * Returns 1 on success, 0 if nothing consumable remains.
 */
static int cc__classify_item(const char* s, size_t n, size_t item,
                             size_t* out_end, CC__ChunkKind* out_kind) {
    size_t i, r, body_l, body_r;
    size_t after_storage;
    int is_type_kw = 0;
    int saw_eq = 0;
    int paren = 0, brace = 0, brack = 0;
    int in_str = 0, in_chr = 0, in_line = 0, in_block = 0;

    if (item >= n) return 0;
    i = item;

    /* Preprocessor directive (line-start after trivia). */
    if (s[i] == '#') {
        while (i < n) {
            if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (s[i] == '\\' && i + 2 < n && s[i + 1] == '\r' && s[i + 2] == '\n') {
                i += 3;
                continue;
            }
            if (s[i] == '\n') {
                i++;
                break;
            }
            i++;
        }
        *out_end = i;
        *out_kind = CC__CHUNK_TU;
        return 1;
    }

    /* @grammar(schema|rules) Name {~~~~ ... ~~~~} */
    if (s[i] == '@' && cc_match_ident_kw(s, n, i + 1, "grammar")) {
        size_t end = cc__skip_grammar_decl(s, n, i);
        if (end > i) {
            *out_end = end;
            *out_kind = CC__CHUNK_TU;
            return 1;
        }
    }

    /* @comptime { ... } */
    if (cc_match_comptime_block(s, n, i, &body_l, &body_r)) {
        *out_end = body_r + 1;
        *out_kind = CC__CHUNK_TU;
        return 1;
    }

    /* typedef ... ; */
    if (cc__starts_with_kw(s, n, i, "typedef")) {
        *out_end = cc__scan_to_semicolon(s, n, i);
        *out_kind = CC__CHUNK_TU;
        return 1;
    }

    /* Control-flow / jump statements → main. */
    if (cc__starts_with_kw(s, n, i, "if") ||
        cc__starts_with_kw(s, n, i, "for") ||
        cc__starts_with_kw(s, n, i, "while") ||
        cc__starts_with_kw(s, n, i, "switch") ||
        cc__starts_with_kw(s, n, i, "do") ||
        cc__starts_with_kw(s, n, i, "return") ||
        cc__starts_with_kw(s, n, i, "goto") ||
        cc__starts_with_kw(s, n, i, "break") ||
        cc__starts_with_kw(s, n, i, "continue")) {
        *out_end = cc__skip_statement(s, n, i);
        *out_kind = CC__CHUNK_MAIN;
        return 1;
    }

    after_storage = cc__skip_storage(s, n, i);
    if (cc__starts_with_kw(s, n, after_storage, "struct") ||
        cc__starts_with_kw(s, n, after_storage, "enum") ||
        cc__starts_with_kw(s, n, after_storage, "union"))
        is_type_kw = 1;

    /* General scan: function def/prototype, type def, or statement/decl. */
    for (; i < n; i++) {
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

        if (c == '(') {
            if (paren == 0 && brace == 0 && brack == 0) {
                size_t lpar = i, rpar = 0, j;
                if (!cc_find_matching_paren(s, n, lpar, &rpar)) {
                    *out_end = n;
                    *out_kind = CC__CHUNK_MAIN;
                    return 1;
                }
                j = cc__skip_throws_clause(s, n, rpar + 1);
                j = cc_skip_ws_and_comments(s, n, j);
                if (j < n && (s[j] == '{' || s[j] == ';') &&
                    cc__looks_like_func_declarator(s, n, item, lpar)) {
                    if (s[j] == '{') {
                        if (!cc_find_matching_brace(s, n, j, &r)) {
                            *out_end = n;
                            *out_kind = CC__CHUNK_TU;
                            return 1;
                        }
                        *out_end = r + 1;
                    } else {
                        *out_end = j + 1;
                    }
                    *out_kind = CC__CHUNK_TU;
                    return 1;
                }
                /* Not a function declarator — continue inside as normal. */
                paren = 1;
                i = lpar;
                continue;
            }
            paren++;
            continue;
        }
        if (c == ')') { if (paren) paren--; continue; }
        if (c == '[') { brack++; continue; }
        if (c == ']') { if (brack) brack--; continue; }

        if (paren == 0 && brace == 0 && brack == 0 && c == '=')
            saw_eq = 1;

        if (c == '{') {
            if (paren == 0 && brace == 0 && brack == 0) {
                if (!cc_find_matching_brace(s, n, i, &r)) {
                    *out_end = n;
                    *out_kind = is_type_kw && !saw_eq ? CC__CHUNK_TU : CC__CHUNK_MAIN;
                    return 1;
                }
                if (is_type_kw && !saw_eq) {
                    size_t j = cc_skip_ws_and_comments(s, n, r + 1);
                    if (j < n && s[j] == ';') {
                        *out_end = j + 1;
                        *out_kind = CC__CHUNK_TU;
                        return 1;
                    }
                    /* struct S { ... } var = ...; → keep scanning */
                    i = r;
                    brace = 0;
                    continue;
                }
                if (saw_eq) {
                    /* Brace initializer: static T x[] = { ... }; — keep
                     * scanning to ';' so the static/extern TU rule applies.
                     * Without this, `{...}` was misclassified as a compound
                     * statement and the decl never landed at file scope. */
                    i = r;
                    continue;
                }
                /* Compound statement at depth 0 → main */
                *out_end = r + 1;
                *out_kind = CC__CHUNK_MAIN;
                return 1;
            }
            brace++;
            continue;
        }
        if (c == '}') { if (brace) brace--; continue; }

        if (c == ';' && paren == 0 && brace == 0 && brack == 0) {
            *out_end = i + 1;
            /* Type defs and file-scope static/extern decls stay at TU so
             * helpers can share counters / globals; non-static runtime
             * decls still hoist into synthetic main. */
            if ((is_type_kw && !saw_eq) ||
                cc__starts_with_kw(s, n, item, "static") ||
                cc__starts_with_kw(s, n, item, "extern"))
                *out_kind = CC__CHUNK_TU;
            else
                *out_kind = CC__CHUNK_MAIN;
            return 1;
        }
    }

    *out_end = n;
    *out_kind = CC__CHUNK_MAIN;
    return 1;
}

static int cc__split_body(const char* body, size_t n,
                          CC__Chunk** out_chunks, size_t* out_count) {
    CC__Chunk* chunks = NULL;
    size_t count = 0, cap = 0;
    size_t i = 0;

    while (i < n) {
        size_t lead = i;
        size_t item = cc_skip_ws_and_comments(body, n, i);
        size_t end = 0;
        CC__ChunkKind kind = CC__CHUNK_MAIN;
        CC__Chunk* slot;

        if (item >= n) {
            /* Trailing trivia: attach to previous chunk or emit as TU no-op. */
            if (count > 0) {
                chunks[count - 1].b = n;
            } else if (lead < n) {
                /* Comment-only body — keep at TU (harmless). */
                if (count + 1 > cap) {
                    size_t ncap = cap ? cap * 2 : 16;
                    CC__Chunk* nc = (CC__Chunk*)realloc(chunks, ncap * sizeof(CC__Chunk));
                    if (!nc) { free(chunks); return -1; }
                    chunks = nc;
                    cap = ncap;
                }
                chunks[count].a = lead;
                chunks[count].b = n;
                chunks[count].kind = CC__CHUNK_TU;
                count++;
            }
            break;
        }

        if (!cc__classify_item(body, n, item, &end, &kind))
            break;
        if (end < item) end = item;

        if (count + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 16;
            CC__Chunk* nc = (CC__Chunk*)realloc(chunks, ncap * sizeof(CC__Chunk));
            if (!nc) { free(chunks); return -1; }
            chunks = nc;
            cap = ncap;
        }
        slot = &chunks[count++];
        slot->a = lead;
        slot->b = end;
        slot->kind = kind;
        i = end;
    }

    *out_chunks = chunks;
    *out_count = count;
    return 0;
}

/* Growable rewrite sink — never trusts a precomputed byte budget. */
typedef struct CC__OutBuf {
    char* data;
    size_t len;
    size_t cap;
} CC__OutBuf;

static void cc__ob_init(CC__OutBuf* ob) {
    ob->data = NULL;
    ob->len = 0;
    ob->cap = 0;
}

static void cc__ob_free(CC__OutBuf* ob) {
    free(ob->data);
    ob->data = NULL;
    ob->len = 0;
    ob->cap = 0;
}

static int cc__ob_ensure(CC__OutBuf* ob, size_t add) {
    size_t need;
    size_t ncap;
    char* nd;
    if (!ob) return -1;
    if (add > SIZE_MAX - ob->len) return -1;
    need = ob->len + add;
    if (need <= ob->cap) return 0;
    ncap = ob->cap ? ob->cap : 1024;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2u) {
            ncap = need;
            break;
        }
        ncap *= 2u;
    }
    if (ncap < need) ncap = need;
    nd = (char*)realloc(ob->data, ncap);
    if (!nd) return -1;
    ob->data = nd;
    ob->cap = ncap;
    return 0;
}

static int cc__append(CC__OutBuf* ob, const char* p, size_t n) {
    if (n == 0) return 0;
    if (!ob || !p) return -1;
    if (cc__ob_ensure(ob, n) != 0) return -1;
    memcpy(ob->data + ob->len, p, n);
    ob->len += n;
    return 0;
}

static int cc__append_ch(CC__OutBuf* ob, char c) {
    return cc__append(ob, &c, 1);
}

/* NUL-terminate in place; leaves ownership with ob until take/free. */
static int cc__ob_terminate(CC__OutBuf* ob) {
    if (cc__ob_ensure(ob, 1) != 0) return -1;
    ob->data[ob->len] = '\0';
    return 0;
}

/* Steal buffer (NUL-terminated). Sets *out_len to byte length excluding NUL. */
static char* cc__ob_take(CC__OutBuf* ob, size_t* out_len) {
    char* p;
    if (cc__ob_terminate(ob) != 0) return NULL;
    p = ob->data;
    if (out_len) *out_len = ob->len;
    ob->data = NULL;
    ob->len = 0;
    ob->cap = 0;
    return p;
}

/* 1-based line of src[off] in the original buffer (honors shebang lines). */
static int cc__src_line_at(const char* src, size_t off) {
    size_t i;
    int line = 1;
    if (!src) return 1;
    for (i = 0; i < off; i++) {
        if (src[i] == '\n') line++;
    }
    return line;
}

/*
 * Provenance stamp before a rewritten chunk.
 * raw_hash_line=1 → file-scope #line N "path" (TU chunks).
 * raw_hash_line=0 → masked CC_LN block comment (MAIN chunks inside synthetic
 * main; pass_create skips block comments for declared types / @destroy).
 */
static int cc__append_prov_marker(CC__OutBuf* ob, int raw_hash_line, int line,
                                  const char* path) {
    char buf[1200];
    int m;
    if (!path || !path[0] || line < 1) return 0;
    if (raw_hash_line) {
        m = snprintf(buf, sizeof(buf), "#line %d \"%s\"\n", line, path);
    } else {
        m = snprintf(buf, sizeof(buf), "/*CC_LN %d %s*/\n", line, path);
    }
    if (m < 0 || (size_t)m >= sizeof(buf)) return -1;
    return cc__append(ob, buf, (size_t)m);
}

/* Top-level arity of a parameter list span (0 if empty/whitespace-only). */
static int cc__param_list_arity(const char* s, size_t a, size_t b) {
    int depth = 0, commas = 0, any = 0;
    size_t i = a;
    while (i < b) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth) depth--;
        } else if (c == ',' && depth == 0) {
            commas++;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') any = 1;
        i++;
    }
    if (!any) return 0;
    return commas + 1;
}

/* Split [a,b) on the first top-level comma; returns 1 and fills ranges. */
static int cc__param_list_split2(const char* s, size_t a, size_t b,
                                 size_t* a0, size_t* b0, size_t* a1, size_t* b1) {
    int depth = 0;
    size_t i = a;
    if (!s || !a0 || !b0 || !a1 || !b1 || a >= b) return 0;
    while (i < b) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth) depth--;
        } else if (c == ',' && depth == 0) {
            *a0 = a;
            *b0 = i;
            *a1 = i + 1;
            *b1 = b;
            return 1;
        }
        i++;
    }
    return 0;
}

/* Pointer depth: each `*` and each trailing `[]` counts one. */
static int cc__param_ptr_depth(const char* s, size_t a, size_t b) {
    int depth = 0;
    int paren = 0, brack = 0, brace = 0;
    size_t i = a;
    while (i < b) {
        char c = s[i];
        if (c == '(') {
            paren++;
            i++;
            continue;
        }
        if (c == ')') {
            if (paren) paren--;
            i++;
            continue;
        }
        if (c == '{') {
            brace++;
            i++;
            continue;
        }
        if (c == '}') {
            if (brace) brace--;
            i++;
            continue;
        }
        if (c == '[') {
            if (paren == 0 && brace == 0) depth++;
            brack++;
            i++;
            continue;
        }
        if (c == ']') {
            if (brack) brack--;
            i++;
            continue;
        }
        if (c == '*' && paren == 0 && brack == 0 && brace == 0) depth++;
        i++;
    }
    return depth;
}

static int cc__param_has_kw(const char* s, size_t a, size_t b, const char* kw) {
    size_t i = a;
    while (i < b) {
        if (cc__starts_with_kw(s, b, i, kw)) return 1;
        if (cc_is_ident_start(s[i])) {
            while (i < b && cc_is_ident_char(s[i])) i++;
            continue;
        }
        i++;
    }
    return 0;
}

/* True for `(int …, char **…)` / `(int …, char *…[])` — per-parameter shape. */
static int cc__param_list_is_argc_argv(const char* s, size_t a, size_t b) {
    size_t a0, b0, a1, b1;
    if (cc__param_list_arity(s, a, b) != 2) return 0;
    if (!cc__param_list_split2(s, a, b, &a0, &b0, &a1, &b1)) return 0;
    if (!cc__param_has_kw(s, a0, b0, "int")) return 0;
    if (cc__param_ptr_depth(s, a0, b0) != 0) return 0;
    if (!cc__param_has_kw(s, a1, b1, "char")) return 0;
    if (cc__param_ptr_depth(s, a1, b1) < 2) return 0;
    return 1;
}

/* True for `()` or `(void)`. */
static int cc__param_list_is_void(const char* s, size_t a, size_t b) {
    size_t i = a;
    while (i < b && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    if (i >= b) return 1;
    if (!cc__starts_with_kw(s, b, i, "void")) return 0;
    i += 4;
    while (i < b && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    return i >= b;
}

typedef struct {
    char name[128];
    char summary[256];
    int takes_argv; /* 1 = (int,char**), 0 = void/empty */
    size_t chunk_a; /* body offset of enclosing TU chunk */
    size_t brace;   /* body offset of function-body '{' */
} CC__ScriptTask;

/*
 * Parse `[a,b)` as `int name(…)` definition (optional storage).
 * Returns 1 and fills name/takes_argv when shape is a valid task callable;
 * 0 otherwise. Prototypes and `main` are rejected.
 */
static int cc__extract_script_task_shape(const char* s, size_t a, size_t b,
                                         char* name_out, size_t name_cap,
                                         int* out_takes_argv,
                                         size_t* out_brace) {
    size_t i, ns, nl, rpar = 0, j;
    int takes_argv = 0;
    if (!s || a >= b || !name_out || name_cap < 2) return 0;
    i = cc_skip_ws_and_comments(s, b, a);
    i = cc__skip_storage(s, b, i);
    i = cc_skip_ws_and_comments(s, b, i);
    if (!cc__starts_with_kw(s, b, i, "int")) return 0;
    i += 3;
    i = cc_skip_ws_and_comments(s, b, i);
    if (i < b && s[i] == '*') return 0;
    if (i >= b || !cc_is_ident_start(s[i])) return 0;
    ns = i;
    while (i < b && cc_is_ident_char(s[i])) i++;
    nl = i - ns;
    if (nl == 0 || nl + 1 > name_cap) return 0;
    if (nl == 4 && memcmp(s + ns, "main", 4) == 0) return 0;
    i = cc_skip_ws_and_comments(s, b, i);
    if (i >= b || s[i] != '(') return 0;
    if (!cc_find_matching_paren(s, b, i, &rpar)) return 0;
    if (cc__param_list_is_argc_argv(s, i + 1, rpar)) {
        takes_argv = 1;
    } else if (cc__param_list_is_void(s, i + 1, rpar)) {
        takes_argv = 0;
    } else {
        return 0;
    }
    j = cc__skip_throws_clause(s, b, rpar + 1);
    j = cc_skip_ws_and_comments(s, b, j);
    if (j >= b || s[j] != '{') return 0;
    memcpy(name_out, s + ns, nl);
    name_out[nl] = '\0';
    if (out_takes_argv) *out_takes_argv = takes_argv;
    if (out_brace) *out_brace = j;
    return 1;
}

static int cc__task_name_cmp(const void* a, const void* b) {
    return strcmp(((const CC__ScriptTask*)a)->name,
                  ((const CC__ScriptTask*)b)->name);
}

/* Last CCDoc block (slash-star-star ... star-slash) in [a, decl) with only
 * whitespace between the closer and decl. */
static int cc__find_ccdoc_before(const char* s, size_t a, size_t decl,
                                 size_t* out_a, size_t* out_b) {
    size_t i = a;
    size_t last_a = 0, last_b = 0;
    int found = 0;
    if (!s || a >= decl) return 0;
    while (i + 3 < decl) {
        size_t j, end, k;
        if (!(s[i] == '/' && s[i + 1] == '*' && s[i + 2] == '*')) {
            i++;
            continue;
        }
        /* Skip empty slash-star-star-slash markers. */
        if (i + 3 < decl && s[i + 3] == '/') {
            i += 4;
            continue;
        }
        j = i + 3;
        while (j + 1 < decl && !(s[j] == '*' && s[j + 1] == '/')) j++;
        if (j + 1 >= decl) break;
        end = j + 2;
        k = end;
        while (k < decl &&
               (s[k] == ' ' || s[k] == '\t' || s[k] == '\n' || s[k] == '\r'))
            k++;
        if (k == decl) {
            last_a = i;
            last_b = end;
            found = 1;
        }
        i = end;
    }
    if (!found) return 0;
    *out_a = last_a;
    *out_b = last_b;
    return 1;
}

static void cc__trim_inplace(char* s) {
    size_t n, i, j;
    if (!s) return;
    n = strlen(s);
    i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    j = n;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' ||
                     s[j - 1] == '\n' || s[j - 1] == '\r'))
        j--;
    if (i > 0 || j < n) {
        size_t len = j - i;
        memmove(s, s + i, len);
        s[len] = '\0';
    }
}

/* 1 if CCDoc block [a,b) has a line whose first token is the `@task` tag. */
static int cc__ccdoc_has_task_tag(const char* s, size_t a, size_t b) {
    size_t i, o = 0;
    char buf[1024];
    if (!s || a + 5 > b) return 0;
    /* Same strip as one_line_summary: drop opener/closer then scan lines. */
    i = a + 3;
    if (b >= 2) b -= 2;
    while (i < b && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < b && s[i] == '*' && !(i + 1 < b && s[i + 1] == '/')) {
        i++;
        if (i < b && s[i] == ' ') i++;
    }
    while (i < b && o + 1 < sizeof(buf)) {
        char c = s[i];
        if (c == '\r') { i++; continue; }
        if (c == '\n') {
            if (o + 1 < sizeof(buf)) buf[o++] = '\n';
            i++;
            while (i < b && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i < b && s[i] == '*') {
                i++;
                if (i < b && s[i] == '/') break;
                if (i < b && s[i] == ' ') i++;
            }
            continue;
        }
        buf[o++] = c;
        i++;
    }
    buf[o] = '\0';
    i = 0;
    while (i < o) {
        size_t line_a = i, line_b;
        char* p;
        while (i < o && buf[i] != '\n') i++;
        line_b = i;
        if (i < o && buf[i] == '\n') i++;
        p = buf + line_a;
        while (p < buf + line_b && (*p == ' ' || *p == '\t')) p++;
        if (p + 5 <= buf + line_b && memcmp(p, "@task", 5) == 0) {
            char next = (p + 5 < buf + line_b) ? p[5] : '\0';
            if (next == '\0' || next == ' ' || next == '\t') return 1;
        }
    }
    return 0;
}

/* Fill summary_out from a CCDoc block [a,b). Prefers @task text; else the
 * first free-text line before any @tag. */
static void cc__ccdoc_one_line_summary(const char* s, size_t a, size_t b,
                                       char* summary_out, size_t summary_cap) {
    char buf[1024];
    size_t o = 0;
    size_t i, line_a;
    char task_sum[256];
    char free_sum[256];
    int saw_tag = 0;

    if (!summary_out || summary_cap == 0) return;
    summary_out[0] = '\0';
    task_sum[0] = '\0';
    free_sum[0] = '\0';
    if (!s || a + 5 > b) return;

    /* Strip opening slash-star-star and closing star-slash. */
    i = a + 3;
    if (b >= 2) b -= 2;
    /* Optional '*' gutter on the opening line. */
    while (i < b && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < b && s[i] == '*' && !(i + 1 < b && s[i + 1] == '/')) {
        i++;
        if (i < b && s[i] == ' ') i++;
    }
    while (i < b) {
        char c = s[i];
        if (c == '\r') { i++; continue; }
        if (c == '\n') {
            if (o + 1 < sizeof(buf)) buf[o++] = '\n';
            i++;
            while (i < b && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i < b && s[i] == '*') {
                i++;
                if (i < b && s[i] == '/') break;
                if (i < b && s[i] == ' ') i++;
            }
            continue;
        }
        if (o + 1 < sizeof(buf)) buf[o++] = c;
        i++;
    }
    buf[o] = '\0';

    line_a = 0;
    while (line_a <= o) {
        size_t line_b = line_a;
        char line[256];
        size_t llen;
        char* p;
        while (line_b < o && buf[line_b] != '\n') line_b++;
        llen = line_b - line_a;
        if (llen >= sizeof(line)) llen = sizeof(line) - 1;
        memcpy(line, buf + line_a, llen);
        line[llen] = '\0';
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') {
            if (!saw_tag && free_sum[0]) break;
        } else if (*p == '@') {
            saw_tag = 1;
            if (strncmp(p, "@task", 5) == 0 &&
                (p[5] == '\0' || p[5] == ' ' || p[5] == '\t')) {
                p += 5;
                while (*p == ' ' || *p == '\t') p++;
                if (*p && !task_sum[0]) {
                    strncpy(task_sum, p, sizeof(task_sum) - 1);
                    task_sum[sizeof(task_sum) - 1] = '\0';
                    cc__trim_inplace(task_sum);
                }
            }
        } else if (!saw_tag && !free_sum[0]) {
            strncpy(free_sum, p, sizeof(free_sum) - 1);
            free_sum[sizeof(free_sum) - 1] = '\0';
            cc__trim_inplace(free_sum);
        }
        if (line_b >= o) break;
        line_a = line_b + 1;
    }

    if (task_sum[0])
        strncpy(summary_out, task_sum, summary_cap - 1);
    else
        strncpy(summary_out, free_sum, summary_cap - 1);
    summary_out[summary_cap - 1] = '\0';
}

/* Escape src into a C string literal body (no surrounding quotes). */
static int cc__escape_c_string(const char* src, char* dst, size_t dst_cap) {
    size_t o = 0;
    size_t i;
    if (!dst || dst_cap == 0) return -1;
    if (!src) src = "";
    for (i = 0; src[i]; i++) {
        char c = src[i];
        if (c == '\\' || c == '"') {
            if (o + 2 >= dst_cap) return -1;
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (o + 2 >= dst_cap) return -1;
            dst[o++] = '\\';
            dst[o++] = (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            if (o + 1 >= dst_cap) return -1;
            dst[o++] = c;
        }
    }
    dst[o] = '\0';
    return 0;
}

/*
 * Discover opt-in tasks: CCDoc `@task` on an immediately preceding block, plus
 * a valid shape `int name(void)` / `int name()` / `int name(int, char**)`.
 * `@task` on an incompatible declaration is an error (returns -1).
 */
static int cc__collect_script_tasks(const char* path,
                                    const char* body, const CC__Chunk* chunks,
                                    size_t nchunks,
                                    CC__ScriptTask** out_tasks,
                                    size_t* out_n) {
    CC__ScriptTask* tasks = NULL;
    size_t n = 0, cap = 0;
    size_t ci;
    for (ci = 0; ci < nchunks; ci++) {
        char name[128];
        size_t ti, decl, da, db, brace = 0;
        int takes_argv = 0;
        if (chunks[ci].kind != CC__CHUNK_TU) continue;
        decl = cc_skip_ws_and_comments(body, chunks[ci].b, chunks[ci].a);
        if (!cc__find_ccdoc_before(body, chunks[ci].a, decl, &da, &db))
            continue;
        if (!cc__ccdoc_has_task_tag(body, da, db))
            continue;
        if (!cc__extract_script_task_shape(body, chunks[ci].a, chunks[ci].b,
                                           name, sizeof(name), &takes_argv,
                                           &brace)) {
            fprintf(stderr,
                    "%s: error: CCDoc @task requires "
                    "`int name(void)` or `int name(int, char **)` "
                    "with a function body\n",
                    path ? path : "<shcc>");
            free(tasks);
            return -1;
        }
        for (ti = 0; ti < n; ti++) {
            if (strcmp(tasks[ti].name, name) == 0) break;
        }
        if (ti < n) continue;
        if (n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8;
            CC__ScriptTask* nt =
                (CC__ScriptTask*)realloc(tasks, ncap * sizeof(CC__ScriptTask));
            if (!nt) { free(tasks); return -1; }
            tasks = nt;
            cap = ncap;
        }
        memset(&tasks[n], 0, sizeof(tasks[n]));
        memcpy(tasks[n].name, name, sizeof(tasks[n].name));
        tasks[n].takes_argv = takes_argv;
        tasks[n].chunk_a = chunks[ci].a;
        tasks[n].brace = brace;
        cc__ccdoc_one_line_summary(body, da, db,
                                   tasks[n].summary, sizeof(tasks[n].summary));
        n++;
    }
    if (n > 1)
        qsort(tasks, n, sizeof(CC__ScriptTask), cc__task_name_cmp);
    *out_tasks = tasks;
    *out_n = n;
    return 0;
}

static size_t cc__task_name_width(const CC__ScriptTask* tasks, size_t ntasks) {
    size_t w = 0, ti;
    for (ti = 0; ti < ntasks; ti++) {
        size_t n = strlen(tasks[ti].name);
        if (n > w) w = n;
    }
    if (w < 8) w = 8;
    if (w > 40) w = 40;
    return w;
}

/*
 * Append one listing line.
 * to_stderr=0 → printf(...); to_stderr=1 → fprintf(stderr, ...).
 */
static int cc__append_task_list_line(CC__OutBuf* ob, int to_stderr, size_t width,
                                     const CC__ScriptTask* task) {
    char esc[512];
    char line[768];
    int m;
    const char* call = to_stderr ? "fprintf(stderr, " : "printf(";
    if (task->summary[0]) {
        if (cc__escape_c_string(task->summary, esc, sizeof(esc)) != 0)
            return -1;
        m = snprintf(line, sizeof(line),
                     "            %s\"%%-%zus  %%s\\n\", \"%s\", \"%s\");\n",
                     call, width, task->name, esc);
    } else {
        m = snprintf(line, sizeof(line),
                     "            %s\"%s\\n\");\n", call, task->name);
    }
    if (m < 0 || (size_t)m >= sizeof(line)) return -1;
    return cc__append(ob, line, (size_t)m);
}

/* Append @task dispatch; returns 0 on success, -1 on OOM / encode failure. */
static int cc__append_task_dispatch(CC__OutBuf* ob, const CC__ScriptTask* tasks,
                                    size_t ntasks) {
    size_t ti;
    size_t width = cc__task_name_width(tasks, ntasks);
    static const char head[] =
        "    if (argc >= 2 && argv[1] && argv[1][0] == '@') {\n"
        "        const char *__cc_task = argv[1] + 1;\n"
        "        if (__cc_task[0] == '\\0') {\n";
    static const char after_list[] =
        "            return 0;\n"
        "        }\n"
        "        {\n"
        "            int __i;\n"
        "            for (__i = 1; __i < argc - 1; __i++) argv[__i] = argv[__i + 1];\n"
        "            argc -= 1;\n"
        "            argv[argc] = NULL;\n"
        "        }\n";
    static const char unknown_head[] =
        "        fprintf(stderr, \"shcc: unknown task @%s\\n\", __cc_task);\n"
        "        fprintf(stderr, \"tasks:\\n\");\n";
    static const char unknown_tail[] =
        "        return 2;\n"
        "    }\n"
        "\n";

    if (cc__append(ob, head, sizeof(head) - 1) != 0) return -1;
    for (ti = 0; ti < ntasks; ti++) {
        if (cc__append_task_list_line(ob, 0, width, &tasks[ti]) != 0)
            return -1;
    }
    if (cc__append(ob, after_list, sizeof(after_list) - 1) != 0) return -1;
    for (ti = 0; ti < ntasks; ti++) {
        char line[256];
        int m;
        if (tasks[ti].takes_argv) {
            m = snprintf(line, sizeof(line),
                         "        if (strcmp(__cc_task, \"%s\") == 0) "
                         "return %s(argc, argv);\n",
                         tasks[ti].name, tasks[ti].name);
        } else {
            m = snprintf(line, sizeof(line),
                         "        if (strcmp(__cc_task, \"%s\") == 0) "
                         "return %s();\n",
                         tasks[ti].name, tasks[ti].name);
        }
        if (m < 0 || (size_t)m >= sizeof(line)) return -1;
        if (cc__append(ob, line, (size_t)m) != 0) return -1;
    }
    if (cc__append(ob, unknown_head, sizeof(unknown_head) - 1) != 0) return -1;
    for (ti = 0; ti < ntasks; ti++) {
        if (cc__append_task_list_line(ob, 1, width, &tasks[ti]) != 0)
            return -1;
    }
    return cc__append(ob, unknown_tail, sizeof(unknown_tail) - 1);
}

char* cc_script_rewrite_source(const char* path,
                               const char* src,
                               size_t len,
                               size_t* out_len) {
    if (!cc_path_is_shcc(path) || !src) return NULL;

    size_t body_off = cc__script_skip_shebang(src, len);
    const char* body = src + body_off;
    size_t body_len = len - body_off;
    int has_main = cc__script_has_toplevel_main(body, body_len);

    /* Include only at TU scope. Default @errhandler lives inside main so
     * statement-start rewinds for `!>` / `@err` stay within the function
     * (file-scope handler + synthetic main confused pass_err_syntax). */
    static const char prelude[] =
        "/* .shcc entry: auto prelude */\n"
        "#include <ccc/script/prelude.cch>\n"
        "\n";
    static const char default_eh[] =
        "    @errhandler(CCError e) {\n"
        "        (void)cc_eprintln(e.message);\n"
        "        return 1;\n"
        "    }\n"
        "\n";
    static const char main_open[] =
        "int main(int argc, char **argv) {\n";
    static const char main_close[] =
        "\n    return 0;\n"
        "}\n";
    static const char main_stmt_err[] =
        "#error \".shcc: explicit main cannot coexist with top-level statements\"\n";

    size_t pre_len = sizeof(prelude) - 1;
    size_t eh_len = sizeof(default_eh) - 1;
    size_t open_len = sizeof(main_open) - 1;
    size_t close_len = sizeof(main_close) - 1;

    CC__Chunk* chunks = NULL;
    size_t nchunks = 0;
    size_t ci;
    int has_main_chunk = 0;
    CC__ScriptTask* tasks = NULL;
    size_t ntasks = 0;
    CC__OutBuf ob;
    char* out;

    if (cc__split_body(body, body_len, &chunks, &nchunks) != 0)
        return NULL;

    for (ci = 0; ci < nchunks; ci++) {
        if (chunks[ci].kind != CC__CHUNK_TU)
            has_main_chunk = 1;
    }

    cc__ob_init(&ob);

    if (has_main) {
        if (has_main_chunk) {
            fprintf(stderr,
                    "%s: .shcc with explicit main cannot also have "
                    "top-level statements\n",
                    path ? path : "<shcc>");
            if (cc__append(&ob, prelude, pre_len) != 0 ||
                cc__append(&ob, main_stmt_err, sizeof(main_stmt_err) - 1) != 0 ||
                cc__append(&ob, body, body_len) != 0) {
                cc__ob_free(&ob);
                free(chunks);
                return NULL;
            }
            out = cc__ob_take(&ob, out_len);
            free(chunks);
            return out;
        }
        /* Explicit main, TU-only extras: prelude + body with file-scope #line. */
        {
            int body_line = cc__src_line_at(src, body_off);
            if (cc__append(&ob, prelude, pre_len) != 0) {
                cc__ob_free(&ob);
                free(chunks);
                return NULL;
            }
            if (path && body_len > 0 &&
                cc__append_prov_marker(&ob, 1, body_line, path) != 0) {
                cc__ob_free(&ob);
                free(chunks);
                return NULL;
            }
            if (cc__append(&ob, body, body_len) != 0) {
                cc__ob_free(&ob);
                free(chunks);
                return NULL;
            }
            out = cc__ob_take(&ob, out_len);
            free(chunks);
            return out;
        }
    }

    if (cc__collect_script_tasks(path, body, chunks, nchunks, &tasks, &ntasks) != 0) {
        cc__ob_free(&ob);
        free(chunks);
        return NULL;
    }

    /* No explicit main: TU chunks (raw #line), then synthetic main with
     * masked CC_LN before each MAIN chunk (raw mid-function #line breaks
     * @create/@destroy type extraction in pass_create). */
    if (cc__append(&ob, prelude, pre_len) != 0) goto fail_rewrite;

    for (ci = 0; ci < nchunks; ci++) {
        int line;
        const CC__ScriptTask* task = NULL;
        size_t ti;
        if (chunks[ci].kind != CC__CHUNK_TU) continue;
        if (chunks[ci].b <= chunks[ci].a) continue;
        line = cc__src_line_at(src, body_off + chunks[ci].a);
        if (path && cc__append_prov_marker(&ob, 1, line, path) != 0)
            goto fail_rewrite;
        for (ti = 0; ti < ntasks; ti++) {
            if (tasks[ti].chunk_a == chunks[ci].a) {
                task = &tasks[ti];
                break;
            }
        }
        if (task && task->brace >= chunks[ci].a &&
            task->brace < chunks[ci].b) {
            size_t brace = task->brace;
            size_t body_a = brace + 1;
            size_t body_n = chunks[ci].b > body_a ? chunks[ci].b - body_a : 0;
            /* Same default @errhandler as synthetic main — tasks are
             * separate functions and do not see main's handler.
             * Leading newline: the prefix ends at `{` (not the following
             * `\n`), so without it `@errhandler` sticks to `{` on one line
             * and later statements fail to parse. */
            static const char task_eh[] =
                "\n    @errhandler(CCError e) {\n"
                "        (void)cc_eprintln(e.message);\n"
                "        return 1;\n"
                "    }\n"
                "\n";
            if (cc__append(&ob, body + chunks[ci].a,
                           brace + 1 - chunks[ci].a) != 0)
                goto fail_rewrite;
            if (cc__append(&ob, task_eh, sizeof(task_eh) - 1) != 0)
                goto fail_rewrite;
            if (body_n &&
                cc__append(&ob, body + body_a, body_n) != 0)
                goto fail_rewrite;
        } else if (cc__append(&ob, body + chunks[ci].a,
                              chunks[ci].b - chunks[ci].a) != 0) {
            goto fail_rewrite;
        }
        if (ob.len > 0 && ob.data[ob.len - 1] != '\n' &&
            cc__append_ch(&ob, '\n') != 0)
            goto fail_rewrite;
    }

    if (cc__append(&ob, main_open, open_len) != 0 ||
        cc__append(&ob, default_eh, eh_len) != 0 ||
        cc__append_task_dispatch(&ob, tasks, ntasks) != 0)
        goto fail_rewrite;

    /* Token-gated a/io/in/args for the synthetic-main wrap only — not
     * @task bodies (those declare explicitly when needed). */
    {
        size_t main_len = 0;
        char* main_buf = NULL;
        char* pre = NULL;
        size_t pre_n = 0;
        for (ci = 0; ci < nchunks; ci++) {
            if (chunks[ci].kind != CC__CHUNK_MAIN) continue;
            if (chunks[ci].b > chunks[ci].a)
                main_len += chunks[ci].b - chunks[ci].a + 1;
        }
        if (main_len > 0) {
            size_t o = 0;
            main_buf = (char*)malloc(main_len + 1);
            if (!main_buf) goto fail_rewrite;
            for (ci = 0; ci < nchunks; ci++) {
                size_t n;
                if (chunks[ci].kind != CC__CHUNK_MAIN) continue;
                if (chunks[ci].b <= chunks[ci].a) continue;
                n = chunks[ci].b - chunks[ci].a;
                memcpy(main_buf + o, body + chunks[ci].a, n);
                o += n;
                main_buf[o++] = '\n';
            }
            main_buf[o] = '\0';
            pre = cc_script_oneliner_predecls_for(main_buf, o, 1, "    ",
                                                  &pre_n);
            free(main_buf);
            if (!pre) goto fail_rewrite;
            if (pre_n && cc__append(&ob, pre, pre_n) != 0) {
                free(pre);
                goto fail_rewrite;
            }
            free(pre);
        }
    }

    for (ci = 0; ci < nchunks; ci++) {
        int line;
        if (chunks[ci].kind != CC__CHUNK_MAIN) continue;
        if (chunks[ci].b <= chunks[ci].a) continue;
        line = cc__src_line_at(src, body_off + chunks[ci].a);
        if (path && cc__append_prov_marker(&ob, 0, line, path) != 0)
            goto fail_rewrite;
        if (cc__append(&ob, body + chunks[ci].a,
                       chunks[ci].b - chunks[ci].a) != 0)
            goto fail_rewrite;
        if (ob.len > 0 && ob.data[ob.len - 1] != '\n' &&
            cc__append_ch(&ob, '\n') != 0)
            goto fail_rewrite;
    }

    if (cc__append(&ob, main_close, close_len) != 0) goto fail_rewrite;
    out = cc__ob_take(&ob, out_len);
    free(chunks);
    free(tasks);
    return out;

fail_rewrite:
    cc__ob_free(&ob);
    free(chunks);
    free(tasks);
    return NULL;
}
