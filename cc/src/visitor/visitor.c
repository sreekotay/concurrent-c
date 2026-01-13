#include "visitor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "visitor/ufcs.h"

/* Text-based async lowering (implemented in `cc/src/visitor/async_text.c`). */
int cc_async_rewrite_state_machine_text(const char* in_src, size_t in_len, char** out_src, size_t* out_len);

/* --- Closure scan/lowering helpers (best-effort, early) ---
   Goal: allow `spawn(() => { ... })` to lower to valid C by generating a
   top-level env+thunk and rewriting the spawn statement to use CCClosure0. */

typedef struct {
    int start_line;
    int end_line;
    int nursery_id;
    int id;
    int start_col; /* 0-based, in start_line */
    int end_col;   /* 0-based, in end_line (exclusive) */
    int param_count;   /* 0..2 (early) */
    char* param0_name; /* owned; NULL if param_count<1 */
    char* param1_name; /* owned; NULL if param_count<2 */
    char* param0_type; /* owned; NULL if inferred */
    char* param1_type; /* owned; NULL if inferred */
    char** cap_names;
    char** cap_types; /* parallel to cap_names; NULL if unknown */
    unsigned char* cap_flags; /* parallel; bit0=is_slice, bit1=move-only */
    int cap_count;
    char* body; /* includes surrounding { ... } */
} CCClosureDesc;

/* Forward decl (used by early checkers). */
static int cc__scan_spawn_closures(const char* src,
                                  size_t src_len,
                                  const char* src_path,
                                  int line_base,
                                  int* io_next_closure_id,
                                  CCClosureDesc** out_descs,
                                  int* out_count,
                                  int** out_line_map,
                                  int* out_line_cap,
                                  char** out_protos,
                                  size_t* out_protos_len,
                                  char** out_defs,
                                  size_t* out_defs_len);

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

static void cc__maybe_record_decl(char*** scope_names,
                                  char*** scope_types,
                                  unsigned char** scope_flags,
                                  int* scope_counts,
                                  int depth,
                                  const char* line) {
    if (!scope_names || !scope_types || !scope_flags || !scope_counts || depth < 0 || depth >= 256 || !line) return;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0') return;
    const char* semi = strchr(p, ';');
    if (!semi) return;
    /* Ignore function prototypes (best-effort):
       if we see '(' before ';' and there is no '=' before that '(', it's likely a prototype/declarator. */
    const char* lp = strchr(p, '(');
    if (lp && lp < semi) {
        const char* eq = strchr(p, '=');
        if (!eq || eq > lp) return;
    }

    /* Find the declared variable name as the last identifier before '=', ',', or ';'. */
    const char* name_s = NULL;
    size_t name_n = 0;
    const char* cur = p;
    while (cur < semi) {
        if (*cur == '"' || *cur == '\'') {
            char q = *cur++;
            while (cur < semi) {
                if (*cur == '\\' && (cur + 1) < semi) { cur += 2; continue; }
                if (*cur == q) { cur++; break; }
                cur++;
            }
            continue;
        }
        if (*cur == '=' || *cur == ',' || *cur == ';') break;
        if (!cc__is_ident_start_char(*cur)) { cur++; continue; }
        const char* s = cur++;
        while (cur < semi && cc__is_ident_char2(*cur)) cur++;
        size_t n = (size_t)(cur - s);
        if (n == 0 || cc__is_keyword_tok(s, n)) continue;
        name_s = s;
        name_n = n;
    }
    if (!name_s || name_n == 0) return;
    /* Type is everything from p to name_s (trimmed). */
    const char* ty_s = p;
    const char* ty_e = name_s;
    while (ty_s < ty_e && (*ty_s == ' ' || *ty_s == '\t')) ty_s++;
    while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;
    if (ty_e <= ty_s) return;

    int cur_n = scope_counts[depth];
    if (cc__name_in_list(scope_names[depth], cur_n, name_s, name_n)) return;

    /* Build a file-scope-safe type string.
       If the type uses CC slice syntax (`T[:]`/`T[:!]`), map it to CCSlice (plus pointer stars if present). */
    int is_slice = 0;
    int slice_has_bang = 0;
    int ptr_n = 0;
    for (const char* s = ty_s; s < ty_e; s++) {
        if (*s == '*') ptr_n++;
        if (*s == '[') {
            const char* t = s;
            /* allow spaces: [ : ] or [ : ! ] */
            while (t < ty_e && *t != ']') t++;
            if (t < ty_e) {
                /* very small heuristic: contains ':' inside brackets => slice-ish */
                for (const char* u = s; u < t; u++) {
                    if (*u == ':') is_slice = 1;
                    if (*u == '!') slice_has_bang = 1;
                }
            }
        }
    }

    char* ty = NULL;
    if (is_slice) {
        const char* base = "CCSlice";
        size_t bt = strlen(base);
        ty = (char*)malloc(bt + (size_t)ptr_n + 1);
        if (!ty) return;
        memcpy(ty, base, bt);
        for (int i = 0; i < ptr_n; i++) ty[bt + (size_t)i] = '*';
        ty[bt + (size_t)ptr_n] = '\0';
    } else {
        size_t tn = (size_t)(ty_e - ty_s);
        ty = (char*)malloc(tn + 1);
        if (!ty) return;
        memcpy(ty, ty_s, tn);
        ty[tn] = '\0';
    }

    char* name = (char*)malloc(name_n + 1);
    if (!name) { free(ty); return; }
    memcpy(name, name_s, name_n);
    name[name_n] = '\0';
    char** next = (char**)realloc(scope_names[depth], (size_t)(cur_n + 1) * sizeof(char*));
    if (!next) { free(name); free(ty); return; }
    scope_names[depth] = next;
    char** tnext = (char**)realloc(scope_types[depth], (size_t)(cur_n + 1) * sizeof(char*));
    if (!tnext) { free(name); free(ty); return; }
    scope_types[depth] = tnext;
    unsigned char* fnext = (unsigned char*)realloc(scope_flags[depth], (size_t)(cur_n + 1) * sizeof(unsigned char));
    if (!fnext) { free(name); free(ty); return; }
    scope_flags[depth] = fnext;

    /* Flags: bit0 = is_slice(CCSlice), bit1 = move-only slice hint. */
    unsigned char flags = 0;
    if (strcmp(ty, "CCSlice") == 0) flags |= 1;
    if (is_slice && slice_has_bang) flags |= 2;
    /* Provenance hint (more "real"): detect unique-id construction in initializer.
       - cc_slice_make_id(..., true/1, ...)
       - CC_SLICE_ID_UNIQUE bit present in an id expression
       This is still best-effort text parsing until we have a typed AST. */
    if ((flags & 1) != 0) {
        const char* eq = strchr(name_s, '=');
        if (eq && eq < semi) {
            if (strstr(eq, "CC_SLICE_ID_UNIQUE")) flags |= 2;
            const char* mk = strstr(eq, "cc_slice_make_id");
            if (mk) {
                const char* lp = strchr(mk, '(');
                if (lp) {
                    /* Parse 2nd argument (unique) in cc_slice_make_id(a, unique, ...). */
                    const char* t = lp + 1;
                    int comma = 0;
                    while (*t && t < semi) {
                        if (*t == '"' || *t == '\'') {
                            char qq = *t++;
                            while (*t && t < semi) {
                                if (*t == '\\' && t[1]) { t += 2; continue; }
                                if (*t == qq) { t++; break; }
                                t++;
                            }
                            continue;
                        }
                        if (*t == ',') {
                            comma++;
                            if (comma == 1) {
                                /* now at start of arg2 */
                                t++;
                                while (*t == ' ' || *t == '\t') t++;
                                if (strncmp(t, "true", 4) == 0) { flags |= 2; break; }
                                if (*t == '1') { flags |= 2; break; }
                            }
                        }
                        t++;
                    }
                }
            }
        }
    }

    scope_names[depth][cur_n] = name;
    scope_types[depth][cur_n] = ty;
    scope_flags[depth][cur_n] = flags;
    scope_counts[depth] = cur_n + 1;
}

static const char* cc__lookup_decl_type(char** scope_names,
                                        char** scope_types,
                                        int n,
                                        const char* name) {
    if (!scope_names || !scope_types || !name) return NULL;
    for (int i = 0; i < n; i++) {
        if (!scope_names[i] || !scope_types[i]) continue;
        if (strcmp(scope_names[i], name) == 0) return scope_types[i];
    }
    return NULL;
}

static unsigned char cc__lookup_decl_flags(char** scope_names,
                                           unsigned char* scope_flags,
                                           int n,
                                           const char* name) {
    if (!scope_names || !scope_flags || !name) return 0;
    for (int i = 0; i < n; i++) {
        if (!scope_names[i]) continue;
        if (strcmp(scope_names[i], name) == 0) return scope_flags[i];
    }
    return 0;
}

typedef struct {
    char* name;
    int depth;
} CCMovedName;

static int cc__moved_contains(CCMovedName* moved, int moved_n, const char* s, size_t n) {
    for (int i = 0; i < moved_n; i++) {
        if (!moved[i].name) continue;
        if (strlen(moved[i].name) == n && strncmp(moved[i].name, s, n) == 0) return 1;
    }
    return 0;
}

static void cc__moved_push(CCMovedName** io_moved, int* io_n, int* io_cap, const char* s, size_t n, int depth) {
    if (!io_moved || !io_n || !io_cap || !s || n == 0) return;
    if (cc__moved_contains(*io_moved, *io_n, s, n)) return;
    if (*io_n == *io_cap) {
        int nc = (*io_cap) ? (*io_cap) * 2 : 16;
        CCMovedName* nm = (CCMovedName*)realloc(*io_moved, (size_t)nc * sizeof(CCMovedName));
        if (!nm) return;
        *io_moved = nm;
        *io_cap = nc;
    }
    char* name = (char*)malloc(n + 1);
    if (!name) return;
    memcpy(name, s, n);
    name[n] = '\0';
    (*io_moved)[*io_n] = (CCMovedName){ .name = name, .depth = depth };
    (*io_n)++;
}

static void cc__moved_pop_depth(CCMovedName* moved, int* io_n, int depth) {
    if (!moved || !io_n) return;
    int n = *io_n;
    for (int i = 0; i < n; ) {
        if (moved[i].name && moved[i].depth > depth) {
            free(moved[i].name);
            moved[i] = moved[n - 1];
            n--;
            continue;
        }
        i++;
    }
    *io_n = n;
}

/* Best-effort checker: reject use-after-move for CCSlice locals moved via cc_move(x).
   This is an early slice-safety step until we have a real typed AST. */
static int cc__check_slice_use_after_move(const char* src, size_t src_len, const char* src_path) {
    if (!src || src_len == 0) return 0;
    /* Pre-scan closures so we can treat move-only slice captures as implicit moves. */
    CCClosureDesc* closure_descs = NULL;
    int closure_count = 0;
    int* closure_line_map = NULL;
    int closure_line_cap = 0;
    char* dummy_protos = NULL;
    size_t dummy_protos_len = 0;
    char* dummy_defs = NULL;
    size_t dummy_defs_len = 0;
    int closure_next_id = 1;
    (void)cc__scan_spawn_closures(src, src_len, src_path,
                                 1, &closure_next_id,
                                 &closure_descs, &closure_count,
                                 &closure_line_map, &closure_line_cap,
                                 &dummy_protos, &dummy_protos_len,
                                 &dummy_defs, &dummy_defs_len);
    free(closure_line_map);
    free(dummy_protos);
    free(dummy_defs);

    char** scope_names[256];
    char** scope_types[256];
    unsigned char* scope_flags[256];
    int scope_counts[256];
    for (int i = 0; i < 256; i++) { scope_names[i] = NULL; scope_types[i] = NULL; scope_flags[i] = NULL; scope_counts[i] = 0; }
    int depth = 0;

    CCMovedName* moved = NULL;
    int moved_n = 0, moved_cap = 0;

    const char* cur = src;
    int line_no = 1;
    while ((size_t)(cur - src) < src_len && *cur) {
        const char* line_start = cur;
        const char* nl = strchr(cur, '\n');
        const char* line_end = nl ? nl : (src + src_len);
        size_t line_len = (size_t)(line_end - line_start);

        char tmp_line[2048];
        size_t cp = line_len < sizeof(tmp_line) - 1 ? line_len : sizeof(tmp_line) - 1;
        memcpy(tmp_line, line_start, cp);
        tmp_line[cp] = '\0';

        /* record decls */
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, depth, tmp_line);

        /* Implicit moves: move-only slice captures into closures move the captured value.
           To avoid falsely flagging uses inside the closure body, we apply the move *after* the closure ends. */
        if (closure_descs) {
            for (int ci = 0; ci < closure_count; ci++) {
                if ((closure_descs[ci].end_line + 1) != line_no) continue;
                for (int k = 0; k < closure_descs[ci].cap_count; k++) {
                    unsigned char fl = (closure_descs[ci].cap_flags ? closure_descs[ci].cap_flags[k] : 0);
                    if ((fl & 1) != 0 && (fl & 2) != 0) {
                        const char* nm = closure_descs[ci].cap_names ? closure_descs[ci].cap_names[k] : NULL;
                        if (nm) {
                            int md = depth;
                            int found_decl = 0;
                            /* Move should apply at the decl's scope depth so it survives exiting inner blocks. */
                            for (int d = depth; d >= 1 && !found_decl; d--) {
                                for (int i = 0; i < scope_counts[d]; i++) {
                                    if (!scope_names[d][i]) continue;
                                    if (strcmp(scope_names[d][i], nm) == 0) { md = d; found_decl = 1; break; }
                                }
                            }
                            cc__moved_push(&moved, &moved_n, &moved_cap, nm, strlen(nm), md);
                        }
                    }
                }
            }
        }

        /* scan tokens */
        const char* p = tmp_line;
        int in_str = 0;
        char q = 0;
        /* (reserved for future richer parsing) */
        while (*p) {
            char c = *p;
            if (in_str) {
                if (c == '\\' && p[1]) { p += 2; continue; }
                if (c == q) in_str = 0;
                p++;
                continue;
            }
            if (c == '"' || c == '\'') { in_str = 1; q = c; p++; continue; }
            if (!cc__is_ident_start_char(c)) { p++; continue; }

            const char* s = p++;
            while (cc__is_ident_char2(*p)) p++;
            size_t n = (size_t)(p - s);
            if (cc__is_keyword_tok(s, n)) continue;

            /* cc_move(name) marks name as moved if it's a CCSlice local */
            if (n == 7 && strncmp(s, "cc_move", 7) == 0) {
                const char* t = p;
                while (*t == ' ' || *t == '\t') t++;
                if (*t == '(') {
                    t++;
                    while (*t == ' ' || *t == '\t') t++;
                    const char* a = t;
                    if (cc__is_ident_start_char(*a)) {
                        a++;
                        while (cc__is_ident_char2(*a)) a++;
                        size_t an = (size_t)(a - t);
                        unsigned char fl = 0;
                        for (int d = depth; d >= 1 && fl == 0; d--) {
                            for (int i = 0; i < scope_counts[d]; i++) {
                                if (!scope_names[d][i]) continue;
                                if (strlen(scope_names[d][i]) == an && strncmp(scope_names[d][i], t, an) == 0) {
                                    fl = scope_flags[d] ? scope_flags[d][i] : 0;
                                    break;
                                }
                            }
                        }
                        if ((fl & 1) != 0) {
                            cc__moved_push(&moved, &moved_n, &moved_cap, t, an, depth);
                        }
                    }
                }
                continue;
            }

            /* If this identifier is being assigned to (simple `name =`), treat it as reinit (not a read). */
            const char* t2 = p;
            while (*t2 == ' ' || *t2 == '\t') t2++;
            if (*t2 == '=' && t2[1] != '=') {
                /* Reinitialization: allow assigning to a moved name. */
                continue;
            }

            /* Any later use of a moved slice is an error (best-effort). */
            if (cc__moved_contains(moved, moved_n, s, n)) {
                /* ignore member access like moved.ptr or moved->ptr? still use-after-move, so keep error */
                fprintf(stderr, "%s:%d: error: CC: use after move of slice '%.*s'\n",
                        src_path ? src_path : "<src>", line_no, (int)n, s);
                goto fail;
            }
        }

        /* Best-effort copy check: `lhs = rhs` where rhs is a move-only slice and lhs is a slice, and rhs is not cc_move(rhs). */
        {
            const char* eq = strchr(tmp_line, '=');
            if (eq && (eq[1] != '=')) {
                /* lhs: last identifier before '=' */
                const char* lhsp = tmp_line;
                const char* lhs_s = NULL;
                size_t lhs_n = 0;
                while (lhsp < eq) {
                    if (!cc__is_ident_start_char(*lhsp)) { lhsp++; continue; }
                    const char* ss = lhsp++;
                    while (cc__is_ident_char2(*lhsp)) lhsp++;
                    size_t nn = (size_t)(lhsp - ss);
                    if (cc__is_keyword_tok(ss, nn)) continue;
                    lhs_s = ss; lhs_n = nn;
                }
                /* rhs: first identifier after '=' (unless it's `cc_move(`). */
                const char* rhsp = eq + 1;
                while (*rhsp == ' ' || *rhsp == '\t') rhsp++;
                int rhs_is_move = 0;
                if (strncmp(rhsp, "cc_move", 7) == 0) rhs_is_move = 1;
                if (!rhs_is_move) {
                    while (*rhsp && !cc__is_ident_start_char(*rhsp)) rhsp++;
                    if (lhs_s && cc__is_ident_start_char(*rhsp)) {
                        const char* rs = rhsp++;
                        while (cc__is_ident_char2(*rhsp)) rhsp++;
                        size_t rn = (size_t)(rhsp - rs);

                        /* Look up lhs/rhs flags. */
                        unsigned char lhs_fl = 0, rhs_fl = 0;
                        for (int d = depth; d >= 1; d--) {
                            for (int i = 0; i < scope_counts[d]; i++) {
                                if (!scope_names[d][i]) continue;
                                if (lhs_fl == 0 && strlen(scope_names[d][i]) == lhs_n && strncmp(scope_names[d][i], lhs_s, lhs_n) == 0)
                                    lhs_fl = scope_flags[d] ? scope_flags[d][i] : 0;
                                if (rhs_fl == 0 && strlen(scope_names[d][i]) == rn && strncmp(scope_names[d][i], rs, rn) == 0)
                                    rhs_fl = scope_flags[d] ? scope_flags[d][i] : 0;
                            }
                        }
                        if ((lhs_fl & 1) != 0 && (rhs_fl & 1) != 0 && (rhs_fl & 2) != 0) {
                            fprintf(stderr, "%s:%d: error: CC: cannot copy move-only slice '%.*s' (use cc_move(%.*s))\n",
                                    src_path ? src_path : "<src>", line_no, (int)rn, rs, (int)rn, rs);
                            goto fail;
                        }
                    }
                }
            }
        }

        /* Update scope depth using braces in the original line (best-effort). */
        for (size_t i = 0; i < cp; i++) {
            if (tmp_line[i] == '{') depth++;
            else if (tmp_line[i] == '}') {
                /* leaving scope: drop decl tables for this depth */
                if (depth > 0) {
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_names[depth][j]);
                    free(scope_names[depth]); scope_names[depth] = NULL;
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_types[depth][j]);
                    free(scope_types[depth]); scope_types[depth] = NULL;
                    free(scope_flags[depth]); scope_flags[depth] = NULL;
                    scope_counts[depth] = 0;
                    depth--;
                }
                cc__moved_pop_depth(moved, &moved_n, depth);
            }
        }

        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    /* cleanup */
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
        for (int i = 0; i < scope_counts[d]; i++) free(scope_types[d][i]);
        free(scope_types[d]);
        free(scope_flags[d]);
    }
    if (closure_descs) {
        for (int i = 0; i < closure_count; i++) {
            for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_names[j]);
            free(closure_descs[i].cap_names);
            for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_types ? closure_descs[i].cap_types[j] : NULL);
            free(closure_descs[i].cap_types);
            free(closure_descs[i].cap_flags);
            free(closure_descs[i].param0_name);
            free(closure_descs[i].param1_name);
            free(closure_descs[i].param0_type);
            free(closure_descs[i].param1_type);
            free(closure_descs[i].body);
        }
        free(closure_descs);
    }
    for (int i = 0; i < moved_n; i++) free(moved[i].name);
    free(moved);
    return 0;

fail:
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
        for (int i = 0; i < scope_counts[d]; i++) free(scope_types[d][i]);
        free(scope_types[d]);
        free(scope_flags[d]);
    }
    if (closure_descs) {
        for (int i = 0; i < closure_count; i++) {
            for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_names[j]);
            free(closure_descs[i].cap_names);
            for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_types ? closure_descs[i].cap_types[j] : NULL);
            free(closure_descs[i].cap_types);
            free(closure_descs[i].cap_flags);
            free(closure_descs[i].param0_name);
            free(closure_descs[i].param1_name);
            free(closure_descs[i].param0_type);
            free(closure_descs[i].param1_type);
            free(closure_descs[i].body);
        }
        free(closure_descs);
    }
    for (int i = 0; i < moved_n; i++) free(moved[i].name);
    free(moved);
    return -1;
}

static void cc__collect_caps_from_block(char*** scope_names,
                                        int* scope_counts,
                                        int max_depth,
                                        const char* block,
                                        const char* ignore_name0,
                                        const char* ignore_name1,
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
        if (ignore_name0 && strlen(ignore_name0) == n && strncmp(ignore_name0, s, n) == 0) continue;
        if (ignore_name1 && strlen(ignore_name1) == n && strncmp(ignore_name1, s, n) == 0) continue;
        /* ignore member access */
        if (s > block && (s[-1] == '.' || (s[-1] == '>' && s > block + 1 && s[-2] == '-'))) continue;
        int found = 0;
        /* Only treat non-global names as captures for now.
           Globals (depth 0) can be referenced directly and should not force capture/env. */
        for (int d = max_depth; d >= 1 && !found; d--) {
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

static int cc__append_n(char** buf, size_t* len, size_t* cap, const char* s, size_t n) {
    if (!buf || !len || !cap || !s) return 0;
    if (n == 0) return 1;
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

static int cc__ab_only_ws_comments(const char* s, size_t a, size_t b) {
    if (!s) return 0;
    size_t i = a;
    while (i < b) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < b && s[i + 1] == '/') {
            i += 2;
            while (i < b && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < b && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < b && !(s[i] == '*' && s[i + 1] == '/')) i++;
            if (i + 1 < b) i += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

#ifdef CC_TCC_EXT_AVAILABLE
static size_t cc__offset_of_line_1based(const char* s, size_t len, int line_no);
static size_t cc__offset_of_line_col_1based(const char* s, size_t len, int line_no, int col_no);
static int cc__node_file_matches_this_tu(const CCASTRoot* root,
                                         const CCVisitorCtx* ctx,
                                         const char* file);
#endif

/* Forward decl: used by closure scan to recursively lower closure bodies. */
static char* cc__lower_cc_in_block_text(const char* text,
                                       size_t text_len,
                                       const char* src_path,
                                       int base_line,
                                       int* io_next_closure_id,
                                       char** out_more_protos,
                                       size_t* out_more_protos_len,
                                       char** out_more_defs,
                                       size_t* out_more_defs_len);

static int cc__append_fmt(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
    if (!buf || !len || !cap || !fmt) return 0;
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    tmp[sizeof(tmp) - 1] = '\0';
    return cc__append_str(buf, len, cap, tmp);
}

static int cc__rewrite_closure_calls_in_line(char*** scope_names,
                                             char*** scope_types,
                                             int* scope_counts,
                                             int depth,
                                             const char* line,
                                             char* out,
                                             size_t out_cap) {
    if (!scope_names || !scope_types || !scope_counts || !line || !out || out_cap == 0) return 0;
    out[0] = '\0';

    size_t in_len = strlen(line);
    if (in_len + 1 > out_cap) return 0;

    int changed = 0;
    size_t i = 0;
    size_t o = 0;

    int in_line_comment = 0;
    int in_block_comment = 0;
    int in_str = 0;
    char str_q = 0;

    while (i < in_len) {
        char c = line[i];

        if (in_line_comment) {
            out[o++] = c;
            if (c == '\n') in_line_comment = 0;
            i++;
            continue;
        }
        if (in_block_comment) {
            out[o++] = c;
            if (c == '*' && (i + 1) < in_len && line[i + 1] == '/') {
                out[o++] = '/';
                i += 2;
                in_block_comment = 0;
                continue;
            }
            i++;
            continue;
        }
        if (in_str) {
            out[o++] = c;
            if (c == '\\' && (i + 1) < in_len) {
                out[o++] = line[i + 1];
                i += 2;
                continue;
            }
            if (c == str_q) in_str = 0;
            i++;
            continue;
        }

        if (c == '/' && (i + 1) < in_len) {
            if (line[i + 1] == '/') { out[o++] = c; out[o++] = '/'; i += 2; in_line_comment = 1; continue; }
            if (line[i + 1] == '*') { out[o++] = c; out[o++] = '*'; i += 2; in_block_comment = 1; continue; }
        }
        if (c == '"' || c == '\'') { out[o++] = c; in_str = 1; str_q = c; i++; continue; }

        if (cc__is_ident_start_char(c)) {
            /* capture identifier */
            size_t name_s = i;
            i++;
            while (i < in_len && cc__is_ident_char2(line[i])) i++;
            size_t name_n = i - name_s;

            /* avoid member calls: .name( or ->name( */
            if (name_s > 0) {
                char prev = line[name_s - 1];
                if (prev == '.' || (prev == '>' && name_s > 1 && line[name_s - 2] == '-')) {
                    if (o + name_n >= out_cap) return 0;
                    memcpy(out + o, line + name_s, name_n);
                    o += name_n;
                    continue;
                }
            }

            /* skip ws */
            size_t j = i;
            while (j < in_len && (line[j] == ' ' || line[j] == '\t')) j++;
            if (j >= in_len || line[j] != '(') {
                if (o + name_n >= out_cap) return 0;
                memcpy(out + o, line + name_s, name_n);
                o += name_n;
                continue;
            }

            char name[128];
            if (name_n >= sizeof(name)) {
                if (o + name_n >= out_cap) return 0;
                memcpy(out + o, line + name_s, name_n);
                o += name_n;
                continue;
            }
            memcpy(name, line + name_s, name_n);
            name[name_n] = '\0';

            const char* ty = NULL;
            for (int d = depth; d >= 0 && !ty; d--) {
                ty = cc__lookup_decl_type(scope_names[d], scope_types[d], scope_counts[d], name);
            }
            int arity = 0;
            if (ty && strstr(ty, "CCClosure2")) arity = 2;
            else if (ty && strstr(ty, "CCClosure1")) arity = 1;
            if (arity == 0) {
                if (o + name_n >= out_cap) return 0;
                memcpy(out + o, line + name_s, name_n);
                o += name_n;
                continue;
            }

            /* find matching ')' from j ('(') */
            size_t args_s = j + 1;
            size_t k = args_s;
            int par = 0, brk = 0, br = 0;
            int ins = 0;
            char qch = 0;
            while (k < in_len) {
                char ch = line[k];
                if (ins) {
                    if (ch == '\\' && (k + 1) < in_len) { k += 2; continue; }
                    if (ch == qch) ins = 0;
                    k++;
                    continue;
                }
                if (ch == '"' || ch == '\'') { ins = 1; qch = ch; k++; continue; }
                if (ch == '(') par++;
                else if (ch == ')') {
                    if (par == 0 && brk == 0 && br == 0) break;
                    if (par > 0) par--;
                } else if (ch == '[') brk++;
                else if (ch == ']') { if (brk > 0) brk--; }
                else if (ch == '{') br++;
                else if (ch == '}') { if (br > 0) br--; }
                k++;
            }
            if (k >= in_len || line[k] != ')') {
                if (o + name_n >= out_cap) return 0;
                memcpy(out + o, line + name_s, name_n);
                o += name_n;
                continue;
            }
            size_t args_e = k; /* exclusive */

            /* count top-level commas in args */
            int commas = 0;
            {
                size_t z = args_s;
                int p2 = 0, b2 = 0, r2 = 0;
                int ins2 = 0;
                char q2 = 0;
                while (z < args_e) {
                    char ch = line[z++];
                    if (ins2) {
                        if (ch == '\\' && z < args_e) { z++; continue; }
                        if (ch == q2) ins2 = 0;
                        continue;
                    }
                    if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; continue; }
                    if (ch == '(') p2++;
                    else if (ch == ')') { if (p2) p2--; }
                    else if (ch == '[') b2++;
                    else if (ch == ']') { if (b2) b2--; }
                    else if (ch == '{') r2++;
                    else if (ch == '}') { if (r2) r2--; }
                    else if (ch == ',' && p2 == 0 && b2 == 0 && r2 == 0) commas++;
                }
            }
            if ((arity == 1 && commas != 0) || (arity == 2 && commas != 1)) {
                if (o + name_n >= out_cap) return 0;
                memcpy(out + o, line + name_s, name_n);
                o += name_n;
                continue;
            }

            const char* call_fn = (arity == 1) ? "cc_closure1_call" : "cc_closure2_call";
            size_t call_fn_n = strlen(call_fn);
            size_t args_n = args_e - args_s;

            if (arity == 1) {
                const char* castp = "(intptr_t)(";
                const char* cclose = ")";
                size_t need = call_fn_n + 1 + name_n + 2 + strlen(castp) + args_n + strlen(cclose) + 1;
                if (o + need >= out_cap) return 0;
                memcpy(out + o, call_fn, call_fn_n); o += call_fn_n;
                out[o++] = '(';
                memcpy(out + o, name, name_n); o += name_n;
                out[o++] = ',';
                out[o++] = ' ';
                memcpy(out + o, castp, strlen(castp)); o += strlen(castp);
                memcpy(out + o, line + args_s, args_n); o += args_n;
                memcpy(out + o, cclose, strlen(cclose)); o += strlen(cclose);
                out[o++] = ')';
            } else {
                /* Split arg0,arg1 at the first top-level comma */
                size_t comma_i = 0;
                {
                    size_t z = args_s;
                    int p2 = 0, b2 = 0, r2 = 0;
                    int ins2 = 0;
                    char q2 = 0;
                    while (z < args_e) {
                        char ch = line[z];
                        if (ins2) {
                            if (ch == '\\' && (z + 1) < args_e) { z += 2; continue; }
                            if (ch == q2) ins2 = 0;
                            z++;
                            continue;
                        }
                        if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; z++; continue; }
                        if (ch == '(') p2++;
                        else if (ch == ')') { if (p2) p2--; }
                        else if (ch == '[') b2++;
                        else if (ch == ']') { if (b2) b2--; }
                        else if (ch == '{') r2++;
                        else if (ch == '}') { if (r2) r2--; }
                        else if (ch == ',' && p2 == 0 && b2 == 0 && r2 == 0) { comma_i = z; break; }
                        z++;
                    }
                }
                if (comma_i <= args_s || comma_i >= args_e) return 0;
                size_t a0_s = args_s;
                size_t a0_e = comma_i;
                size_t a1_s = comma_i + 1;
                size_t a1_e = args_e;
                while (a0_e > a0_s && (line[a0_e - 1] == ' ' || line[a0_e - 1] == '\t')) a0_e--;
                while (a1_s < a1_e && (line[a1_s] == ' ' || line[a1_s] == '\t')) a1_s++;
                while (a1_e > a1_s && (line[a1_e - 1] == ' ' || line[a1_e - 1] == '\t')) a1_e--;
                size_t a0_n = a0_e - a0_s;
                size_t a1_n = a1_e - a1_s;
                const char* castp = "(intptr_t)(";
                const char* cclose = ")";
                size_t need = call_fn_n + 1 + name_n + 2 +
                              strlen(castp) + a0_n + strlen(cclose) + 2 +
                              strlen(castp) + a1_n + strlen(cclose) + 1;
                if (o + need >= out_cap) return 0;
                memcpy(out + o, call_fn, call_fn_n); o += call_fn_n;
                out[o++] = '(';
                memcpy(out + o, name, name_n); o += name_n;
                out[o++] = ',';
                out[o++] = ' ';
                memcpy(out + o, castp, strlen(castp)); o += strlen(castp);
                memcpy(out + o, line + a0_s, a0_n); o += a0_n;
                memcpy(out + o, cclose, strlen(cclose)); o += strlen(cclose);
                out[o++] = ',';
                out[o++] = ' ';
                memcpy(out + o, castp, strlen(castp)); o += strlen(castp);
                memcpy(out + o, line + a1_s, a1_n); o += a1_n;
                memcpy(out + o, cclose, strlen(cclose)); o += strlen(cclose);
                out[o++] = ')';
            }
            changed = 1;

            /* advance i to after ')' */
            i = k + 1;
            continue;
        }

        out[o++] = c;
        i++;
        if (o + 1 >= out_cap) return 0;
    }

    out[o] = '\0';
    return changed;
}

typedef struct {
    int line_start;
    int col_start; /* 1-based, points at '(' token */
    int line_end;
    int col_end;   /* 1-based, exclusive */
} CCStubCallSpan;

static int cc__linecol_to_offset(const char* s, size_t n, int line1, int col1, size_t* out_off) {
    if (!s || !out_off || line1 <= 0 || col1 <= 0) return 0;
    int line = 1;
    int col = 1;
    for (size_t i = 0; i < n; i++) {
        if (line == line1 && col == col1) { *out_off = i; return 1; }
        char c = s[i];
        if (c == '\n') { line++; col = 1; continue; }
        col++;
    }
    if (line == line1 && col == col1) { *out_off = n; return 1; }
    return 0;
}

static int cc__rewrite_multiline_closure_call_chunk(char*** scope_names,
                                                    char*** scope_types,
                                                    int* scope_counts,
                                                    int depth,
                                                    const CCStubCallSpan* sp,
                                                    const char* chunk,
                                                    size_t chunk_len,
                                                    char** out_chunk,
                                                    size_t* out_len) {
    if (!scope_names || !scope_types || !scope_counts || !sp || !chunk || !out_chunk || !out_len) return 0;
    *out_chunk = NULL;
    *out_len = 0;

    size_t lparen_off = 0;
    size_t end_off = 0;
    if (sp->col_start > 0 && sp->col_end > 0) {
        if (!cc__linecol_to_offset(chunk, chunk_len, 1, sp->col_start, &lparen_off)) return 0;
        if (!cc__linecol_to_offset(chunk, chunk_len, (sp->line_end - sp->line_start + 1), sp->col_end, &end_off)) return 0;
    } else {
        /* Fallback: find the call parens by scanning text (works even if col tracking is missing). */
        const char* s = chunk;
        while ((size_t)(s - chunk) < chunk_len && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
        if (!cc__is_ident_start_char(*s)) return 0;
        while ((size_t)(s - chunk) < chunk_len && cc__is_ident_char2(*s)) s++;
        while ((size_t)(s - chunk) < chunk_len && (*s == ' ' || *s == '\t')) s++;
        if (*s != '(') return 0;
        lparen_off = (size_t)(s - chunk);
        /* find matching ')' */
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        size_t i = lparen_off + 1;
        for (; i < chunk_len; i++) {
            char ch = chunk[i];
            if (ins) {
                if (ch == '\\' && i + 1 < chunk_len) { i++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') {
                if (par == 0 && brk == 0 && br == 0) { end_off = i + 1; break; }
                if (par) par--;
            } else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
        }
        if (!end_off) return 0;
    }
    if (lparen_off >= chunk_len || end_off > chunk_len || end_off <= lparen_off) return 0;

    /* Scan left from '(' to find callee identifier. */
    size_t k = lparen_off;
    while (k > 0 && (chunk[k - 1] == ' ' || chunk[k - 1] == '\t' || chunk[k - 1] == '\r' || chunk[k - 1] == '\n')) k--;
    size_t name_end = k;
    while (k > 0 && cc__is_ident_char2(chunk[k - 1])) k--;
    size_t name_start = k;
    if (name_start == name_end || !cc__is_ident_start_char(chunk[name_start])) return 0;

    char name[128];
    size_t name_n = name_end - name_start;
    if (name_n >= sizeof(name)) return 0;
    memcpy(name, chunk + name_start, name_n);
    name[name_n] = '\0';

    const char* ty = NULL;
    for (int d = depth; d >= 0 && !ty; d--) ty = cc__lookup_decl_type(scope_names[d], scope_types[d], scope_counts[d], name);
    int arity = 0;
    if (ty && strstr(ty, "CCClosure2")) arity = 2;
    else if (ty && strstr(ty, "CCClosure1")) arity = 1;
    if (arity == 0) return 0;

    /* args text inside parens */
    size_t args_s = lparen_off + 1;
    /* find matching ')' before end_off */
    size_t rparen_off = end_off;
    while (rparen_off > args_s && chunk[rparen_off - 1] != ')') rparen_off--;
    if (rparen_off <= args_s || chunk[rparen_off - 1] != ')') return 0;
    size_t args_e = rparen_off - 1;

    /* Split args for arity 2 at first top-level comma. */
    size_t a0_s = args_s, a0_e = args_e, a1_s = args_e, a1_e = args_e;
    if (arity == 2) {
        size_t comma = 0;
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        for (size_t i = args_s; i < args_e; i++) {
            char ch = chunk[i];
            if (ins) {
                if (ch == '\\' && i + 1 < args_e) { i++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
            else if (ch == ',' && par == 0 && brk == 0 && br == 0) { comma = i; break; }
        }
        if (!comma) return 0;
        a0_s = args_s; a0_e = comma;
        a1_s = comma + 1; a1_e = args_e;
    }

    /* Trim whitespace */
    while (a0_s < a0_e && (chunk[a0_s] == ' ' || chunk[a0_s] == '\t' || chunk[a0_s] == '\n' || chunk[a0_s] == '\r')) a0_s++;
    while (a0_e > a0_s && (chunk[a0_e - 1] == ' ' || chunk[a0_e - 1] == '\t' || chunk[a0_e - 1] == '\n' || chunk[a0_e - 1] == '\r')) a0_e--;
    while (a1_s < a1_e && (chunk[a1_s] == ' ' || chunk[a1_s] == '\t' || chunk[a1_s] == '\n' || chunk[a1_s] == '\r')) a1_s++;
    while (a1_e > a1_s && (chunk[a1_e - 1] == ' ' || chunk[a1_e - 1] == '\t' || chunk[a1_e - 1] == '\n' || chunk[a1_e - 1] == '\r')) a1_e--;

    const char* fn = (arity == 1) ? "cc_closure1_call" : "cc_closure2_call";
    char repl[2048];
    if (arity == 1) {
        snprintf(repl, sizeof(repl), "%s(%s, (intptr_t)(%.*s))", fn, name, (int)(a0_e - a0_s), chunk + a0_s);
    } else {
        snprintf(repl, sizeof(repl), "%s(%s, (intptr_t)(%.*s), (intptr_t)(%.*s))",
                 fn, name,
                 (int)(a0_e - a0_s), chunk + a0_s,
                 (int)(a1_e - a1_s), chunk + a1_s);
    }

    /* Replace [name_start, end_off) with repl */
    size_t pre_n = name_start;
    size_t post_n = chunk_len - end_off;
    size_t repl_n = strlen(repl);
    size_t outn = pre_n + repl_n + post_n;
    char* outb = (char*)malloc(outn + 1);
    if (!outb) return 0;
    memcpy(outb, chunk, pre_n);
    memcpy(outb + pre_n, repl, repl_n);
    memcpy(outb + pre_n + repl_n, chunk + end_off, post_n);
    outb[outn] = '\0';
    *out_chunk = outb;
    *out_len = outn;
    return 1;
}

typedef struct {
    int line_start;
    int col_start;
    int line_end;
    int col_end;
    const char* callee; /* identifier */
    int occ_1based;     /* Nth occurrence of this callee call on the start line */
    int arity;          /* 1 or 2 */
} CCClosureCallNode;

static int cc__is_word_boundary(char c) {
    return !(cc__is_ident_char2(c));
}

static int cc__find_nth_callee_call_span_in_range(const char* s,
                                                  size_t range_start,
                                                  size_t range_end,
                                                  const char* callee,
                                                  int occ_1based,
                                                  size_t* out_name_start,
                                                  size_t* out_lparen,
                                                  size_t* out_rparen_end) {
    if (!s || !callee || !out_name_start || !out_lparen || !out_rparen_end) return 0;
    if (occ_1based <= 0) occ_1based = 1;
    size_t n = strlen(callee);
    if (n == 0) return 0;
    if (range_end <= range_start) return 0;

    int occ = 0;
    for (size_t i = range_start; i + n < range_end; i++) {
        if (memcmp(s + i, callee, n) != 0) continue;
        char before = (i == 0) ? '\0' : s[i - 1];
        char after = (i + n < range_end) ? s[i + n] : '\0';
        if (i > 0 && !cc__is_word_boundary(before)) continue;
        if (i + n < range_end && !cc__is_word_boundary(after) && after != ' ' && after != '\t' && after != '\n' && after != '\r') continue;

        size_t j = i + n;
        while (j < range_end && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' || s[j] == '\r')) j++;
        if (j >= range_end || s[j] != '(') continue;
        occ++;
        if (occ != occ_1based) continue;

        size_t lparen = j;
        /* Find matching ')' */
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        for (size_t k = lparen + 1; k < range_end; k++) {
            char ch = s[k];
            if (ins) {
                if (ch == '\\' && k + 1 < range_end) { k++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') {
                if (par == 0 && brk == 0 && br == 0) {
                    *out_name_start = i;
                    *out_lparen = lparen;
                    *out_rparen_end = k + 1;
                    return 1;
                }
                if (par) par--;
            } else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
        }
        return 0;
    }
    return 0;
}

typedef struct {
    size_t name_start;
    size_t lparen;
    size_t rparen_end;
    int arity;
    int parent;          /* index in spans array, -1 if none */
    int* children;
    int child_n;
    int child_cap;
} CCClosureCallSpan;

static void cc__span_add_child(CCClosureCallSpan* spans, int parent, int child) {
    if (!spans || parent < 0 || child < 0) return;
    CCClosureCallSpan* p = &spans[parent];
    if (p->child_n == p->child_cap) {
        p->child_cap = p->child_cap ? p->child_cap * 2 : 4;
        p->children = (int*)realloc(p->children, (size_t)p->child_cap * sizeof(int));
        if (!p->children) { p->child_cap = 0; p->child_n = 0; return; }
    }
    p->children[p->child_n++] = child;
}

static void cc__emit_range_with_call_spans(const char* src,
                                           size_t start,
                                           size_t end,
                                           const CCClosureCallSpan* spans,
                                           int span_idx,
                                           char** io_out,
                                           size_t* io_len,
                                           size_t* io_cap);

static void cc__emit_arg_range(const char* src,
                               size_t a0,
                               size_t a1,
                               const CCClosureCallSpan* spans,
                               int span_idx,
                               char** io_out,
                               size_t* io_len,
                               size_t* io_cap) {
    cc__emit_range_with_call_spans(src, a0, a1, spans, span_idx, io_out, io_len, io_cap);
}

static void cc__emit_call_replacement(const char* src,
                                      const char* callee,
                                      const CCClosureCallSpan* spans,
                                      int span_idx,
                                      char** io_out,
                                      size_t* io_len,
                                      size_t* io_cap) {
    const CCClosureCallSpan* sp = &spans[span_idx];
    size_t args_s = sp->lparen + 1;
    size_t args_e = sp->rparen_end - 1;
    /* Find comma for arity=2 in original args text (top-level only). */
    size_t comma = 0;
    if (sp->arity == 2) {
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        for (size_t i = args_s; i < args_e; i++) {
            char ch = src[i];
            if (ins) {
                if (ch == '\\' && i + 1 < args_e) { i++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
            else if (ch == ',' && par == 0 && brk == 0 && br == 0) { comma = i; break; }
        }
        if (!comma) return; /* malformed; emit nothing */
    }

    const char* fn = (sp->arity == 1) ? "cc_closure1_call" : "cc_closure2_call";
    cc__append_fmt(io_out, io_len, io_cap, "%s(%s, (intptr_t)(", fn, callee);
    if (sp->arity == 1) {
        cc__emit_arg_range(src, args_s, args_e, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "))");
    } else {
        cc__emit_arg_range(src, args_s, comma, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "), (intptr_t)(");
        cc__emit_arg_range(src, comma + 1, args_e, spans, span_idx, io_out, io_len, io_cap);
        cc__append_str(io_out, io_len, io_cap, "))");
    }
}

static void cc__emit_range_with_call_spans(const char* src,
                                           size_t start,
                                           size_t end,
                                           const CCClosureCallSpan* spans,
                                           int span_idx,
                                           char** io_out,
                                           size_t* io_len,
                                           size_t* io_cap) {
    if (!src || !spans || !io_out || !io_len || !io_cap) return;
    const CCClosureCallSpan* sp = &spans[span_idx];
    /* Walk direct children and emit text around them. */
    size_t cur = start;
    for (int ci = 0; ci < sp->child_n; ci++) {
        int child = sp->children[ci];
        const CCClosureCallSpan* csp = &spans[child];
        if (csp->name_start < start || csp->rparen_end > end) continue;
        if (csp->name_start > cur) {
            cc__append_n(io_out, io_len, io_cap, src + cur, csp->name_start - cur);
        }
        /* Emit rewritten child call */
        /* Callee name is the identifier between name_start and lparen (trim). */
        size_t nm_s = csp->name_start;
        size_t nm_e = csp->lparen;
        while (nm_e > nm_s && (src[nm_e - 1] == ' ' || src[nm_e - 1] == '\t' || src[nm_e - 1] == '\n' || src[nm_e - 1] == '\r')) nm_e--;
        char nm[128];
        size_t nn = nm_e > nm_s ? (nm_e - nm_s) : 0;
        if (nn > 0 && nn < sizeof(nm)) {
            memcpy(nm, src + nm_s, nn);
            nm[nn] = '\0';
            cc__emit_call_replacement(src, nm, spans, child, io_out, io_len, io_cap);
        } else {
            cc__append_n(io_out, io_len, io_cap, src + csp->name_start, csp->rparen_end - csp->name_start);
        }
        cur = csp->rparen_end;
    }
    if (cur < end) cc__append_n(io_out, io_len, io_cap, src + cur, end - cur);
}

static int cc__rewrite_all_closure_calls_with_nodes(const CCASTRoot* root,
                                                    const CCVisitorCtx* ctx,
                                                    const char* in_src,
                                                    size_t in_len,
                                                    char** out_src,
                                                    size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    /* Collect non-UFCS CALL nodes with a callee name. */
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

    CCClosureCallNode* calls = NULL;
    int call_n = 0;
    int call_cap = 0;
    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue; /* CALL */
        int is_ufcs = (n[i].aux2 & 2) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue;
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
        if (call_n == call_cap) {
            call_cap = call_cap ? call_cap * 2 : 64;
            calls = (CCClosureCallNode*)realloc(calls, (size_t)call_cap * sizeof(*calls));
            if (!calls) return 0;
        }
        calls[call_n++] = (CCClosureCallNode){
            .line_start = n[i].line_start,
            .col_start = n[i].col_start,
            .line_end = n[i].line_end,
            .col_end = n[i].col_end,
            .callee = n[i].aux_s1,
            .occ_1based = 1,
            .arity = 0,
        };
    }
    if (call_n == 0) { free(calls); return 0; }

    /* Sort by (line_start, col_start). */
    for (int i = 0; i < call_n; i++) {
        for (int j = i + 1; j < call_n; j++) {
            int swap = 0;
            if (calls[j].line_start < calls[i].line_start) swap = 1;
            else if (calls[j].line_start == calls[i].line_start && calls[j].col_start < calls[i].col_start) swap = 1;
            if (swap) { CCClosureCallNode t = calls[i]; calls[i] = calls[j]; calls[j] = t; }
        }
    }

    /* Assign occurrence per (line_start, callee) so we can find spans after prior rewrites. */
    for (int i = 0; i < call_n; i++) {
        int occ = 1;
        for (int j = 0; j < i; j++) {
            if (calls[j].line_start == calls[i].line_start && calls[j].callee && calls[i].callee &&
                strcmp(calls[j].callee, calls[i].callee) == 0) occ++;
        }
        calls[i].occ_1based = occ;
    }

    /* Best-effort: build a global decl table (depth 0) for CCClosure1/2 vars. */
    char** decl_names[1] = {0};
    char** decl_types[1] = {0};
    unsigned char* decl_flags[1] = {0};
    int decl_counts[1] = {0};
    {
        const char* cur = in_src;
        const char* end = in_src + in_len;
        while (cur < end) {
            const char* nl = memchr(cur, '\n', (size_t)(end - cur));
            size_t ll = nl ? (size_t)(nl - cur) : (size_t)(end - cur);
            char tmp[1024];
            size_t cp = ll < sizeof(tmp) - 1 ? ll : sizeof(tmp) - 1;
            memcpy(tmp, cur, cp);
            tmp[cp] = '\0';
            cc__maybe_record_decl(decl_names, decl_types, decl_flags, decl_counts, 0, tmp);
            if (!nl) break;
            cur = nl + 1;
        }
    }

    /* Determine arity for each call based on declared type of the callee identifier. */
    int rewrite_n = 0;
    for (int i = 0; i < call_n; i++) {
        const char* ty = cc__lookup_decl_type(decl_names[0], decl_types[0], decl_counts[0], calls[i].callee);
        if (!ty) continue;
        if (strstr(ty, "CCClosure2")) calls[i].arity = 2;
        else if (strstr(ty, "CCClosure1")) calls[i].arity = 1;
        if (calls[i].arity) rewrite_n++;
    }
    if (rewrite_n == 0) {
        for (int i = 0; i < decl_counts[0]; i++) { free(decl_names[0][i]); free(decl_types[0][i]); }
        free(decl_names[0]); free(decl_types[0]); free(decl_flags[0]);
        free(calls);
        return 0;
    }

    /* Build call spans for closure calls. */
    CCClosureCallSpan* spans = (CCClosureCallSpan*)calloc((size_t)rewrite_n, sizeof(*spans));
    if (!spans) return 0;
    int sn = 0;
    for (int i = 0; i < call_n; i++) {
        if (!calls[i].arity) continue;
        /* Range based on lines [line_start, line_end]. */
        size_t rs = cc__offset_of_line_1based(in_src, in_len, calls[i].line_start);
        size_t re = cc__offset_of_line_1based(in_src, in_len, calls[i].line_end + 1);
        if (re > in_len) re = in_len;
        size_t nm_s = 0, lp = 0, rp_end = 0;
        if (!cc__find_nth_callee_call_span_in_range(in_src, rs, re, calls[i].callee, calls[i].occ_1based, &nm_s, &lp, &rp_end))
            continue;
        spans[sn++] = (CCClosureCallSpan){
            .name_start = nm_s,
            .lparen = lp,
            .rparen_end = rp_end,
            .arity = calls[i].arity,
            .parent = -1,
            .children = NULL,
            .child_n = 0,
            .child_cap = 0,
        };
    }
    if (sn == 0) { free(spans); spans = NULL; }

    /* Clean decl table */
    for (int i = 0; i < decl_counts[0]; i++) { free(decl_names[0][i]); free(decl_types[0][i]); }
    free(decl_names[0]); free(decl_types[0]); free(decl_flags[0]);
    free(calls);
    if (!spans) return 0;

    /* Sort spans by (name_start asc, rparen_end desc) to build nesting. */
    for (int i = 0; i < sn; i++) {
        for (int j = i + 1; j < sn; j++) {
            int swap = 0;
            if (spans[j].name_start < spans[i].name_start) swap = 1;
            else if (spans[j].name_start == spans[i].name_start && spans[j].rparen_end > spans[i].rparen_end) swap = 1;
            if (swap) { CCClosureCallSpan t = spans[i]; spans[i] = spans[j]; spans[j] = t; }
        }
    }

    int stack[256];
    int sp = 0;
    for (int i = 0; i < sn; i++) {
        while (sp > 0) {
            int top = stack[sp - 1];
            if (spans[i].name_start >= spans[top].rparen_end) { sp--; continue; }
            break;
        }
        if (sp > 0) {
            int parent = stack[sp - 1];
            spans[i].parent = parent;
            cc__span_add_child(spans, parent, i);
        }
        if (sp < (int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++] = i;
    }

    /* Emit rewritten source */
    char* out = NULL;
    size_t out_len2 = 0, out_cap2 = 0;
    size_t cur = 0;
    for (int i = 0; i < sn; i++) {
        if (spans[i].parent != -1) continue;
        if (spans[i].name_start > cur) cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, spans[i].name_start - cur);
        /* Emit rewritten call */
        size_t nm_s = spans[i].name_start;
        size_t nm_e = spans[i].lparen;
        while (nm_e > nm_s && (in_src[nm_e - 1] == ' ' || in_src[nm_e - 1] == '\t' || in_src[nm_e - 1] == '\n' || in_src[nm_e - 1] == '\r')) nm_e--;
        char nm[128];
        size_t nn = nm_e > nm_s ? (nm_e - nm_s) : 0;
        if (nn > 0 && nn < sizeof(nm)) {
            memcpy(nm, in_src + nm_s, nn);
            nm[nn] = '\0';
            cc__emit_call_replacement(in_src, nm, spans, i, &out, &out_len2, &out_cap2);
        } else {
            cc__append_n(&out, &out_len2, &out_cap2, in_src + spans[i].name_start, spans[i].rparen_end - spans[i].name_start);
        }
        cur = spans[i].rparen_end;
    }
    if (cur < in_len) cc__append_n(&out, &out_len2, &out_cap2, in_src + cur, in_len - cur);

    for (int i = 0; i < sn; i++) free(spans[i].children);
    free(spans);

    if (!out) return 0;
    *out_src = out;
    *out_len = out_len2;
    return 1;
}

#ifdef CC_TCC_EXT_AVAILABLE
static int cc__rewrite_async_state_machine_noarg(const CCASTRoot* root,
                                                 const CCVisitorCtx* ctx,
                                                 const char* in_src,
                                                 size_t in_len,
                                                 char** out_src,
                                                 size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    typedef struct NodeView {
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
    const NodeView* n = (const NodeView*)root->nodes;

    typedef struct {
        size_t start;
        size_t end;
        int line_start;
        int line_end;
        const char* name;
        int is_await;
        char expr[256];
        char callee[128];
        int is_intptr;
    } AsyncFn;
    AsyncFn fns[64];
    int fn_n = 0;

    for (int i = 0; i < root->node_count && fn_n < (int)(sizeof(fns)/sizeof(fns[0])); i++) {
        if (n[i].kind != 12) continue; /* DECL_ITEM */
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
        if (!n[i].aux_s1 || !n[i].aux_s2) continue;
        if (((unsigned int)n[i].aux2 & (1u << 0)) == 0) continue; /* CC_FN_ATTR_ASYNC */

        /* We only support `@async int|intptr_t name(void) { return ...; }` for now. */
        if (!strchr(n[i].aux_s2, '(')) continue;
        const char* lp = strchr(n[i].aux_s2, '(');
        const char* rp = strrchr(n[i].aux_s2, ')');
        if (!lp || !rp || rp < lp) continue;
        /* Ensure params are empty or void. */
        int ok_params = 0;
        {
            const char* p = lp + 1;
            while (p < rp && (*p == ' ' || *p == '\t')) p++;
            if (p == rp) ok_params = 1;
            else if ((rp - p) == 4 && memcmp(p, "void", 4) == 0) ok_params = 1;
        }
        if (!ok_params) continue;

        int is_intptr = (strstr(n[i].aux_s2, "intptr_t") != NULL);
        if (!is_intptr && strstr(n[i].aux_s2, "int") == NULL) continue;

        /* Compute function span by scanning for the first `{` and matching braces. */
        int ls = n[i].line_start;
        if (ls <= 0) continue;
        size_t start = cc__offset_of_line_1based(in_src, in_len, ls);
        if (start >= in_len) continue;
        size_t p = start;
        /* Find `{` */
        while (p < in_len && in_src[p] != '{') p++;
        if (p >= in_len) continue;
        size_t body_lbrace = p;
        /* Match braces */
        int depth = 0;
        size_t q = body_lbrace;
        for (; q < in_len; q++) {
            char ch = in_src[q];
            if (ch == '"' || ch == '\'') {
                char quote = ch;
                q++;
                while (q < in_len) {
                    char c2 = in_src[q];
                    if (c2 == '\\' && q + 1 < in_len) { q += 2; continue; }
                    if (c2 == quote) break;
                    q++;
                }
                continue;
            }
            if (ch == '{') depth++;
            else if (ch == '}') {
                depth--;
                if (depth == 0) { q++; break; } /* include '}' */
            }
        }
        if (depth != 0) continue;
        size_t end = q;
        if (end > in_len) end = in_len;
        /* Extend to include trailing newline, if any. */
        while (end < in_len && in_src[end] != '\n') end++;
        if (end < in_len) end++;
        if (end <= start) continue;

        const char* body = in_src + body_lbrace + 1;
        const char* body_end = in_src + (q - 1); /* points at matching '}' */
        while (body < body_end && (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r')) body++;
        /* Expect `return ...;` */
        if ((body_end - body) < 6 || memcmp(body, "return", 6) != 0) continue;
        body += 6;
        while (body < body_end && (*body == ' ' || *body == '\t')) body++;
        int is_await = 0;
        if ((body_end - body) >= 5 && memcmp(body, "await", 5) == 0) {
            is_await = 1;
            body += 5;
            while (body < body_end && (*body == ' ' || *body == '\t')) body++;
        }
        const char* semi = memchr(body, ';', (size_t)(body_end - body));
        if (!semi) continue;
        /* Ensure only whitespace after ';' up to '}' */
        const char* tail = semi + 1;
        while (tail < body_end && (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r')) tail++;
        if (tail != body_end) continue;

        AsyncFn fn;
        memset(&fn, 0, sizeof(fn));
        fn.start = start;
        fn.end = end;
        fn.line_start = ls;
        fn.line_end = n[i].line_end ? n[i].line_end : n[i].line_start;
        fn.name = n[i].aux_s1;
        fn.is_await = is_await;
        fn.is_intptr = is_intptr;
        size_t expr_n = (size_t)(semi - body);
        if (expr_n >= sizeof(fn.expr)) continue;
        memcpy(fn.expr, body, expr_n);
        fn.expr[expr_n] = 0;

        if (is_await) {
            /* Require expr is a no-arg call: ident() */
            const char* lpc = strchr(fn.expr, '(');
            const char* rpc = strrchr(fn.expr, ')');
            if (!lpc || !rpc || rpc < lpc) continue;
            /* require inside parens whitespace only */
            const char* ap = lpc + 1;
            while (ap < rpc && (*ap == ' ' || *ap == '\t' || *ap == '\n' || *ap == '\r')) ap++;
            if (ap != rpc) continue;
            size_t callee_n = (size_t)(lpc - fn.expr);
            while (callee_n > 0 && (fn.expr[callee_n - 1] == ' ' || fn.expr[callee_n - 1] == '\t')) callee_n--;
            if (callee_n == 0 || callee_n >= sizeof(fn.callee)) continue;
            memcpy(fn.callee, fn.expr, callee_n);
            fn.callee[callee_n] = 0;
        }

        fns[fn_n++] = fn;
    }

    if (fn_n == 0) return 0;

    /* Apply replacements from end to start. Keep newline count identical by emitting one-line replacements
       and padding with newlines to match the original slice newline count. */
    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return 0;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t cur_len = in_len;

    static int g_async_id = 1;
    for (int fi = fn_n - 1; fi >= 0; fi--) {
        AsyncFn* fn = &fns[fi];
        if (fn->start >= fn->end || fn->end > cur_len) continue;

        /* Count original newlines */
        int orig_nl = 0;
        for (size_t k = fn->start; k < fn->end; k++) if (cur[k] == '\n') orig_nl++;

        int id = g_async_id++;
        char repl[4096];
        int rn = 0;
        if (!fn->is_await) {
            rn = snprintf(repl, sizeof(repl),
                          "typedef struct{int __st; intptr_t __r;}__cc_af%d_f;"
                          "static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){(void)__e;__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:__f->__r=(intptr_t)(%s);__f->__st=1;/*fall*/case 1:if(__o)*__o=__f->__r;return CC_FUTURE_READY;}return CC_FUTURE_ERR;}"
                          "static void __cc_af%d_drop(void*__p){free(__p);}"
                          "CCTaskIntptr %s(void){__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;return cc_task_intptr_make_poll(__cc_af%d_poll,__f,__cc_af%d_drop);}",
                          id, id, id, id, fn->expr, id, fn->name, id, id, id, id, id);
        } else {
            rn = snprintf(repl, sizeof(repl),
                          "typedef struct{int __st; CCTaskIntptr __t; intptr_t __r;}__cc_af%d_f;"
                          "static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:__f->__t=%s();__f->__st=1;/*fall*/case 1:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t,&__v,&__err);if(__st==CC_FUTURE_PENDING){return CC_FUTURE_PENDING;}cc_task_intptr_free(&__f->__t);(void)__e; if(__o)*__o=__v; __f->__r=__v; __f->__st=2;return CC_FUTURE_READY;}case 2:if(__o)*__o=__f->__r;return CC_FUTURE_READY;}return CC_FUTURE_ERR;}"
                          "static void __cc_af%d_drop(void*__p){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(__f){cc_task_intptr_free(&__f->__t);free(__f);}}"
                          "CCTaskIntptr %s(void){__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;memset(&__f->__t,0,sizeof(__f->__t));return cc_task_intptr_make_poll(__cc_af%d_poll,__f,__cc_af%d_drop);}",
                          id, id, id, id, fn->callee, id, id, id, fn->name, id, id, id, id, id);
        }
        if (rn <= 0 || (size_t)rn >= sizeof(repl)) continue;

        /* Count repl newlines */
        int repl_nl = 0;
        for (int k = 0; k < rn; k++) if (repl[k] == '\n') repl_nl++;
        if (repl_nl > orig_nl) {
            /* We promised not to increase lines; skip. */
            continue;
        }

        /* Pad with newlines to keep line mapping stable */
        char* padded = NULL;
        size_t padded_len = (size_t)rn + (size_t)(orig_nl - repl_nl);
        padded = (char*)malloc(padded_len + 1);
        if (!padded) continue;
        memcpy(padded, repl, (size_t)rn);
        for (int k = 0; k < (orig_nl - repl_nl); k++) padded[rn + k] = '\n';
        padded[padded_len] = 0;

        size_t new_len = cur_len - (fn->end - fn->start) + padded_len;
        char* next = (char*)malloc(new_len + 1);
        if (!next) { free(padded); continue; }
        memcpy(next, cur, fn->start);
        memcpy(next + fn->start, padded, padded_len);
        memcpy(next + fn->start + padded_len, cur + fn->end, cur_len - fn->end);
        next[new_len] = 0;
        free(padded);
        free(cur);
        cur = next;
        cur_len = new_len;
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}
#endif

enum {
    CC_FN_ATTR_ASYNC = 1u << 0,
    CC_FN_ATTR_NOBLOCK = 1u << 1,
    CC_FN_ATTR_LATENCY_SENSITIVE = 1u << 2,
};

static int cc__rewrite_autoblocking_calls_with_nodes(const CCASTRoot* root,
                                                     const CCVisitorCtx* ctx,
                                                     const char* in_src,
                                                     size_t in_len,
                                                     char** out_src,
                                                     size_t* out_len) {
    if (!root || !ctx || !ctx->symbols || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    const struct NodeView {
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
    }* n = (const struct NodeView*)root->nodes;

    typedef enum {
        CC_AB_REWRITE_STMT_CALL = 0,
        CC_AB_REWRITE_RETURN_CALL = 1,
        CC_AB_REWRITE_ASSIGN_CALL = 2,
        CC_AB_REWRITE_BATCH_STMT_CALLS = 3,
        CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN = 4,
        CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN = 5,
        CC_AB_REWRITE_RETURN_EXPR_CALL = 6,
        CC_AB_REWRITE_ASSIGN_EXPR_CALL = 7,
    } CCAutoBlockRewriteKind;

    typedef struct {
        size_t call_start;
        size_t call_end;
        int line_start;
        const char* callee;
        int argc;
        char* param_types[16]; /* owned */
    } CCAutoBlockBatchItem;

    typedef struct {
        size_t start;     /* statement start */
        size_t end;       /* statement end (inclusive of ';') */
        size_t call_start;
        size_t call_end;  /* end of ')' */
        int line_start;
        const char* callee;
        const char* lhs_name; /* for assign */
        CCAutoBlockRewriteKind kind;
        int argc;
        int ret_is_ptr;
        int ret_is_void;
        int ret_is_structy;
        char* param_types[16]; /* owned */
        size_t indent_start;
        size_t indent_len;
        CCAutoBlockBatchItem* batch;
        int batch_n;
        /* Optional trailing value-producing call to fold into the same dispatch. */
        int tail_kind; /* 0 none, 1 return, 2 assign */
        size_t tail_call_start;
        size_t tail_call_end;
        const char* tail_callee;
        const char* tail_lhs_name;
        int tail_argc;
        int tail_ret_is_ptr;
        char* tail_param_types[16]; /* owned */
    } Replace;
    Replace* reps = NULL;
    int rep_n = 0;
    int rep_cap = 0;

    for (int i = 0; i < root->node_count; i++) {
        if (n[i].kind != 5) continue; /* CALL */
        int is_ufcs = (n[i].aux2 & 2) != 0;
        if (is_ufcs) continue;
        if (!n[i].aux_s1) continue; /* callee name */
        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;

        /* Find enclosing function decl-item and check @async attr. */
        int cur = n[i].parent;
        const char* owner = NULL;
        unsigned int owner_attrs = 0;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12 && n[cur].aux_s1 && n[cur].aux_s2 && strchr(n[cur].aux_s2, '(') &&
                cc__node_file_matches_this_tu(root, ctx, n[cur].file)) {
                owner = n[cur].aux_s1;
                owner_attrs = (unsigned int)n[cur].aux2;
                break;
            }
            cur = n[cur].parent;
        }
        if (!owner || ((owner_attrs & CC_FN_ATTR_ASYNC) == 0)) continue;

        /* Only skip known-nonblocking callees; if we don't know attrs, assume blocking. */
        unsigned int callee_attrs = 0;
        (void)cc_symbols_lookup_fn_attrs(ctx->symbols, n[i].aux_s1, &callee_attrs);
        if (callee_attrs & CC_FN_ATTR_ASYNC) continue;
        if (callee_attrs & CC_FN_ATTR_NOBLOCK) continue;

        /* Compute span offsets in the CURRENT input buffer using line/col. */
        if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
        if (n[i].line_end <= 0 || n[i].col_end <= 0) continue;
        size_t call_start = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        size_t call_end = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        if (call_start >= call_end || call_end > in_len) continue;

        /* Some TCC call spans report col_start at '(' rather than the callee identifier.
           Expand start leftwards to include a preceding identifier token. */
        {
            size_t s2 = call_start;
            while (s2 > 0 && (in_src[s2 - 1] == ' ' || in_src[s2 - 1] == '\t')) s2--;
            while (s2 > 0 && (isalnum((unsigned char)in_src[s2 - 1]) || in_src[s2 - 1] == '_')) s2--;
            if (s2 < call_start) call_start = s2;
        }

        /* Next non-ws token after call. */
        size_t j = call_end;
        while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t' || in_src[j] == '\n' || in_src[j] == '\r')) j++;
        int is_stmt_form = (j < in_len && in_src[j] == ';') ? 1 : 0;
        size_t stmt_end = is_stmt_form ? (j + 1) : call_end;

        /* Line + indent info */
        size_t lb = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
        size_t first = lb;
        while (first < in_len && (in_src[first] == ' ' || in_src[first] == '\t')) first++;
        size_t indent_start = lb;
        size_t indent_len = first > lb ? (first - lb) : 0;

        /* Find callee signature string (best-effort) from decl items in this TU. */
        const char* callee_sig = NULL;
        for (int k = 0; k < root->node_count; k++) {
            if (n[k].kind != 12) continue; /* DECL_ITEM */
            if (!n[k].aux_s1 || !n[k].aux_s2) continue;
            if (!cc__node_file_matches_this_tu(root, ctx, n[k].file)) continue;
            if (strcmp(n[k].aux_s1, n[i].aux_s1) != 0) continue;
            if (!strchr(n[k].aux_s2, '(')) continue;
            callee_sig = n[k].aux_s2;
            break;
        }
        if (!callee_sig) continue;

        /* Parse parameter types + return shape from signature "(...)" */
        int ret_is_ptr = 0, ret_is_void = 0, ret_is_structy = 0;
        {
            const char* l = strchr(callee_sig, '(');
            if (!l) continue;
            /* Prefix before '(' */
            size_t pre_n = (size_t)(l - callee_sig);
            if (pre_n > 255) pre_n = 255;
            char pre[256];
            memcpy(pre, callee_sig, pre_n);
            pre[pre_n] = 0;
            /* Trim */
            size_t a = 0;
            while (pre[a] == ' ' || pre[a] == '\t') a++;
            size_t b = strlen(pre);
            while (b > a && (pre[b - 1] == ' ' || pre[b - 1] == '\t')) b--;
            pre[b] = 0;
            const char* t = pre + a;
            if (strstr(t, "struct") || strstr(t, "CCSlice")) ret_is_structy = 1;
            if (strchr(t, '*')) ret_is_ptr = 1;
            /* best-effort void detect */
            if (!ret_is_ptr && !ret_is_structy) {
                const char* v = strstr(t, "void");
                if (v && (v[4] == 0 || v[4] == ' ' || v[4] == '\t')) {
                    /* ensure last token ends in void */
                    const char* endt = t + strlen(t);
                    while (endt > t && (endt[-1] == ' ' || endt[-1] == '\t')) endt--;
                    if (endt - t >= 4 && memcmp(endt - 4, "void", 4) == 0) ret_is_void = 1;
                }
            }
        }

        const char* ps = strchr(callee_sig, '(');
        const char* pe = strrchr(callee_sig, ')');
        if (!ps || !pe || pe <= ps) continue;
        ps++; /* inside parens */
        /* Trim outer spaces */
        while (ps < pe && (*ps == ' ' || *ps == '\t')) ps++;
        while (pe > ps && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;

        char* param_buf = NULL;
        size_t param_len = 0;
        {
            size_t ncp = (size_t)(pe - ps);
            param_buf = (char*)malloc(ncp + 1);
            if (!param_buf) continue;
            memcpy(param_buf, ps, ncp);
            param_buf[ncp] = 0;
            param_len = ncp;
        }

        /* Split parameter list on commas (no nested types supported yet). */
        char* param_types[16] = {0};
        int paramc = 0;
        if (param_len == 0 || (param_len == 4 && memcmp(param_buf, "void", 4) == 0)) {
            paramc = 0;
        } else {
            char* pcur = param_buf;
            while (*pcur && paramc < 16) {
                while (*pcur == ' ' || *pcur == '\t') pcur++;
                char* startp = pcur;
                while (*pcur && *pcur != ',') pcur++;
                char* endp = pcur;
                while (endp > startp && (endp[-1] == ' ' || endp[-1] == '\t')) endp--;
                size_t ln = (size_t)(endp - startp);
                if (ln > 0) {
                    char* ty = (char*)malloc(ln + 1);
                    if (!ty) break;
                    memcpy(ty, startp, ln);
                    ty[ln] = 0;
                    param_types[paramc++] = ty;
                }
                if (*pcur == ',') { pcur++; continue; }
                break;
            }
        }
        free(param_buf);

        /* Determine rewrite kind + statement start + validity checks for return/assign roots. */
        CCAutoBlockRewriteKind kind = CC_AB_REWRITE_STMT_CALL;
        const char* lhs_name = NULL;
        size_t stmt_start = lb;

        /* Check for nearest RETURN or ASSIGN ancestor. */
        int assign_idx = -1;
        int ret_idx = -1;
        for (int cur2 = n[i].parent; cur2 >= 0 && cur2 < root->node_count; cur2 = n[cur2].parent) {
            if (n[cur2].kind == 15) { ret_idx = cur2; break; } /* RETURN */
            if (n[cur2].kind == 14) { assign_idx = cur2; break; } /* ASSIGN */
        }

        if (ret_idx >= 0 && n[ret_idx].line_start == n[i].line_start && is_stmt_form) {
            /* return <call>; */
            size_t rs = lb;
            while (rs < in_len && (in_src[rs] == ' ' || in_src[rs] == '\t')) rs++;
            if (rs + 6 <= in_len && memcmp(in_src + rs, "return", 6) == 0) {
                size_t after = rs + 6;
                while (after < in_len && (in_src[after] == ' ' || in_src[after] == '\t')) after++;
                /* require call is the expression root */
                size_t after_call = call_end;
                while (after_call < in_len && (in_src[after_call] == ' ' || in_src[after_call] == '\t')) after_call++;
                if (after == call_start && after_call == j) {
                    if (!ret_is_void && !ret_is_structy) {
                        kind = CC_AB_REWRITE_RETURN_CALL;
                        stmt_start = lb;
                    }
                }
            }
            if (kind != CC_AB_REWRITE_RETURN_CALL) {
                /* Not a root `return <call>;` -> rewrite the whole return statement as
                   `tmp = await run_blocking(...); return ...tmp...;` (no braces; async_text can't handle blocks). */
                if (!ret_is_void && !ret_is_structy) {
                    /* Find statement end ';' at top-level from the start of the line. */
                    size_t endp = lb;
                    int par = 0, brk = 0, br = 0;
                    int ins = 0; char q = 0;
                    int in_lc = 0, in_bc = 0;
                    for (size_t k = lb; k < in_len; k++) {
                        char ch = in_src[k];
                        char ch2 = (k + 1 < in_len) ? in_src[k + 1] : 0;
                        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
                        if (ins) { if (ch == '\\' && k + 1 < in_len) { k++; continue; } if (ch == q) ins = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
                        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par) par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk) brk--; }
                        else if (ch == '{') br++;
                        else if (ch == '}') { if (br) br--; }
                        else if (ch == ';' && par == 0 && brk == 0 && br == 0) { endp = k + 1; break; }
                    }
                    if (endp > lb && endp <= in_len) {
                        kind = CC_AB_REWRITE_RETURN_EXPR_CALL;
                        stmt_start = lb;
                        stmt_end = endp;
                    }
                }
            }
        } else if (assign_idx >= 0 && n[assign_idx].line_start == n[i].line_start && is_stmt_form) {
            /* <lhs> = <call>; */
            const char* op = n[assign_idx].aux_s2;
            if (op && strcmp(op, "=") == 0 && n[assign_idx].aux_s1) {
                lhs_name = n[assign_idx].aux_s1;
                /* require statement starts with lhs_name */
                size_t lhs_len = strlen(lhs_name);
                if (lhs_len > 0 && first + lhs_len <= in_len && memcmp(in_src + first, lhs_name, lhs_len) == 0) {
                    size_t p = first + lhs_len;
                    while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
                    if (p < in_len && in_src[p] == '=') {
                        p++;
                        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
                        size_t after_call = call_end;
                        while (after_call < in_len && (in_src[after_call] == ' ' || in_src[after_call] == '\t')) after_call++;
                        if (p == call_start && after_call == j) {
                            if (!ret_is_void && !ret_is_structy) {
                                kind = CC_AB_REWRITE_ASSIGN_CALL;
                                stmt_start = lb;
                            }
                        }
                    }
                }
            }
            if (kind != CC_AB_REWRITE_ASSIGN_CALL) {
                if (!ret_is_void && !ret_is_structy) {
                    /* Find statement end ';' at top-level from the start of the line. */
                    size_t endp = lb;
                    int par = 0, brk = 0, br = 0;
                    int ins = 0; char q = 0;
                    int in_lc = 0, in_bc = 0;
                    for (size_t k = lb; k < in_len; k++) {
                        char ch = in_src[k];
                        char ch2 = (k + 1 < in_len) ? in_src[k + 1] : 0;
                        if (in_lc) { if (ch == '\n') in_lc = 0; continue; }
                        if (in_bc) { if (ch == '*' && ch2 == '/') { in_bc = 0; k++; } continue; }
                        if (ins) { if (ch == '\\' && k + 1 < in_len) { k++; continue; } if (ch == q) ins = 0; continue; }
                        if (ch == '/' && ch2 == '/') { in_lc = 1; k++; continue; }
                        if (ch == '/' && ch2 == '*') { in_bc = 1; k++; continue; }
                        if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par) par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk) brk--; }
                        else if (ch == '{') br++;
                        else if (ch == '}') { if (br) br--; }
                        else if (ch == ';' && par == 0 && brk == 0 && br == 0) { endp = k + 1; break; }
                    }
                    if (endp > lb && endp <= in_len) {
                        kind = CC_AB_REWRITE_ASSIGN_EXPR_CALL;
                        stmt_start = lb;
                        stmt_end = endp;
                    }
                }
            }
        } else {
            /* plain statement call: require call begins statement token */
            if (is_stmt_form) {
                int ok = 1;
                for (size_t k = first; k < call_start; k++) {
                    char ch = in_src[k];
                    if (ch == ' ' || ch == '\t') continue;
                    ok = 0;
                    break;
                }
                if (ok) {
                    kind = CC_AB_REWRITE_STMT_CALL;
                    stmt_start = lb;
                } else {
                    /* Don't try to rewrite general expression contexts yet (e.g. for-loop headers). */
                    for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
                    continue;
                }
            } else {
                /* Don't try to rewrite general expression contexts yet (e.g. for-loop headers). */
                for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
                continue;
            }
        }

        /* If we didn't select a kind (due to conservative checks), skip. */
        if (kind != CC_AB_REWRITE_STMT_CALL &&
            kind != CC_AB_REWRITE_RETURN_CALL &&
            kind != CC_AB_REWRITE_ASSIGN_CALL &&
            kind != CC_AB_REWRITE_RETURN_EXPR_CALL &&
            kind != CC_AB_REWRITE_ASSIGN_EXPR_CALL) {
            for (int pi = 0; pi < paramc; pi++) free(param_types[pi]);
            continue;
        }
        /* If return/assign checks failed, fall back only if it was a plain stmt-call (already handled above). */
        if (kind == CC_AB_REWRITE_RETURN_CALL || kind == CC_AB_REWRITE_ASSIGN_CALL || kind == CC_AB_REWRITE_STMT_CALL) {
            /* ok */
        }

        if (rep_n == rep_cap) {
            rep_cap = rep_cap ? rep_cap * 2 : 64;
            reps = (Replace*)realloc(reps, (size_t)rep_cap * sizeof(*reps));
            if (!reps) return 0;
        }
        reps[rep_n] = (Replace){
            .start = stmt_start,
            .end = stmt_end,
            .call_start = call_start,
            .call_end = call_end,
            .line_start = n[i].line_start,
            .callee = n[i].aux_s1,
            .lhs_name = lhs_name,
            .kind = kind,
            .argc = paramc,
            .ret_is_ptr = ret_is_ptr,
            .ret_is_void = ret_is_void,
            .ret_is_structy = ret_is_structy,
            .indent_start = indent_start,
            .indent_len = indent_len,
            .batch = NULL,
            .batch_n = 0,
            .tail_kind = 0,
            .tail_call_start = 0,
            .tail_call_end = 0,
            .tail_callee = NULL,
            .tail_lhs_name = NULL,
            .tail_argc = 0,
            .tail_ret_is_ptr = 0,
        };
        for (int pi = 0; pi < paramc; pi++) reps[rep_n].param_types[pi] = param_types[pi];
        for (int pi = paramc; pi < 16; pi++) reps[rep_n].param_types[pi] = NULL;
        for (int pi = 0; pi < 16; pi++) reps[rep_n].tail_param_types[pi] = NULL;
        rep_n++;
    }

    if (rep_n == 0) {
        free(reps);
        return 0;
    }

    /* Filter overlaps (keep outermost). */
    /* Sort ASC by start, tie-break by larger end first (outer spans first). */
    for (int i = 0; i < rep_n - 1; i++) {
        for (int k = i + 1; k < rep_n; k++) {
            if (reps[k].start < reps[i].start ||
                (reps[k].start == reps[i].start && reps[k].end > reps[i].end)) {
                Replace tmp = reps[i];
                reps[i] = reps[k];
                reps[k] = tmp;
            }
        }
    }

    int out_n = 0;
    for (int i = 0; i < rep_n; i++) {
        int overlap = 0;
        for (int j2 = 0; j2 < out_n; j2++) {
            if (!(reps[i].end <= reps[j2].start || reps[i].start >= reps[j2].end)) {
                overlap = 1;
                break;
            }
        }
        if (!overlap) reps[out_n++] = reps[i];
    }
    rep_n = out_n;

    /* Batch consecutive statement-form sync calls in @async (coalescing/batching).
       Only batches CC_AB_REWRITE_STMT_CALL nodes, and only when separated by whitespace/comments. */
    {
        /* Sort ASC for grouping */
        for (int i = 0; i < rep_n - 1; i++) {
            for (int k = i + 1; k < rep_n; k++) {
                if (reps[k].start < reps[i].start) {
                    Replace tmp = reps[i];
                    reps[i] = reps[k];
                    reps[k] = tmp;
                }
            }
        }

        Replace* out = (Replace*)calloc((size_t)rep_n, sizeof(Replace));
        if (out) {
            int on = 0;
            for (int i = 0; i < rep_n; ) {
                if (reps[i].kind != CC_AB_REWRITE_STMT_CALL) {
                    out[on++] = reps[i++];
                    continue;
                }
                int j = i + 1;
                while (j < rep_n &&
                       reps[j].kind == CC_AB_REWRITE_STMT_CALL &&
                       cc__ab_only_ws_comments(in_src, reps[j - 1].end, reps[j].start)) {
                    j++;
                }
                int group_n = j - i;
                int tail_idx = -1;
                if (j < rep_n &&
                    (reps[j].kind == CC_AB_REWRITE_RETURN_CALL || reps[j].kind == CC_AB_REWRITE_ASSIGN_CALL) &&
                    cc__ab_only_ws_comments(in_src, reps[j - 1].end, reps[j].start)) {
                    tail_idx = j;
                    j++;
                }
                if (group_n <= 1 && tail_idx < 0) {
                    out[on++] = reps[i++];
                    continue;
                }

                Replace r = reps[i];
                r.kind = (tail_idx >= 0 && reps[tail_idx].kind == CC_AB_REWRITE_RETURN_CALL)
                             ? CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN
                             : (tail_idx >= 0 && reps[tail_idx].kind == CC_AB_REWRITE_ASSIGN_CALL)
                                   ? CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN
                                   : CC_AB_REWRITE_BATCH_STMT_CALLS;
                r.start = reps[i].start;
                r.end = reps[(tail_idx >= 0) ? tail_idx : (j - 1)].end;
                r.call_start = reps[i].call_start;
                r.call_end = reps[(tail_idx >= 0) ? tail_idx : (j - 1)].call_end;
                /* Batch carries per-item metadata; do not keep per-rep param_types to avoid double-free. */
                r.argc = 0;
                for (int pi = 0; pi < 16; pi++) r.param_types[pi] = NULL;
                r.batch_n = group_n;
                r.batch = (CCAutoBlockBatchItem*)calloc((size_t)group_n, sizeof(CCAutoBlockBatchItem));
                if (!r.batch) {
                    out[on++] = reps[i++];
                    continue;
                }
                for (int bi = 0; bi < group_n; bi++) {
                    Replace* src = &reps[i + bi];
                    CCAutoBlockBatchItem* it = &r.batch[bi];
                    it->call_start = src->call_start;
                    it->call_end = src->call_end;
                    it->line_start = src->line_start;
                    it->callee = src->callee;
                    it->argc = src->argc;
                    for (int pi = 0; pi < src->argc; pi++) {
                        it->param_types[pi] = src->param_types[pi];
                        src->param_types[pi] = NULL; /* transfer ownership */
                    }
                }

                if (tail_idx >= 0) {
                    Replace* tail = &reps[tail_idx];
                    r.tail_kind = (tail->kind == CC_AB_REWRITE_RETURN_CALL) ? 1 : 2;
                    r.tail_call_start = tail->call_start;
                    r.tail_call_end = tail->call_end;
                    r.tail_callee = tail->callee;
                    r.tail_lhs_name = tail->lhs_name;
                    r.tail_argc = tail->argc;
                    r.tail_ret_is_ptr = tail->ret_is_ptr;
                    for (int pi = 0; pi < tail->argc; pi++) {
                        r.tail_param_types[pi] = tail->param_types[pi];
                        tail->param_types[pi] = NULL; /* transfer ownership */
                    }
                }
                out[on++] = r;
                i = j;
            }
            free(reps);
            reps = out;
            rep_n = on;
        } else {
            /* if we couldn't allocate, keep unbatched and leave reps as-is */
        }

        /* Sort DESC again for splicing */
        for (int i = 0; i < rep_n - 1; i++) {
            for (int k = i + 1; k < rep_n; k++) {
                if (reps[k].start > reps[i].start) {
                    Replace tmp = reps[i];
                    reps[i] = reps[k];
                    reps[k] = tmp;
                }
            }
        }
    }

    char* cur_src = (char*)malloc(in_len + 1);
    if (!cur_src) { free(reps); return 0; }
    memcpy(cur_src, in_src, in_len);
    cur_src[in_len] = 0;
    size_t cur_len = in_len;

    for (int ri = 0; ri < rep_n; ri++) {
        size_t s = reps[ri].start;
        size_t e = reps[ri].end;
        if (s >= e || e > cur_len) continue;

        /* Extract original statement text */
        size_t stmt_len = e - s;
        char* stmt_txt = (char*)malloc(stmt_len + 1);
        if (!stmt_txt) continue;
        memcpy(stmt_txt, cur_src + s, stmt_len);
        stmt_txt[stmt_len] = 0;

        /* Batched statement calls: collapse adjacent sync calls into one blocking dispatch. */
        if ((reps[ri].kind == CC_AB_REWRITE_BATCH_STMT_CALLS ||
             reps[ri].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN ||
             reps[ri].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN) &&
            reps[ri].batch && reps[ri].batch_n > 0) {
            char* ind = NULL;
            if (reps[ri].indent_len > 0 && reps[ri].indent_start + reps[ri].indent_len <= cur_len) {
                ind = (char*)malloc(reps[ri].indent_len + 1);
                if (ind) {
                    memcpy(ind, cur_src + reps[ri].indent_start, reps[ri].indent_len);
                    ind[reps[ri].indent_len] = 0;
                }
            }
            const char* I = ind ? ind : "";

            char* repl = NULL;
            size_t repl_len = 0;
            size_t repl_cap = 0;
            /* No extra block wrapper: we use per-line unique temp names to avoid collisions. */

            /* Bind all args in order (in the async context), then do one run_blocking dispatch. */
            for (int bi = 0; bi < reps[ri].batch_n; bi++) {
                const CCAutoBlockBatchItem* it = &reps[ri].batch[bi];
                size_t call_s = it->call_start - s;
                size_t call_e = it->call_end - s;
                if (call_e > stmt_len || call_s >= call_e) continue;
                size_t call_len = call_e - call_s;
                char* call_txt = (char*)malloc(call_len + 1);
                if (!call_txt) continue;
                memcpy(call_txt, stmt_txt + call_s, call_len);
                call_txt[call_len] = 0;

                const char* lpar = strchr(call_txt, '(');
                const char* rpar = strrchr(call_txt, ')');
                if (!lpar || !rpar || rpar <= lpar) { free(call_txt); continue; }
                size_t args_s = (size_t)(lpar - call_txt) + 1;
                size_t args_e = (size_t)(rpar - call_txt);

                size_t arg_starts[16];
                size_t arg_ends[16];
                int argc = 0;
                int par = 0, brk = 0, br = 0;
                int ins = 0; char q = 0;
                size_t cur_a = args_s;
                for (size_t k = args_s; k < args_e; k++) {
                    char ch = call_txt[k];
                    if (ins) {
                        if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                        if (ch == q) ins = 0;
                        continue;
                    }
                    if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                    if (ch == '(') par++;
                    else if (ch == ')') { if (par) par--; }
                    else if (ch == '[') brk++;
                    else if (ch == ']') { if (brk) brk--; }
                    else if (ch == '{') br++;
                    else if (ch == '}') { if (br) br--; }
                    else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                        if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                            arg_starts[argc] = cur_a;
                            arg_ends[argc] = k;
                            argc++;
                        }
                        cur_a = k + 1;
                    }
                }
                if (args_s == args_e) argc = 0;
                else if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                    arg_starts[argc] = cur_a;
                    arg_ends[argc] = args_e;
                    argc++;
                }

                for (int ai = 0; ai < argc; ai++) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCAbIntptr __cc_ab_l%d_b%d_a%d = (CCAbIntptr)(%.*s);\n",
                                   I, reps[ri].line_start, bi, ai,
                                   (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
                }

                free(call_txt);
            }

            /* If we have a trailing return/assign, bind its args too. */
            if (reps[ri].tail_kind != 0 && reps[ri].tail_call_end > reps[ri].tail_call_start) {
                size_t call_s = reps[ri].tail_call_start - s;
                size_t call_e = reps[ri].tail_call_end - s;
                if (call_e <= stmt_len && call_s < call_e) {
                    size_t call_len = call_e - call_s;
                    char* call_txt = (char*)malloc(call_len + 1);
                    if (call_txt) {
                        memcpy(call_txt, stmt_txt + call_s, call_len);
                        call_txt[call_len] = 0;
                        const char* lpar = strchr(call_txt, '(');
                        const char* rpar = strrchr(call_txt, ')');
                        if (lpar && rpar && rpar > lpar) {
                            size_t args_s = (size_t)(lpar - call_txt) + 1;
                            size_t args_e = (size_t)(rpar - call_txt);
                            size_t arg_starts[16];
                            size_t arg_ends[16];
                            int argc = 0;
                            int par = 0, brk = 0, br = 0;
                            int ins = 0; char q = 0;
                            size_t cur_a = args_s;
                            for (size_t k = args_s; k < args_e; k++) {
                                char ch = call_txt[k];
                                if (ins) {
                                    if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                                    if (ch == q) ins = 0;
                                    continue;
                                }
                                if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
                                if (ch == '(') par++;
                                else if (ch == ')') { if (par) par--; }
                                else if (ch == '[') brk++;
                                else if (ch == ']') { if (brk) brk--; }
                                else if (ch == '{') br++;
                                else if (ch == '}') { if (br) br--; }
                                else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                                    if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                                        arg_starts[argc] = cur_a;
                                        arg_ends[argc] = k;
                                        argc++;
                                    }
                                    cur_a = k + 1;
                                }
                            }
                            if (args_s == args_e) argc = 0;
                            else if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                                arg_starts[argc] = cur_a;
                                arg_ends[argc] = args_e;
                                argc++;
                            }
                            for (int ai = 0; ai < argc; ai++) {
                                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCAbIntptr __cc_ab_l%d_t_a%d = (CCAbIntptr)(%.*s);\n",
                                               I, reps[ri].line_start, ai,
                                               (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
                            }
                        }
                        free(call_txt);
                    }
                }
            }

            /* Task-based auto-blocking: create a CCClosure0 value first, then await the task.
               This avoids embedding multiline `() => { ... }` directly inside `await <expr>` which
               interacts badly with later closure-elision + #line resync. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => {\n", I, reps[ri].line_start);
            for (int bi = 0; bi < reps[ri].batch_n; bi++) {
                const CCAutoBlockBatchItem* it = &reps[ri].batch[bi];
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    %s(", I, it->callee ? it->callee : "");
                for (int ai = 0; ai < it->argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (it->param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_b%d_a%d", it->param_types[ai], reps[ri].line_start, bi, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_b%d_a%d", reps[ri].line_start, bi, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ");\n");
            }
            if (reps[ri].tail_kind != 0) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    return ", I);
                if (!reps[ri].tail_ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].tail_callee ? reps[ri].tail_callee : "");
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < reps[ri].tail_argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (reps[ri].tail_param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_t_a%d", reps[ri].tail_param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_t_a%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, ");\n");
            } else {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s    return NULL;\n", I);
            }
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  };\n", I);
            if (reps[ri].tail_kind == 0) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else if (reps[ri].tail_kind == 1) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  return await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                               I,
                               reps[ri].tail_lhs_name ? reps[ri].tail_lhs_name : "__cc_ab_lhs",
                               reps[ri].line_start);
            }

            free(stmt_txt);
            free(ind);
            if (!repl) continue;

            /* Splice */
            size_t new_len = cur_len - (e - s) + repl_len;
            char* next = (char*)malloc(new_len + 1);
            if (!next) { free(repl); continue; }
            memcpy(next, cur_src, s);
            memcpy(next + s, repl, repl_len);
            memcpy(next + s + repl_len, cur_src + e, cur_len - e);
            next[new_len] = 0;
            free(repl);
            free(cur_src);
            cur_src = next;
            cur_len = new_len;
            continue;
        }

        /* Extract original call text (from statement slice) for arg parsing */
        size_t call_s = reps[ri].call_start - s;
        size_t call_e = reps[ri].call_end - s;
        if (call_e > stmt_len || call_s >= call_e) { free(stmt_txt); continue; }
        size_t call_len = call_e - call_s;
        char* call_txt = (char*)malloc(call_len + 1);
        if (!call_txt) continue;
        memcpy(call_txt, stmt_txt + call_s, call_len);
        call_txt[call_len] = 0;

        /* Build replacement (indent-aware). */
        char* repl = NULL;
        size_t repl_len = 0;
        size_t repl_cap = 0;

        /* Find args inside call_txt. */
        const char* lpar = strchr(call_txt, '(');
        const char* rpar = strrchr(call_txt, ')');
        if (!lpar || !rpar || rpar <= lpar) {
            free(call_txt);
            free(stmt_txt);
            free(repl);
            continue;
        }
        size_t args_s = (size_t)(lpar - call_txt) + 1;
        size_t args_e = (size_t)(rpar - call_txt);

        /* Split args on top-level commas. */
        size_t arg_starts[16];
        size_t arg_ends[16];
        int argc = 0;
        int par = 0, brk = 0, br = 0;
        int ins = 0; char q = 0;
        size_t cur_a = args_s;
        for (size_t k = args_s; k < args_e; k++) {
            char ch = call_txt[k];
            if (ins) {
                if (ch == '\\' && k + 1 < args_e) { k++; continue; }
                if (ch == q) ins = 0;
                continue;
            }
            if (ch == '"' || ch == '\'') { ins = 1; q = ch; continue; }
            if (ch == '(') par++;
            else if (ch == ')') { if (par) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk) brk--; }
            else if (ch == '{') br++;
            else if (ch == '}') { if (br) br--; }
            else if (ch == ',' && par == 0 && brk == 0 && br == 0) {
                if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
                    arg_starts[argc] = cur_a;
                    arg_ends[argc] = k;
                    argc++;
                }
                cur_a = k + 1;
            }
        }
        /* Last arg (or empty) */
        if (argc < (int)(sizeof(arg_starts)/sizeof(arg_starts[0]))) {
            arg_starts[argc] = cur_a;
            arg_ends[argc] = args_e;
            argc++;
        }
        /* Handle empty-arg call. */
        if (args_s == args_e) argc = 0;

        /* Indent string from original line. */
        char* ind = NULL;
        if (reps[ri].indent_len > 0 && reps[ri].indent_start + reps[ri].indent_len <= cur_len) {
            ind = (char*)malloc(reps[ri].indent_len + 1);
            if (ind) {
                memcpy(ind, cur_src + reps[ri].indent_start, reps[ri].indent_len);
                ind[reps[ri].indent_len] = 0;
            }
        }
        const char* I = ind ? ind : "";

        for (int ai = 0; ai < argc; ai++) {
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCAbIntptr __cc_ab_l%d_arg%d = (CCAbIntptr)(%.*s);\n",
                           I,
                           reps[ri].line_start,
                           ai,
                           (int)(arg_ends[ai] - arg_starts[ai]), call_txt + arg_starts[ai]);
        }
        if (reps[ri].kind == CC_AB_REWRITE_RETURN_EXPR_CALL || reps[ri].kind == CC_AB_REWRITE_ASSIGN_EXPR_CALL) {
            char tmp_name[96];
            snprintf(tmp_name, sizeof(tmp_name), "__cc_ab_expr_l%d", reps[ri].line_start);

            /* Emit a CCClosure0 value first (avoid embedding closure literal directly in `await <expr>`),
               then await the task into an intptr temp, then emit the original statement with the call
               replaced by that temp. */
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sCCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
            if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
            else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%sintptr_t %s = 0;\n", I, tmp_name);
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s%s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                           I, tmp_name, reps[ri].line_start);

            /* Original statement with call replaced by tmp. */
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt, call_s);
            cc__append_str(&repl, &repl_len, &repl_cap, tmp_name);
            cc__append_n(&repl, &repl_len, &repl_cap, stmt_txt + call_e, stmt_len - call_e);
            if (repl_len > 0 && repl[repl_len - 1] != '\n') cc__append_str(&repl, &repl_len, &repl_cap, "\n");
        } else if (reps[ri].kind == CC_AB_REWRITE_STMT_CALL) {
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { ", I, reps[ri].line_start);
            cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
            cc__append_str(&repl, &repl_len, &repl_cap, "(");
            for (int ai = 0; ai < argc; ai++) {
                if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                } else {
                    cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                }
            }
            cc__append_str(&repl, &repl_len, &repl_cap, "); return NULL; };\n");
            cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
        } else {
            if (reps[ri].kind == CC_AB_REWRITE_RETURN_CALL) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
                if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  return await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n", I, reps[ri].line_start);
            } else if (reps[ri].kind == CC_AB_REWRITE_ASSIGN_CALL && reps[ri].lhs_name) {
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  CCClosure0 __cc_ab_c_l%d = () => { return ", I, reps[ri].line_start);
                if (!reps[ri].ret_is_ptr) cc__append_str(&repl, &repl_len, &repl_cap, "(void*)(intptr_t)");
                else cc__append_str(&repl, &repl_len, &repl_cap, "(void*)");
                cc__append_str(&repl, &repl_len, &repl_cap, reps[ri].callee);
                cc__append_str(&repl, &repl_len, &repl_cap, "(");
                for (int ai = 0; ai < argc; ai++) {
                    if (ai) cc__append_str(&repl, &repl_len, &repl_cap, ", ");
                    if (ai < reps[ri].argc && reps[ri].param_types[ai]) {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "(%s)__cc_ab_l%d_arg%d", reps[ri].param_types[ai], reps[ri].line_start, ai);
                    } else {
                        cc__append_fmt(&repl, &repl_len, &repl_cap, "__cc_ab_l%d_arg%d", reps[ri].line_start, ai);
                    }
                }
                cc__append_str(&repl, &repl_len, &repl_cap, "); };\n");
                cc__append_fmt(&repl, &repl_len, &repl_cap, "%s  %s = await cc_run_blocking_task_intptr(__cc_ab_c_l%d);\n",
                               I, reps[ri].lhs_name, reps[ri].line_start);
            }
        }

        free(call_txt);
        free(stmt_txt);
        free(ind);
        if (!repl) continue;

        /* Splice */
        size_t new_len = cur_len - (e - s) + repl_len;
        char* next = (char*)malloc(new_len + 1);
        if (!next) { free(repl); continue; }
        memcpy(next, cur_src, s);
        memcpy(next + s, repl, repl_len);
        memcpy(next + s + repl_len, cur_src + e, cur_len - e);
        next[new_len] = 0;
        free(repl);
        free(cur_src);
        cur_src = next;
        cur_len = new_len;
    }

    for (int i = 0; i < rep_n; i++) {
        for (int j = 0; j < reps[i].argc; j++) free(reps[i].param_types[j]);
        if ((reps[i].kind == CC_AB_REWRITE_BATCH_STMT_CALLS ||
             reps[i].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_RETURN ||
             reps[i].kind == CC_AB_REWRITE_BATCH_STMTS_THEN_ASSIGN) &&
            reps[i].batch) {
            for (int bi = 0; bi < reps[i].batch_n; bi++) {
                for (int pj = 0; pj < reps[i].batch[bi].argc; pj++) {
                    free(reps[i].batch[bi].param_types[pj]);
                }
            }
            free(reps[i].batch);
            reps[i].batch = NULL;
        }
        if (reps[i].tail_kind != 0) {
            for (int pj = 0; pj < reps[i].tail_argc; pj++) free(reps[i].tail_param_types[pj]);
        }
    }
    free(reps);
    *out_src = cur_src;
    *out_len = cur_len;
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
                                  int line_base,
                                  int* io_next_closure_id,
                                  CCClosureDesc** out_descs,
                                  int* out_count,
                                  int** out_line_map,
                                  int* out_line_cap,
                                  char** out_protos,
                                  size_t* out_protos_len,
                                  char** out_defs,
                                  size_t* out_defs_len) {
    if (!src || !out_descs || !out_count || !out_line_map || !out_line_cap || !out_defs || !out_defs_len) return 0;
    *out_descs = NULL;
    *out_count = 0;
    *out_line_map = NULL;
    *out_line_cap = 0;
    if (out_protos) *out_protos = NULL;
    if (out_protos_len) *out_protos_len = 0;
    *out_defs = NULL;
    *out_defs_len = 0;

    int lines = 1;
    for (size_t i = 0; i < src_len; i++) if (src[i] == '\n') lines++;
    int* line_map = (int*)calloc((size_t)lines + 2, sizeof(int));
    if (!line_map) return 0;

    CCClosureDesc* descs = NULL;
    int count = 0, cap = 0;
    char* protos = NULL;
    size_t protos_len = 0, protos_cap = 0;
    char* defs = NULL;
    size_t defs_len = 0, defs_cap = 0;

    char** scope_names[256];
    char** scope_types[256];
    unsigned char* scope_flags[256];
    int scope_counts[256];
    for (int i = 0; i < 256; i++) { scope_names[i] = NULL; scope_types[i] = NULL; scope_flags[i] = NULL; scope_counts[i] = 0; }
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
        cc__maybe_record_decl(scope_names, scope_types, scope_flags, scope_counts, depth, tmp_line);

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

        /* closure literal:
           - `() => { ... }` / `() => expr`
           - `(x) => { ... }` / `(x) => expr`
           - `x => { ... }` / `x => expr`
           (best-effort scan) */
        {
            int consumed_literal = 0;
            const char* s = line_start;
            int in_str = 0;
            char str_q = 0;
            while (s < line_end) {
                char c = *s;
                if (in_str) {
                    if (c == '\\' && (s + 1) < line_end) { s += 2; continue; }
                    if (c == str_q) in_str = 0;
                    s++;
                    continue;
                }
                if (c == '"' || c == '\'') { in_str = 1; str_q = c; s++; continue; }
                int param_count = 0;
                char param0[128] = {0};
                char param1[128] = {0};
                char param0_type[128] = {0};
                char param1_type[128] = {0};
                const char* p = NULL;

                if (c == '(') {
                    /* Parse `( ... ) =>` where `...` is empty, `x`, `x,y`, `int x`, `int x, int y`, etc. */
                    const char* rp = s + 1;
                    while (rp < line_end && *rp != ')') rp++;
                    if (rp >= line_end || *rp != ')') { s++; continue; }

                    const char* after_rp = rp + 1;
                    while (after_rp < line_end && (*after_rp == ' ' || *after_rp == '\t')) after_rp++;
                    if ((after_rp + 2) > line_end || after_rp[0] != '=' || after_rp[1] != '>') { s++; continue; }
                    p = after_rp + 2;

                    /* Parse params substring [s+1, rp). */
                    const char* ps = s + 1;
                    const char* pe = rp;
                    while (ps < pe && (*ps == ' ' || *ps == '\t')) ps++;
                    while (pe > ps && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;

                    param_count = 0;
                    if (ps < pe) {
                        /* Split by top-level commas (no nesting expected in early param list). */
                        const char* seg_s = ps;
                        int seg_idx = 0;
                        for (const char* z = ps; z <= pe; z++) {
                            int at_end = (z == pe);
                            if (!at_end && *z != ',') continue;
                            const char* seg_e = z;
                            while (seg_s < seg_e && (*seg_s == ' ' || *seg_s == '\t')) seg_s++;
                            while (seg_e > seg_s && (seg_e[-1] == ' ' || seg_e[-1] == '\t')) seg_e--;
                            if (seg_e <= seg_s) { seg_s = z + 1; continue; }

                            /* Find last identifier in segment: it's the param name; prefix is type (optional). */
                            const char* nm_e = seg_e;
                            while (nm_e > seg_s && !cc__is_ident_char2(nm_e[-1])) nm_e--;
                            const char* nm_s = nm_e;
                            while (nm_s > seg_s && cc__is_ident_char2(nm_s[-1])) nm_s--;
                            if (nm_s >= nm_e || !cc__is_ident_start_char(*nm_s)) { break; }

                            size_t nm_n = (size_t)(nm_e - nm_s);
                            if (nm_n >= sizeof(param0)) { break; }

                            const char* ty_s = seg_s;
                            const char* ty_e = nm_s;
                            while (ty_e > ty_s && (ty_e[-1] == ' ' || ty_e[-1] == '\t')) ty_e--;

                            if (seg_idx == 0) {
                                memcpy(param0, nm_s, nm_n); param0[nm_n] = '\0';
                                if (ty_e > ty_s) {
                                    size_t tn = (size_t)(ty_e - ty_s);
                                    if (tn >= sizeof(param0_type)) tn = sizeof(param0_type) - 1;
                                    memcpy(param0_type, ty_s, tn); param0_type[tn] = '\0';
                                } else {
                                    param0_type[0] = '\0';
                                }
                                param_count = 1;
                            } else if (seg_idx == 1) {
                                memcpy(param1, nm_s, nm_n); param1[nm_n] = '\0';
                                if (ty_e > ty_s) {
                                    size_t tn = (size_t)(ty_e - ty_s);
                                    if (tn >= sizeof(param1_type)) tn = sizeof(param1_type) - 1;
                                    memcpy(param1_type, ty_s, tn); param1_type[tn] = '\0';
                                } else {
                                    param1_type[0] = '\0';
                                }
                                param_count = 2;
                            } else {
                                break;
                            }

                            seg_idx++;
                            seg_s = z + 1;
                        }
                        if (param_count == 0) { s++; continue; }
                    }
                } else if (cc__is_ident_start_char(c)) {
                    /* x => ... */
                    const char* n0 = s;
                    const char* q = s + 1;
                    while (q < line_end && cc__is_ident_char2(*q)) q++;
                    size_t nn = (size_t)(q - n0);
                    if (nn == 0 || nn >= sizeof(param0) || cc__is_keyword_tok(n0, nn)) { s++; continue; }
                    const char* r = q;
                    while (r < line_end && (*r == ' ' || *r == '\t')) r++;
                    if ((r + 2) <= line_end && r[0] == '=' && r[1] == '>') {
                        memcpy(param0, n0, nn);
                        param0[nn] = '\0';
                        param_count = 1;
                        param0_type[0] = '\0';
                        p = r + 2;
                    } else {
                        s++; continue;
                    }
                } else {
                    s++; continue;
                }

                while (p < line_end && (*p == ' ' || *p == '\t')) p++;
                if (p >= line_end) { s++; continue; }

                const char* body_start = p;
                const char* body_end = NULL; /* exclusive */
                int end_line = line_no;
                int end_col = 0;

                if (*body_start == '{') {
                    const char* b = body_start;
                    int br = 0;
                    int in_str2 = 0;
                    char q2 = 0;
                    while ((size_t)(b - src) < src_len) {
                        char ch = *b++;
                        if (in_str2) {
                            if (ch == '\\' && (size_t)(b - src) < src_len) { b++; continue; }
                            if (ch == q2) in_str2 = 0;
                            continue;
                        }
                        if (ch == '"' || ch == '\'') { in_str2 = 1; q2 = ch; continue; }
                        if (ch == '{') br++;
                        else if (ch == '}') { br--; if (br == 0) break; }
                    }
                    if (br != 0) { s++; continue; }
                    body_end = b;
                } else {
                    /* expression body: scan until delimiter at nesting depth 0 */
                    const char* b = body_start;
                    int par = 0, brk = 0;
                    int in_str2 = 0;
                    char q2 = 0;
                    while ((size_t)(b - src) < src_len) {
                        char ch = *b;
                        if (in_str2) {
                            if (ch == '\\' && (size_t)((b + 1) - src) < src_len) { b += 2; continue; }
                            if (ch == q2) in_str2 = 0;
                            b++;
                            continue;
                        }
                        if (ch == '"' || ch == '\'') { in_str2 = 1; q2 = ch; b++; continue; }
                        if (ch == '(') par++;
                        else if (ch == ')') { if (par == 0 && brk == 0) break; par--; }
                        else if (ch == '[') brk++;
                        else if (ch == ']') { if (brk == 0 && par == 0) break; brk--; }
                        if (par == 0 && brk == 0) {
                            if (ch == ',' || ch == ';' || ch == '}' || ch == '\n') break;
                        }
                        b++;
                    }
                    body_end = b;
                }

                /* Compute end_line/end_col based on body_end */
                for (const char* x = body_start; x < body_end; x++) if (*x == '\n') end_line++;
                const char* last_nl = NULL;
                for (const char* x = body_start; x < body_end; x++) if (*x == '\n') last_nl = x;
                if (last_nl) end_col = (int)(body_end - (last_nl + 1));
                else end_col = (int)(body_end - line_start);

                int nid = (nursery_top >= 0) ? nursery_stack[nursery_top] : 0;

                char* body = (char*)malloc((size_t)(body_end - body_start) + 1);
                if (!body) { free(line_map); return 0; }
                memcpy(body, body_start, (size_t)(body_end - body_start));
                body[(size_t)(body_end - body_start)] = '\0';

                                char** caps = NULL;
                                int cap_n = 0;
                                cc__collect_caps_from_block(scope_names, scope_counts, depth, body,
                                                            (param_count >= 1 ? param0 : NULL),
                                                            (param_count >= 2 ? param1 : NULL),
                                                            &caps, &cap_n);
                                char** cap_types = NULL;
                                unsigned char* cap_flags = NULL;
                                if (cap_n > 0) {
                                    cap_types = (char**)calloc((size_t)cap_n, sizeof(char*));
                                    cap_flags = (unsigned char*)calloc((size_t)cap_n, sizeof(unsigned char));
                                    if (!cap_types || !cap_flags) { free(cap_types); free(cap_flags); free(body); free(line_map); return 0; }
                                    for (int ci = 0; ci < cap_n; ci++) {
                                        const char* ty = NULL;
                                        unsigned char fl = 0;
                                        for (int d = depth; d >= 1 && !ty; d--) {
                                            ty = cc__lookup_decl_type(scope_names[d], scope_types[d], scope_counts[d], caps[ci]);
                                            if (ty) fl = cc__lookup_decl_flags(scope_names[d], scope_flags[d], scope_counts[d], caps[ci]);
                                        }
                                        if (ty) {
                                            cap_types[ci] = strdup(ty);
                                        }
                                        cap_flags[ci] = fl;
                                    }
                                }

                if (count == cap) {
                    cap = cap ? cap * 2 : 16;
                    CCClosureDesc* nd = (CCClosureDesc*)realloc(descs, (size_t)cap * sizeof(CCClosureDesc));
                    if (!nd) { free(body); free(line_map); return 0; }
                    descs = nd;
                }
                int id = io_next_closure_id ? (*io_next_closure_id)++ : (count + 1);
                int abs_line = (line_base > 0 ? (line_base + line_no - 1) : line_no);
                int start_col = (int)(s - line_start);
                                descs[count++] = (CCClosureDesc){
                    .start_line = line_no,
                    .end_line = end_line,
                    .nursery_id = nid,
                    .id = id,
                    .start_col = start_col,
                    .end_col = end_col,
                    .param_count = param_count,
                    .param0_name = (param_count >= 1) ? strdup(param0) : NULL,
                    .param1_name = (param_count >= 2) ? strdup(param1) : NULL,
                    .param0_type = (param_count >= 1 && param0_type[0]) ? strdup(param0_type) : NULL,
                    .param1_type = (param_count >= 2 && param1_type[0]) ? strdup(param1_type) : NULL,
                    .cap_names = caps,
                                    .cap_types = cap_types,
                                    .cap_flags = cap_flags,
                    .cap_count = cap_n,
                    .body = body,
                };
                if (line_no <= lines) line_map[line_no] = count; /* 1-based index */

                {
                    char pb[128];
                    if (param_count == 0)
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*);\n", id);
                    else if (param_count == 1)
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*, intptr_t);\n", id);
                    else
                        snprintf(pb, sizeof(pb), "static void* __cc_closure_entry_%d(void*, intptr_t, intptr_t);\n", id);
                    cc__append_str(&protos, &protos_len, &protos_cap, pb);
                }
                {
                    /* Factory that captures by value into a heap env and returns a CCClosure0. */
                    cc__append_fmt(&protos, &protos_len, &protos_cap, "static %s __cc_closure_make_%d(",
                                   (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                   id);
                    if (cap_n == 0) {
                        cc__append_str(&protos, &protos_len, &protos_cap, "void");
                    } else {
                for (int ci = 0; ci < cap_n; ci++) {
                            if (ci) cc__append_str(&protos, &protos_len, &protos_cap, ", ");
                            cc__append_fmt(&protos, &protos_len, &protos_cap,
                                           "%s %s",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                    }
                    cc__append_str(&protos, &protos_len, &protos_cap, ");\n");
                }

                /* Always emit a closure definition. If there are captures, emit an env + make() helper. */
                {
                    char* more_protos = NULL;
                    size_t more_protos_len = 0;
                    char* more_defs = NULL;
                    size_t more_defs_len = 0;
                    char* lowered = NULL;
                    /* Only lower nested CC constructs inside block bodies for now.
                       (Expression bodies may need a separate lowering path that doesn't inject directives.) */
                    if (body && body[0] == '{') {
                        lowered = cc__lower_cc_in_block_text(body, strlen(body),
                                                             src_path, abs_line,
                                                             io_next_closure_id,
                                                             &more_protos, &more_protos_len,
                                                             &more_defs, &more_defs_len);
                    }
                    if (more_protos && more_protos_len) cc__append_str(&protos, &protos_len, &protos_cap, more_protos);
                    if (more_defs && more_defs_len) cc__append_str(&defs, &defs_len, &defs_cap, more_defs);
                    free(more_protos);
                    free(more_defs);

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "/* CC closure %d (from %s:%d) */\n",
                                   id, src_path ? src_path : "<src>", abs_line);

                    if (cap_n > 0) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "typedef struct __cc_closure_env_%d {\n", id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  %s %s;\n",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                        cc__append_str(&defs, &defs_len, &defs_cap, "} ");
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "__cc_closure_env_%d;\n", id);
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "static void __cc_closure_env_%d_drop(void* p) { if (p) free(p); }\n",
                                       id);
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "static %s __cc_closure_make_%d(",
                                       (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                       id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            if (ci) cc__append_str(&defs, &defs_len, &defs_cap, ", ");
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "%s %s",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap");
                        }
                        cc__append_str(&defs, &defs_len, &defs_cap, ") {\n");
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)malloc(sizeof(__cc_closure_env_%d));\n",
                                       id, id, id);
                        cc__append_str(&defs, &defs_len, &defs_cap, "  if (!__env) abort();\n");
                        for (int ci = 0; ci < cap_n; ci++) {
                            int mo = (cap_flags && (cap_flags[ci] & 2) != 0);
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  __env->%s = %s%s%s;\n",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? "cc_move(" : "",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? ")" : "");
                        }
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  return %s(__cc_closure_entry_%d, __env, __cc_closure_env_%d_drop);\n",
                                       (param_count == 0 ? "cc_closure0_make" : (param_count == 1 ? "cc_closure1_make" : "cc_closure2_make")),
                                       id, id);
                        cc__append_str(&defs, &defs_len, &defs_cap, "}\n");
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "static %s __cc_closure_make_%d(void) { return %s(__cc_closure_entry_%d, NULL, NULL); }\n",
                                       (param_count == 0 ? "CCClosure0" : (param_count == 1 ? "CCClosure1" : "CCClosure2")),
                                       id,
                                       (param_count == 0 ? "cc_closure0_make" : (param_count == 1 ? "cc_closure1_make" : "cc_closure2_make")),
                                       id);
                    }

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "static void* __cc_closure_entry_%d(%s) {\n",
                                   id,
                                   (param_count == 0 ? "void* __p" : (param_count == 1 ? "void* __p, intptr_t __arg0" : "void* __p, intptr_t __arg0, intptr_t __arg1")));
                    if (cap_n > 0) {
                        cc__append_fmt(&defs, &defs_len, &defs_cap,
                                       "  __cc_closure_env_%d* __env = (__cc_closure_env_%d*)__p;\n",
                                       id, id);
                        for (int ci = 0; ci < cap_n; ci++) {
                            int mo = (cap_flags && (cap_flags[ci] & 2) != 0);
                            cc__append_fmt(&defs, &defs_len, &defs_cap,
                                           "  %s %s = %s__env->%s%s;\n",
                                           cap_types && cap_types[ci] ? cap_types[ci] : "int",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? "cc_move(" : "",
                                           caps[ci] ? caps[ci] : "__cap",
                                           mo ? ")" : "");
                        }
                    } else {
                        cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__p;\n");
                    }
                    if (param_count == 1) {
                        if (param0[0]) {
                            if (param0_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", param0_type, param0, param0_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", param0);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
                    } else if (param_count == 2) {
                        if (param0[0]) {
                            if (param0_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg0;\n", param0_type, param0, param0_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg0;\n", param0);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg0;\n");
                        if (param1[0]) {
                            if (param1_type[0]) {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s %s = (%s)__arg1;\n", param1_type, param1, param1_type);
                            } else {
                                cc__append_fmt(&defs, &defs_len, &defs_cap, "  intptr_t %s = __arg1;\n", param1);
                            }
                        }
                        else cc__append_str(&defs, &defs_len, &defs_cap, "  (void)__arg1;\n");
                    }

                    cc__append_fmt(&defs, &defs_len, &defs_cap,
                                   "#line %d \"%s\"\n",
                                   abs_line, src_path ? src_path : "<src>");
                    if (body && body[0] == '{') {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "  %s\n", lowered ? lowered : body);
                    } else {
                        cc__append_fmt(&defs, &defs_len, &defs_cap, "  (void)(%s);\n", lowered ? lowered : body);
                    }
                    free(lowered);
                    cc__append_str(&defs, &defs_len, &defs_cap, "  return NULL;\n}\n\n");
                }

                /* advance cursor to end of literal */
                cur = body_end;
                line_no = end_line;
                /* If we ended at newline boundary, allow outer loop to progress normally. */
                if (*cur == '\n') { cur++; line_no++; }
                consumed_literal = 1;
                break;
            }
            if (consumed_literal) continue;
        }

        /* brace depth + scope cleanup (best-effort) */
        for (const char* x = line_start; x < line_end; x++) {
            if (*x == '{') {
                depth++;
                if (nursery_top >= 0 && nursery_depth[nursery_top] < 0) nursery_depth[nursery_top] = depth;
            } else if (*x == '}') {
                if (nursery_top >= 0 && nursery_depth[nursery_top] == depth) nursery_top--;
                if (depth > 0) {
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_names[depth][j]);
                    free(scope_names[depth]); scope_names[depth] = NULL;
                    for (int j = 0; j < scope_counts[depth]; j++) free(scope_types[depth][j]);
                    free(scope_types[depth]); scope_types[depth] = NULL;
                    free(scope_flags[depth]); scope_flags[depth] = NULL;
                    scope_counts[depth] = 0;
                    depth--;
                }
            }
        }

        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    /* scope names/types cleanup */
    for (int d = 0; d < 256; d++) {
        for (int i = 0; i < scope_counts[d]; i++) free(scope_names[d][i]);
        free(scope_names[d]);
        for (int i = 0; i < scope_counts[d]; i++) free(scope_types[d][i]);
        free(scope_types[d]);
        free(scope_flags[d]);
    }

    *out_descs = descs;
    *out_count = count;
    *out_line_map = line_map;
    *out_line_cap = lines + 2;
    if (out_protos) *out_protos = protos;
    if (out_protos_len) *out_protos_len = protos_len;
    *out_defs = defs;
    *out_defs_len = defs_len;
    return 1;
}

/* Lower a block-ish snippet of CC/C code in-memory (used for closure bodies).
   Best-effort: currently handles @nursery + spawn closure-literals. */
static char* cc__lower_cc_snippet(const char* text,
                                 size_t text_len,
                                 const char* src_path,
                                 int base_line,
                                 CCClosureDesc* closure_descs,
                                 int closure_count,
                                 int* closure_line_map,
                                 int closure_line_cap) {
    if (!text || text_len == 0) return NULL;
    char* out = NULL;
    size_t out_len = 0, out_cap = 0;

    int nursery_counter = 0;
    int nursery_id_stack[128];
    int nursery_depth_stack[128];
    int nursery_top = -1;
    int brace_depth = 0;

    const char* cur = text;
    int line_no = 1;
    while ((size_t)(cur - text) < text_len && *cur) {
        const char* line_start = cur;
        const char* nl = memchr(cur, '\n', (size_t)(text + text_len - cur));
        const char* line_end = nl ? nl : (text + text_len);
        size_t line_len = (size_t)(line_end - line_start);

        char line_buf[2048];
        size_t cp = line_len < sizeof(line_buf) - 1 ? line_len : sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, cp);
        line_buf[cp] = '\0';

        const char* p = line_buf;
        while (*p == ' ' || *p == '\t') p++;
        int abs_line = (base_line > 0 ? (base_line + line_no - 1) : line_no);

        /* Lower @nursery marker into a runtime nursery scope. */
        if (strncmp(p, "@nursery", 8) == 0 && (p[8] == ' ' || p[8] == '\t' || p[8] == '\n' || p[8] == '\r' || p[8] == '{')) {
            size_t indent_len = (size_t)(p - line_buf);
            char indent[256];
            if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
            memcpy(indent, line_buf, indent_len);
            indent[indent_len] = '\0';

            int id = ++nursery_counter;
            if (nursery_top + 1 < (int)(sizeof(nursery_id_stack) / sizeof(nursery_id_stack[0]))) {
                nursery_id_stack[++nursery_top] = id;
                nursery_depth_stack[nursery_top] = 0;
            }
            cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
            cc__append_fmt(&out, &out_len, &out_cap, "%sCCNursery* __cc_nursery%d = cc_nursery_create();\n", indent, id);
            cc__append_fmt(&out, &out_len, &out_cap, "%sif (!__cc_nursery%d) abort();\n", indent, id);
            cc__append_fmt(&out, &out_len, &out_cap, "%s{\n", indent);
            brace_depth++;
            if (nursery_top >= 0) nursery_depth_stack[nursery_top] = brace_depth;
            cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line + 1, src_path ? src_path : "<src>");
            goto next_line;
        }

        /* Lower spawn(() => { ... }) inside a nursery to cc_nursery_spawn_closure0. */
        if (strncmp(p, "spawn", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
            const char* s0 = p + 5;
            while (*s0 == ' ' || *s0 == '\t') s0++;
            if (*s0 == '(') {
                /* Closure literal: spawn(() => { ... }); uses closure_line_map from the pre-scan. */
                if (closure_line_map && line_no > 0 && line_no < closure_line_cap) {
                    int idx1 = closure_line_map[line_no];
                    if (idx1 > 0 && idx1 <= closure_count) {
                        CCClosureDesc* cd = &closure_descs[idx1 - 1];
                        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
                        cc__append_str(&out, &out_len, &out_cap, "{\n");
                        if (cur_nursery_id == 0) {
                            cc__append_str(&out, &out_len, &out_cap, "/* TODO: spawn outside nursery */\n");
                        } else {
                            cc__append_fmt(&out, &out_len, &out_cap, "  CCClosure0 __c = __cc_closure_make_%d(", cd->id);
                            if (cd->cap_count == 0) {
                                cc__append_str(&out, &out_len, &out_cap, ");\n");
                            } else {
                                for (int ci = 0; ci < cd->cap_count; ci++) {
                                    if (ci) cc__append_str(&out, &out_len, &out_cap, ", ");
                                    int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                    if (mo) cc__append_str(&out, &out_len, &out_cap, "cc_move(");
                                    cc__append_str(&out, &out_len, &out_cap, cd->cap_names[ci] ? cd->cap_names[ci] : "0");
                                    if (mo) cc__append_str(&out, &out_len, &out_cap, ")");
                                }
                                cc__append_str(&out, &out_len, &out_cap, ");\n");
                            }
                            cc__append_fmt(&out, &out_len, &out_cap, "  cc_nursery_spawn_closure0(__cc_nursery%d, __c);\n", cur_nursery_id);
                        }
                        cc__append_str(&out, &out_len, &out_cap, "}\n");

                        /* Skip original closure text lines (multiline). */
                        int target_end = cd->end_line;
                        while (line_no < target_end) {
                            if (!nl) break;
                            cur = nl + 1;
                            line_no++;
                            nl = memchr(cur, '\n', (size_t)(text + text_len - cur));
                        }
                        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", base_line + line_no, src_path ? src_path : "<src>");
                        goto next_line;
                    }
                }
            }
        }

        /* Before emitting a close brace, emit nursery epilogue if this closes a nursery scope. */
        if (p[0] == '}') {
            if (nursery_top >= 0 && nursery_depth_stack[nursery_top] == brace_depth) {
                size_t indent_len = (size_t)(p - line_buf);
                char indent[256];
                if (indent_len >= sizeof(indent)) indent_len = sizeof(indent) - 1;
                memcpy(indent, line_buf, indent_len);
                indent[indent_len] = '\0';

                int id = nursery_id_stack[nursery_top--];
                cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
                cc__append_fmt(&out, &out_len, &out_cap, "%s  cc_nursery_wait(__cc_nursery%d);\n", indent, id);
                cc__append_fmt(&out, &out_len, &out_cap, "%s  cc_nursery_free(__cc_nursery%d);\n", indent, id);
                cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
            }
        }

        /* Default: emit original line. */
        cc__append_fmt(&out, &out_len, &out_cap, "#line %d \"%s\"\n", abs_line, src_path ? src_path : "<src>");
        cc__append_str(&out, &out_len, &out_cap, line_buf);
        cc__append_str(&out, &out_len, &out_cap, "\n");

        /* Update brace depth. */
        for (size_t i = 0; i < cp; i++) {
            if (line_buf[i] == '{') brace_depth++;
            else if (line_buf[i] == '}') { if (brace_depth > 0) brace_depth--; }
        }

    next_line:
        if (!nl) break;
        cur = nl + 1;
        line_no++;
    }

    return out;
}

/* Recursively lower CC constructs inside a closure body, while collecting any additional closure thunks. */
static char* cc__lower_cc_in_block_text(const char* text,
                                       size_t text_len,
                                       const char* src_path,
                                       int base_line,
                                       int* io_next_closure_id,
                                       char** out_more_protos,
                                       size_t* out_more_protos_len,
                                       char** out_more_defs,
                                       size_t* out_more_defs_len) {
    if (out_more_protos) *out_more_protos = NULL;
    if (out_more_protos_len) *out_more_protos_len = 0;
    if (out_more_defs) *out_more_defs = NULL;
    if (out_more_defs_len) *out_more_defs_len = 0;
    if (!text || text_len == 0) return NULL;

    /* Pre-scan this snippet for nested spawn closures; this will also recursively generate their thunks. */
    CCClosureDesc* nested_descs = NULL;
    int nested_count = 0;
    int* nested_line_map = NULL;
    int nested_line_cap = 0;
    char* nested_protos = NULL;
    size_t nested_protos_len = 0;
    char* nested_defs = NULL;
    size_t nested_defs_len = 0;
    (void)cc__scan_spawn_closures(text, text_len, src_path,
                                 base_line, io_next_closure_id,
                                 &nested_descs, &nested_count,
                                 &nested_line_map, &nested_line_cap,
                                 &nested_protos, &nested_protos_len,
                                 &nested_defs, &nested_defs_len);

    if (out_more_protos) *out_more_protos = nested_protos;
    else free(nested_protos);
    if (out_more_protos_len) *out_more_protos_len = nested_protos_len;

    if (out_more_defs) *out_more_defs = nested_defs;
    else free(nested_defs);
    if (out_more_defs_len) *out_more_defs_len = nested_defs_len;

    char* lowered = cc__lower_cc_snippet(text, text_len, src_path, base_line,
                                         nested_descs, nested_count,
                                         nested_line_map, nested_line_cap);

    if (nested_descs) {
        for (int i = 0; i < nested_count; i++) {
            for (int j = 0; j < nested_descs[i].cap_count; j++) free(nested_descs[i].cap_names[j]);
            free(nested_descs[i].cap_names);
            for (int j = 0; j < nested_descs[i].cap_count; j++) free(nested_descs[i].cap_types ? nested_descs[i].cap_types[j] : NULL);
            free(nested_descs[i].cap_types);
            free(nested_descs[i].cap_flags);
            free(nested_descs[i].body);
        }
        free(nested_descs);
    }
    free(nested_line_map);

    return lowered;
}

/* Strip CC decl markers so output is valid C. This is used regardless of whether
   TCC extensions are available, because the output C is compiled by the host compiler. */
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

static int cc__strip_cc_decl_markers(const char* in, size_t in_len, char** out, size_t* out_len) {
    if (!in || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;

    /* Remove only these markers: @async, @noblock, @latency_sensitive.
       This is a conservative text pass so the generated C compiles; real semantics
       will be implemented by async lowering later. */
    char* buf = (char*)malloc(in_len + 1);
    if (!buf) return 0;
    size_t w = 0;
    for (size_t i = 0; i < in_len; ) {
        if (in[i] == '@') {
            const char* kw = NULL;
            size_t kw_len = 0;
            if (i + 6 <= in_len && memcmp(in + i + 1, "async", 5) == 0) { kw = "async"; kw_len = 5; }
            else if (i + 8 <= in_len && memcmp(in + i + 1, "noblock", 7) == 0) { kw = "noblock"; kw_len = 7; }
            else if (i + 18 <= in_len && memcmp(in + i + 1, "latency_sensitive", 17) == 0) { kw = "latency_sensitive"; kw_len = 17; }
            if (kw) {
                size_t j = i + 1 + kw_len;
                /* Ensure keyword boundary */
                if (j == in_len || !(isalnum((unsigned char)in[j]) || in[j] == '_')) {
                    i = j;
                    /* swallow one following space to avoid `@asyncvoid` */
                    if (i < in_len && (in[i] == ' ' || in[i] == '\t')) i++;
                    continue;
                }
            }
        }
        buf[w++] = in[i++];
    }
    buf[w] = 0;
    *out = buf;
    *out_len = w;
    return 1;
}

#ifdef CC_TCC_EXT_AVAILABLE
static int cc__rewrite_await_exprs_with_nodes(const CCASTRoot* root,
                                              const CCVisitorCtx* ctx,
                                              const char* in_src,
                                              size_t in_len,
                                              char** out_src,
                                              size_t* out_len) {
    if (!root || !ctx || !in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;
    if (!root->nodes || root->node_count <= 0) return 0;

    typedef struct NodeView {
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
    const NodeView* n = (const NodeView*)root->nodes;

    enum { CC_FN_ATTR_ASYNC = 1u << 0 };

    typedef struct {
        size_t start;
        size_t end;
        size_t insert_off;
        size_t trim_start;
        size_t trim_end;
        char tmp[64];
        char* insert_text; /* owned */
    } AwaitRep;
    AwaitRep reps[128];
    int rep_n = 0;

    if (getenv("CC_DEBUG_AWAIT_REWRITE")) {
        int aw = 0;
        for (int i = 0; i < root->node_count; i++) if (n[i].kind == 6) aw++;
        fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: await nodes in stub AST: %d\n", aw);
        int shown = 0;
        for (int i = 0; i < root->node_count && shown < 5; i++) {
            if (n[i].kind != 6) continue;
            if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
            size_t os = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
            fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE:  node[%d] file=%s line=%d col=%d off=%zu head='%.16s'\n",
                    i, n[i].file ? n[i].file : "<null>", n[i].line_start, n[i].col_start, os,
                    (os < in_len) ? (in_src + os) : "<oob>");
            shown++;
        }
    }

    for (int i = 0; i < root->node_count && rep_n < (int)(sizeof(reps) / sizeof(reps[0])); i++) {
        if (n[i].kind != 6) continue; /* AWAIT */
        if (n[i].line_start <= 0 || n[i].col_start <= 0) continue;
        if (n[i].line_end <= 0 || n[i].col_end <= 0) continue;
        size_t a_s = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_start, n[i].col_start);
        size_t a_e = cc__offset_of_line_col_1based(in_src, in_len, n[i].line_end, n[i].col_end);
        if (a_e <= a_s || a_e > in_len) continue;
        /* Best-effort: many nodes record `col_start` at the operand; recover the `await` keyword
           by scanning backward on the same line for the nearest `await` token. */
        {
            size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
            size_t k = a_s;
            size_t found = (size_t)-1;
            while (k > line_off + 4) {
                size_t s0 = k - 5;
                if (memcmp(in_src + s0, "await", 5) == 0) {
                    char before = (s0 > line_off) ? in_src[s0 - 1] : ' ';
                    char after = (s0 + 5 < in_len) ? in_src[s0 + 5] : ' ';
                    int before_ok = !(before == '_' || isalnum((unsigned char)before));
                    int after_ok = !(after == '_' || isalnum((unsigned char)after));
                    if (before_ok && after_ok) { found = s0; break; }
                }
                k--;
            }
            if (found != (size_t)-1) a_s = found;
            if (a_s + 5 > in_len || memcmp(in_src + a_s, "await", 5) != 0) continue;
        }

        /* Require inside an @async function (otherwise leave it; checker will error). */
        int cur = n[i].parent;
        int is_async = 0;
        int best_line = n[i].line_start;
        while (cur >= 0 && cur < root->node_count) {
            if (n[cur].kind == 12) {
                /* Any enclosing decl-item marked async implies we're inside @async. */
                if (((unsigned int)n[cur].aux2 & (unsigned int)CC_FN_ATTR_ASYNC) != 0) is_async = 1;
            }
            /* Find earliest line start among nearby statement-ish ancestors. */
            if ((n[cur].kind == 15 || n[cur].kind == 14 || n[cur].kind == 5) &&
                n[cur].line_start > 0 && n[cur].line_start < best_line) {
                best_line = n[cur].line_start;
            }
            cur = n[cur].parent;
        }
        if (!is_async) continue;

        /* Skip if await is already statement-root-ish: `await ...;`, `x = await ...;`, `return await ...;` */
        {
            size_t line_off = cc__offset_of_line_1based(in_src, in_len, n[i].line_start);
            size_t p = line_off;
            while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
            if (p == a_s) continue; /* await at start of statement line */
            /* Check if immediate lhs assignment `= await` by scanning backward for '=' on same line before await. */
            for (size_t k = a_s; k > line_off; k--) {
                char c = in_src[k - 1];
                if (c == '\n') break;
                if (c == '=') { goto skip_this_await; }
            }
            /* Check `return await` by scanning from line start. */
            if (p + 6 <= in_len && memcmp(in_src + p, "return", 6) == 0) {
                size_t q = p + 6;
                while (q < in_len && (in_src[q] == ' ' || in_src[q] == '\t')) q++;
                if (q == a_s) continue;
            }
        }

        /* Compute insertion offset at start of the enclosing statement line. */
        size_t insert_off = cc__offset_of_line_1based(in_src, in_len, best_line);
        if (insert_off > in_len) insert_off = in_len;

        char tmp[64];
        snprintf(tmp, sizeof(tmp), "__cc_aw_l%d_%d", n[i].line_start, rep_n);

        AwaitRep r;
        memset(&r, 0, sizeof(r));
        r.start = a_s;
        r.end = a_e;
        r.insert_off = insert_off;
        /* Trim bounds computed later (after all reps known) */
        strncpy(r.tmp, tmp, sizeof(r.tmp) - 1);
        reps[rep_n++] = r;
        continue;
    skip_this_await:
        (void)0;
    }

    if (rep_n == 0) return 0;

    /* Compute trimmed ranges now. */
    for (int i = 0; i < rep_n; i++) {
        size_t t0 = reps[i].start;
        size_t t1 = reps[i].end;
        while (t0 < t1 && (in_src[t0] == ' ' || in_src[t0] == '\t' || in_src[t0] == '\n' || in_src[t0] == '\r')) t0++;
        while (t1 > t0 && (in_src[t1 - 1] == ' ' || in_src[t1 - 1] == '\t' || in_src[t1 - 1] == '\n' || in_src[t1 - 1] == '\r')) t1--;
        reps[i].trim_start = t0;
        reps[i].trim_end = t1;
    }

    /* Build insertion texts. Ensure nested awaits inside an await-expression are replaced
       by the corresponding temp names (so outer hoists don't contain raw inner `await`). */
    for (int i = 0; i < rep_n; i++) {
        /* Indent prefix for this insertion */
        size_t insert_off = reps[i].insert_off;
        size_t ind_end = insert_off;
        while (ind_end < in_len && (in_src[ind_end] == ' ' || in_src[ind_end] == '\t')) ind_end++;
        size_t ind_len = ind_end - insert_off;

        /* Build await text with nested replacements. */
        char* await_txt = NULL;
        size_t await_len = 0, await_cap = 0;
        size_t cur = reps[i].trim_start;
        size_t end = reps[i].trim_end;
        while (cur < end) {
            int did = 0;
            for (int j = 0; j < rep_n; j++) {
                if (j == i) continue;
                if (reps[j].trim_start >= reps[i].trim_start &&
                    reps[j].trim_end <= reps[i].trim_end &&
                    reps[j].trim_start == cur) {
                    cc__append_str(&await_txt, &await_len, &await_cap, reps[j].tmp);
                    cur = reps[j].trim_end;
                    did = 1;
                    break;
                }
            }
            if (did) continue;
            cc__append_n(&await_txt, &await_len, &await_cap, in_src + cur, 1);
            cur++;
        }
        if (!await_txt || await_len == 0) { free(await_txt); continue; }

        /* Insert two statements: decl + assignment */
        size_t ins_cap = ind_len * 2 + await_len + 256;
        char* ins = (char*)malloc(ins_cap);
        if (!ins) { free(await_txt); continue; }
        int wn = 0;
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*sintptr_t %s = 0;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp);
        wn += snprintf(ins + (size_t)wn, ins_cap - (size_t)wn, "%.*s%s = %.*s;\n",
                       (int)ind_len, in_src + insert_off, reps[i].tmp, (int)await_len, await_txt);
        free(await_txt);
        if (wn <= 0) { free(ins); continue; }
        reps[i].insert_text = ins;
    }

    /* Sort by start asc for replacements; insertions will be handled by bucketed offsets. */
    for (int i = 0; i < rep_n; i++) {
        for (int j = i + 1; j < rep_n; j++) {
            if (reps[j].start < reps[i].start) {
                AwaitRep t = reps[i];
                reps[i] = reps[j];
                reps[j] = t;
            }
        }
    }

    /* Build output streaming: emit insertions when reaching an insertion offset. */
    char* out = NULL;
    size_t outl = 0, outc = 0;

    int ins_idx[128];
    for (int i = 0; i < rep_n; i++) ins_idx[i] = i;
    /* sort indices by insert_off asc */
    for (int i = 0; i < rep_n; i++) {
        for (int j = i + 1; j < rep_n; j++) {
            if (reps[ins_idx[j]].insert_off < reps[ins_idx[i]].insert_off) {
                int t = ins_idx[i]; ins_idx[i] = ins_idx[j]; ins_idx[j] = t;
            }
        }
    }
    int ins_p = 0;

    size_t cur_off = 0;
    int rep_i = 0;
    while (cur_off < in_len) {
        /* Emit any insertions at this offset (may be multiple). */
        if (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == cur_off) {
            /* Collect all with this insert_off, then emit in descending start order (inner first). */
            int tmp_idx[128];
            int tmp_n = 0;
            size_t off = reps[ins_idx[ins_p]].insert_off;
            while (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == off) {
                tmp_idx[tmp_n++] = ins_idx[ins_p++];
            }
            for (int a = 0; a < tmp_n; a++) {
                for (int b = a + 1; b < tmp_n; b++) {
                    if (reps[tmp_idx[b]].start > reps[tmp_idx[a]].start) {
                        int t = tmp_idx[a]; tmp_idx[a] = tmp_idx[b]; tmp_idx[b] = t;
                    }
                }
            }
            for (int k = 0; k < tmp_n; k++) {
                const char* it = reps[tmp_idx[k]].insert_text;
                if (it) cc__append_str(&out, &outl, &outc, it);
            }
        }
        /* Apply next replacement if it starts here. */
        if (rep_i < rep_n && reps[rep_i].start == cur_off) {
            cc__append_str(&out, &outl, &outc, reps[rep_i].tmp);
            cur_off = reps[rep_i].end;
            rep_i++;
            continue;
        }
        /* Otherwise copy one byte */
        cc__append_n(&out, &outl, &outc, in_src + cur_off, 1);
        cur_off++;
    }
    /* Insertions at EOF */
    while (ins_p < rep_n && reps[ins_idx[ins_p]].insert_off == cur_off) {
        const char* it = reps[ins_idx[ins_p]].insert_text;
        if (it) cc__append_str(&out, &outl, &outc, it);
        ins_p++;
    }

    for (int i = 0; i < rep_n; i++) free(reps[i].insert_text);
    if (!out) return 0;
    *out_src = out;
    *out_len = outl;
    return 1;
}
#endif

static char* cc__rewrite_idents_to_repls(const char* s,
                                        const char* const* names,
                                        const char* const* repls,
                                        int n) {
    if (!s) return NULL;
    if (n <= 0) return strdup(s);
    size_t sl = strlen(s);
    size_t out_cap = sl * 3 + 64;
    char* out = (char*)malloc(out_cap);
    if (!out) return NULL;
    size_t out_len = 0;

    for (size_t i = 0; i < sl; ) {
        if (cc__is_ident_start_char(s[i])) {
            size_t j = i + 1;
            while (j < sl && cc__is_ident_char2(s[j])) j++;
            int did = 0;
            for (int k = 0; k < n; k++) {
                size_t nl = strlen(names[k]);
                if (nl == (j - i) && memcmp(s + i, names[k], nl) == 0) {
                    const char* r = repls[k];
                    size_t rl = strlen(r);
                    if (out_len + rl + 1 >= out_cap) {
                        out_cap = out_cap * 2 + rl + 64;
                        out = (char*)realloc(out, out_cap);
                        if (!out) return NULL;
                    }
                    memcpy(out + out_len, r, rl);
                    out_len += rl;
                    did = 1;
                    break;
                }
            }
            if (!did) {
                size_t tl = j - i;
                if (out_len + tl + 1 >= out_cap) {
                    out_cap = out_cap * 2 + tl + 64;
                    out = (char*)realloc(out, out_cap);
                    if (!out) return NULL;
                }
                memcpy(out + out_len, s + i, tl);
                out_len += tl;
            }
            i = j;
            continue;
        }
        if (out_len + 2 >= out_cap) {
            out_cap = out_cap * 2 + 64;
            out = (char*)realloc(out, out_cap);
            if (!out) return NULL;
        }
        out[out_len++] = s[i++];
    }
    out[out_len] = 0;
    return out;
}

static int cc__rewrite_async_state_machine_noarg_text(const char* in_src,
                                                      size_t in_len,
                                                      char** out_src,
                                                      size_t* out_len) {
    if (!in_src || !out_src || !out_len) return 0;
    *out_src = NULL;
    *out_len = 0;

    typedef struct {
        size_t start;
        size_t end;
        int orig_nl;
        int is_await;
        char name[128];
        char expr[256];
        char callee[128];
    } AsyncFn;
    AsyncFn fns[64];
    int fn_n = 0;

    size_t i = 0;
    while (i + 6 < in_len && fn_n < (int)(sizeof(fns)/sizeof(fns[0]))) {
        if (in_src[i] != '@') { i++; continue; }
        size_t j = i + 1;
        while (j < in_len && (in_src[j] == ' ' || in_src[j] == '\t')) j++;
        if (j + 5 > in_len || memcmp(in_src + j, "async", 5) != 0) { i++; continue; }
        size_t p = j + 5;
        if (p < in_len && (isalnum((unsigned char)in_src[p]) || in_src[p] == '_')) { i++; continue; }
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t' || in_src[p] == '\n' || in_src[p] == '\r')) p++;
        if (p + 3 > in_len || memcmp(in_src + p, "int", 3) != 0) { i++; continue; } /* int or intptr_t; keep simple */
        if (p + 8 <= in_len && memcmp(in_src + p, "intptr_t", 8) == 0) p += 8;
        else p += 3;
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p >= in_len || !cc__is_ident_start_char(in_src[p])) { i++; continue; }
        size_t ns = p++;
        while (p < in_len && cc__is_ident_char2(in_src[p])) p++;
        size_t nn = p - ns;
        if (nn == 0 || nn >= 128) { i++; continue; }
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p >= in_len || in_src[p] != '(') { i++; continue; }
        p++;
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p + 4 <= in_len && memcmp(in_src + p, "void", 4) == 0) p += 4;
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t')) p++;
        if (p >= in_len || in_src[p] != ')') { i++; continue; }
        p++;
        while (p < in_len && (in_src[p] == ' ' || in_src[p] == '\t' || in_src[p] == '\n' || in_src[p] == '\r')) p++;
        if (p >= in_len || in_src[p] != '{') { i++; continue; }
        size_t body_lbrace = p;
        int depth = 0;
        size_t q = body_lbrace;
        for (; q < in_len; q++) {
            char ch = in_src[q];
            if (ch == '"' || ch == '\'') {
                char quote = ch;
                q++;
                while (q < in_len) {
                    char c2 = in_src[q];
                    if (c2 == '\\' && q + 1 < in_len) { q += 2; continue; }
                    if (c2 == quote) break;
                    q++;
                }
                continue;
            }
            if (ch == '{') depth++;
            else if (ch == '}') { depth--; if (depth == 0) { q++; break; } }
        }
        if (depth != 0) { i++; continue; }
        size_t end = q;
        while (end < in_len && in_src[end] != '\n') end++;
        if (end < in_len) end++;

        const char* body = in_src + body_lbrace + 1;
        const char* body_end = in_src + (q - 1);
        while (body < body_end && (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r')) body++;
        if ((body_end - body) < 6 || memcmp(body, "return", 6) != 0) { i++; continue; }
        body += 6;
        while (body < body_end && (*body == ' ' || *body == '\t')) body++;
        int is_await = 0;
        if ((body_end - body) >= 5 && memcmp(body, "await", 5) == 0) { is_await = 1; body += 5; while (body < body_end && (*body == ' ' || *body == '\t')) body++; }
        const char* semi = memchr(body, ';', (size_t)(body_end - body));
        if (!semi) { i++; continue; }
        const char* tail = semi + 1;
        while (tail < body_end && (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r')) tail++;
        if (tail != body_end) { i++; continue; }

        AsyncFn fn;
        memset(&fn, 0, sizeof(fn));
        fn.start = i;
        fn.end = end;
        fn.is_await = is_await;
        memcpy(fn.name, in_src + ns, nn);
        fn.name[nn] = 0;
        size_t expr_n = (size_t)(semi - body);
        if (expr_n >= sizeof(fn.expr)) { i++; continue; }
        memcpy(fn.expr, body, expr_n);
        fn.expr[expr_n] = 0;
        if (is_await) {
            const char* lpc = strchr(fn.expr, '(');
            const char* rpc = strrchr(fn.expr, ')');
            if (!lpc || !rpc || rpc < lpc) { i++; continue; }
            const char* ap = lpc + 1;
            while (ap < rpc && (*ap == ' ' || *ap == '\t' || *ap == '\n' || *ap == '\r')) ap++;
            if (ap != rpc) { i++; continue; }
            size_t cn = (size_t)(lpc - fn.expr);
            while (cn > 0 && (fn.expr[cn - 1] == ' ' || fn.expr[cn - 1] == '\t')) cn--;
            if (cn == 0 || cn >= sizeof(fn.callee)) { i++; continue; }
            memcpy(fn.callee, fn.expr, cn);
            fn.callee[cn] = 0;
        }
        fn.orig_nl = 0;
        for (size_t t = fn.start; t < fn.end; t++) if (in_src[t] == '\n') fn.orig_nl++;
        fns[fn_n++] = fn;
        i = end;
    }

    if (fn_n == 0) return 0;
    char* cur = (char*)malloc(in_len + 1);
    if (!cur) return 0;
    memcpy(cur, in_src, in_len);
    cur[in_len] = 0;
    size_t cur_len = in_len;

    static int g_async_id_text = 20000;
    for (int fi = fn_n - 1; fi >= 0; fi--) {
        AsyncFn* fn = &fns[fi];
        int id = g_async_id_text++;
        char repl[4096];
        int rn = 0;
        if (!fn->is_await) {
            rn = snprintf(repl, sizeof(repl),
                          "typedef struct{int __st; intptr_t __r;}__cc_af%d_f;static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){(void)__e;__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:__f->__r=(intptr_t)(%s);__f->__st=1;/*fall*/case 1:if(__o)*__o=__f->__r;return CC_FUTURE_READY;}return CC_FUTURE_ERR;}static void __cc_af%d_drop(void*__p){free(__p);}CCTaskIntptr %s(void){__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;return cc_task_intptr_make_poll(__cc_af%d_poll,__f,__cc_af%d_drop);}",
                          id, id, id, id, fn->expr, id, fn->name, id, id, id, id, id);
        } else {
            rn = snprintf(repl, sizeof(repl),
                          "typedef struct{int __st; CCTaskIntptr __t; intptr_t __r;}__cc_af%d_f;static CCFutureStatus __cc_af%d_poll(void*__p,intptr_t*__o,int*__e){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(!__f)return CC_FUTURE_ERR;switch(__f->__st){case 0:__f->__t=%s();__f->__st=1;/*fall*/case 1:{intptr_t __v=0;int __err=0;CCFutureStatus __st=cc_task_intptr_poll(&__f->__t,&__v,&__err);if(__st==CC_FUTURE_PENDING){return CC_FUTURE_PENDING;}cc_task_intptr_free(&__f->__t);(void)__e; if(__o)*__o=__v; __f->__r=__v; __f->__st=2;return CC_FUTURE_READY;}case 2:if(__o)*__o=__f->__r;return CC_FUTURE_READY;}return CC_FUTURE_ERR;}static void __cc_af%d_drop(void*__p){__cc_af%d_f*__f=(__cc_af%d_f*)__p;if(__f){cc_task_intptr_free(&__f->__t);free(__f);}}CCTaskIntptr %s(void){__cc_af%d_f*__f=(__cc_af%d_f*)calloc(1,sizeof(__cc_af%d_f));if(!__f){CCTaskIntptr __t;memset(&__t,0,sizeof(__t));return __t;}__f->__st=0;memset(&__f->__t,0,sizeof(__f->__t));return cc_task_intptr_make_poll(__cc_af%d_poll,__f,__cc_af%d_drop);}",
                          id, id, id, id, fn->callee, id, id, id, fn->name, id, id, id, id, id);
        }
        if (rn <= 0 || (size_t)rn >= sizeof(repl)) continue;
        int repl_nl = 0;
        for (int kk = 0; kk < rn; kk++) if (repl[kk] == '\n') repl_nl++;
        if (repl_nl > fn->orig_nl) continue;
        size_t padded_len = (size_t)rn + (size_t)(fn->orig_nl - repl_nl);
        char* padded = (char*)malloc(padded_len + 1);
        if (!padded) continue;
        memcpy(padded, repl, (size_t)rn);
        for (int kk = 0; kk < (fn->orig_nl - repl_nl); kk++) padded[rn + kk] = '\n';
        padded[padded_len] = 0;
        size_t new_len = cur_len - (fn->end - fn->start) + padded_len;
        char* next = (char*)malloc(new_len + 1);
        if (!next) { free(padded); continue; }
        memcpy(next, cur, fn->start);
        memcpy(next + fn->start, padded, padded_len);
        memcpy(next + fn->start + padded_len, cur + fn->end, cur_len - fn->end);
        next[new_len] = 0;
        free(padded);
        free(cur);
        cur = next;
        cur_len = new_len;
    }

    *out_src = cur;
    *out_len = cur_len;
    return 1;
}

#ifndef CC_TCC_EXT_AVAILABLE
// Minimal fallbacks when TCC extensions are not available.
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

/* cc__read_entire_file is defined unconditionally earlier. */

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

    /* Rewrite closure calls anywhere (including nested + multiline) using stub CALL nodes. */
#ifdef CC_TCC_EXT_AVAILABLE
    char* src_calls = NULL;
    size_t src_calls_len = 0;
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        if (cc__rewrite_all_closure_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &src_calls, &src_calls_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = src_calls;
            src_ufcs_len = src_calls_len;
        }
    }
#endif

    /* Auto-blocking (first cut): inside @async functions, wrap statement-form calls to known
       non-@async/non-@noblock functions in cc_run_blocking_closure0(() => { ... }). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0 && ctx->symbols) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_autoblocking_calls_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }
#endif

    /* Normalize `await <expr>` used inside larger expressions into temp hoists so the
       text-based async state machine can lower it (AST-driven span rewrite). */
#ifdef CC_TCC_EXT_AVAILABLE
    if (src_ufcs && root && root->nodes && root->node_count > 0) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        if (cc__rewrite_await_exprs_with_nodes(root, ctx, src_ufcs, src_ufcs_len, &rewritten, &rewritten_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
        if (getenv("CC_DEBUG_AWAIT_REWRITE") && src_ufcs) {
            const char* needle = "@async int f";
            const char* p = strstr(src_ufcs, needle);
            if (!p) p = strstr(src_ufcs, "@async");
            if (p) {
                fprintf(stderr, "CC_DEBUG_AWAIT_REWRITE: ---- snippet ----\n");
                size_t off = (size_t)(p - src_ufcs);
                size_t take = 800;
                if (off + take > src_ufcs_len) take = src_ufcs_len - off;
                fwrite(p, 1, take, stderr);
                fprintf(stderr, "\nCC_DEBUG_AWAIT_REWRITE: ---- end ----\n");
            }
        }
    }
#endif

    /* Text-based @async lowering (state machine) after all span-driven rewrites.
       This pass is allowed to change offsets because it runs last in the pipeline. */
    if (src_ufcs) {
        char* rewritten = NULL;
        size_t rewritten_len = 0;
        int ar = cc_async_rewrite_state_machine_text(src_ufcs, src_ufcs_len, &rewritten, &rewritten_len);
        if (ar < 0) {
            /* async_text already printed an error */
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return EINVAL;
        }
        if (ar > 0) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = rewritten;
            src_ufcs_len = rewritten_len;
        }
    }

    /* Strip CC decl markers so output is valid C (run after async lowering so it can see `@async`). */
    if (src_ufcs) {
        char* stripped = NULL;
        size_t stripped_len = 0;
        if (cc__strip_cc_decl_markers(src_ufcs, src_ufcs_len, &stripped, &stripped_len)) {
            if (src_ufcs != src_all) free(src_ufcs);
            src_ufcs = stripped;
            src_ufcs_len = stripped_len;
        }
    }

    /* NOTE: slice move/provenance checking is now handled by the stub-AST checker pass
       (`cc/src/visitor/checker.c`) before visitor lowering. */

    fprintf(out, "/* CC visitor: passthrough of lowered C (preprocess + TCC parse) */\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <stdint.h>\n");
    fprintf(out, "#include \"cc_nursery.cch\"\n");
    fprintf(out, "#include \"cc_closure.cch\"\n");
    fprintf(out, "#include \"cc_slice.cch\"\n");
    fprintf(out, "#include \"cc_runtime.cch\"\n");
    fprintf(out, "#include \"std/task_intptr.cch\"\n");
    /* Helper alias: used for auto-blocking arg binding so async_text doesn't hoist/rewrite these temps. */
    fprintf(out, "typedef intptr_t CCAbIntptr;\n");
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
    char* closure_protos = NULL;
    size_t closure_protos_len = 0;
    char* closure_defs = NULL;
    size_t closure_defs_len = 0;
    if (src_ufcs) {
        int closure_next_id = 1;
        cc__scan_spawn_closures(src_ufcs, src_ufcs_len, src_path,
                                1, &closure_next_id,
                                &closure_descs, &closure_count,
                                &closure_line_map, &closure_line_cap,
                                &closure_protos, &closure_protos_len,
                                &closure_defs, &closure_defs_len);
    }

    /* Capture type check (best-effort):
       We can only lower captures when we can infer a file-scope-safe type string for each captured name. */
    if (closure_descs) {
        for (int i = 0; i < closure_count; i++) {
            for (int ci = 0; ci < closure_descs[i].cap_count; ci++) {
                if (closure_descs[i].cap_types && closure_descs[i].cap_types[ci]) continue;
                int col1 = closure_descs[i].start_col >= 0 ? (closure_descs[i].start_col + 1) : 1;
                fprintf(stderr,
                        "%s:%d:%d: error: CC: cannot infer type for captured name '%s' (capture-by-copy currently supports simple decls like 'int x = ...;' or 'T* p = ...;')\n",
                        src_path,
                        closure_descs[i].start_line,
                        col1,
                        closure_descs[i].cap_names && closure_descs[i].cap_names[ci] ? closure_descs[i].cap_names[ci] : "?");
                fclose(out);
                for (int k = 0; k < closure_count; k++) {
                    for (int j = 0; j < closure_descs[k].cap_count; j++) free(closure_descs[k].cap_names[j]);
                    free(closure_descs[k].cap_names);
                    for (int j = 0; j < closure_descs[k].cap_count; j++) free(closure_descs[k].cap_types ? closure_descs[k].cap_types[j] : NULL);
                    free(closure_descs[k].cap_types);
                    free(closure_descs[k].param1_name);
                    free(closure_descs[k].param0_name);
                    free(closure_descs[k].param0_type);
                    free(closure_descs[k].param1_type);
                    free(closure_descs[k].body);
                }
                free(closure_descs);
                free(closure_line_map);
                free(closure_protos);
                free(closure_defs);
                if (src_ufcs != src_all) free(src_ufcs);
                free(src_all);
                return EINVAL;
            }
        }
    }

    /* Captures are lowered via __cc_closure_make_N factories. */
    if (closure_protos && closure_protos_len > 0) {
        fputs("/* --- CC closure forward decls --- */\n", out);
        fwrite(closure_protos, 1, closure_protos_len, out);
        fputs("/* --- end closure forward decls --- */\n\n", out);
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
        /* Multiline spawn stmt spans: start_line -> end_line (inclusive). */
        int* spawn_ml_end = NULL;
        int spawn_ml_cap = 0;
        /* Spawn arg count by stmt start line (from stub AST direct children). */
        unsigned char* spawn_argc = NULL;
        int spawn_argc_cap = 0;
        if (root && root->nodes && root->node_count > 0) {
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
            int max_start = 0;
            int max_spawn = 0;
            for (int i = 0; i < root->node_count; i++) {
                if (n[i].kind != 5) continue;          /* CALL */
                int is_ufcs = (n[i].aux2 & 2) != 0;
                if (!is_ufcs) continue;                /* only UFCS-marked calls */
                if (!n[i].aux_s1) continue;
                if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                if (n[i].line_end > n[i].line_start && n[i].line_start > max_start)
                    max_start = n[i].line_start;
                if (n[i].line_start > ufcs_single_cap)
                    ufcs_single_cap = n[i].line_start;
            }
            for (int i = 0; i < root->node_count; i++) {
                if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                if (n[i].kind == 3 && n[i].aux_s1 && strcmp(n[i].aux_s1, "spawn") == 0) {
                    if (n[i].line_end > n[i].line_start && n[i].line_start > max_spawn)
                        max_spawn = n[i].line_start;
                }
            }
            if (max_start > 0) {
                ufcs_ml_cap = max_start + 1;
                ufcs_ml_end = (int*)calloc((size_t)ufcs_ml_cap, sizeof(int));
                if (ufcs_ml_end) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (n[i].kind != 5) continue;
                        int is_ufcs = (n[i].aux2 & 2) != 0;
                        if (!is_ufcs) continue;
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
                        int is_ufcs = (n[i].aux2 & 2) != 0;
                        if (!is_ufcs) continue;
                        if (!n[i].aux_s1) continue;
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].line_start > 0 && n[i].line_start <= ufcs_single_cap)
                            ufcs_single[n[i].line_start] = 1;
                    }
                }
            }

            if (max_spawn > 0) {
                spawn_ml_cap = max_spawn + 1;
                spawn_ml_end = (int*)calloc((size_t)spawn_ml_cap, sizeof(int));
                if (spawn_ml_end) {
                    for (int i = 0; i < root->node_count; i++) {
                        if (!cc__node_file_matches_this_tu(root, ctx, n[i].file)) continue;
                        if (n[i].kind != 3) continue;
                        if (!n[i].aux_s1 || strcmp(n[i].aux_s1, "spawn") != 0) continue;
                        if (n[i].line_end > n[i].line_start &&
                            n[i].line_start > 0 &&
                            n[i].line_start < spawn_ml_cap) {
                            if (n[i].line_end > spawn_ml_end[n[i].line_start])
                                spawn_ml_end[n[i].line_start] = n[i].line_end;
                        }
                    }
                }

                spawn_argc_cap = spawn_ml_cap;
                spawn_argc = (unsigned char*)calloc((size_t)spawn_argc_cap, 1);
                if (spawn_argc) {
                    for (int si = 0; si < root->node_count; si++) {
                        if (!cc__node_file_matches_this_tu(root, ctx, n[si].file)) continue;
                        if (n[si].kind != 3) continue;
                        if (!n[si].aux_s1 || strcmp(n[si].aux_s1, "spawn") != 0) continue;
                        int ls = n[si].line_start;
                        if (ls <= 0 || ls >= spawn_argc_cap) continue;
                        int argc = 0;
                        for (int j = 0; j < root->node_count; j++) {
                            if (n[j].parent != si) continue;
                            if (!cc__node_file_matches_this_tu(root, ctx, n[j].file)) continue;
                            argc++;
                        }
                        if (argc < 0) argc = 0;
                        if (argc > 255) argc = 255;
                        spawn_argc[ls] = (unsigned char)argc;
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

        /* Track local decls (best-effort) so we can recognize CCClosure1 variables for call lowering. */
        char** decl_names[256];
        char** decl_types[256];
        unsigned char* decl_flags[256];
        int decl_counts[256];
        for (int i = 0; i < 256; i++) {
            decl_names[i] = NULL;
            decl_types[i] = NULL;
            decl_flags[i] = NULL;
            decl_counts[i] = 0;
        }

        int brace_depth = 0;
        /* nursery id stack is used for spawn lowering */
        int src_line_no = 0;
        const int cc_line_cap = 65536;
        const int cc_rewritten_cap = 131072;
        char* line = (char*)malloc((size_t)cc_line_cap);
        char* rewritten = (char*)malloc((size_t)cc_rewritten_cap);
        if (!line || !rewritten) {
            free(line);
            free(rewritten);
            fclose(in);
            fclose(out);
            if (src_ufcs != src_all) free(src_ufcs);
            free(src_all);
            return ENOMEM;
        }
        while (fgets(line, cc_line_cap, in)) {
            src_line_no++;
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;

            /* Track decls before any rewriting so we can later lower `c(arg);` for CCClosure1 vars. */
            cc__maybe_record_decl(decl_names, decl_types, decl_flags, decl_counts, brace_depth, line);

            /* Multiline spawn lowering (buffer by stub span) */
            if (spawn_ml_end && src_line_no > 0 && src_line_no < spawn_ml_cap && spawn_ml_end[src_line_no] > src_line_no) {
                int is_closure_literal_spawn = (closure_line_map &&
                                               src_line_no > 0 &&
                                               src_line_no < closure_line_cap &&
                                               closure_line_map[src_line_no] > 0);
                if (!is_closure_literal_spawn) {
                int start_line = src_line_no;
                int end_line = spawn_ml_end[src_line_no];
                int expected_argc = (spawn_argc && start_line > 0 && start_line < spawn_argc_cap) ? (int)spawn_argc[start_line] : 0;
                size_t buf_cap = 1024, buf_len = 0;
                char* buf = (char*)malloc(buf_cap);
                if (!buf) { /* fallthrough */ }
                else {
                    buf[0] = '\0';
                    size_t ll = strnlen(line, (size_t)cc_line_cap);
                    if (buf_len + ll + 1 > buf_cap) { while (buf_len + ll + 1 > buf_cap) buf_cap *= 2; buf = (char*)realloc(buf, buf_cap); }
                    if (!buf) { /* fallthrough */ }
                    else {
                        memcpy(buf + buf_len, line, ll); buf_len += ll; buf[buf_len] = '\0';
                        while (src_line_no < end_line && fgets(line, cc_line_cap, in)) {
                            src_line_no++;
                            ll = strnlen(line, (size_t)cc_line_cap);
                            if (buf_len + ll + 1 > buf_cap) { while (buf_len + ll + 1 > buf_cap) buf_cap *= 2; buf = (char*)realloc(buf, buf_cap); if (!buf) break; }
                            if (!buf) break;
                            memcpy(buf + buf_len, line, ll); buf_len += ll; buf[buf_len] = '\0';
                        }
                        if (buf) {
                            /* Reuse the existing single-line spawn parser but on the buffered chunk. */
                            char* pp = buf;
                            while (*pp == ' ' || *pp == '\t') pp++;
                            if (strncmp(pp, "spawn", 5) == 0 && (pp[5] == ' ' || pp[5] == '\t')) {
                                int cur_nursery_id = (nursery_top >= 0) ? nursery_id_stack[nursery_top] : 0;
                                const char* s0 = pp + 5;
                                while (*s0 == ' ' || *s0 == '\t') s0++;
                                if (*s0 == '(') {
                                    s0++;
                                    /* Find matching ')' at depth. */
                                    const char* expr_start = s0;
                                    const char* p2 = expr_start;
                                    int par = 0, brk = 0, br = 0;
                                    int ins = 0;
                                    char qch = 0;
                                    while (*p2) {
                                        char ch = *p2;
                                        if (ins) {
                                            if (ch == '\\' && p2[1]) { p2 += 2; continue; }
                                            if (ch == qch) ins = 0;
                                            p2++;
                                            continue;
                                        }
                                        if (ch == '"' || ch == '\'') { ins = 1; qch = ch; p2++; continue; }
                                        if (ch == '(') par++;
                                        else if (ch == ')') { if (par == 0 && brk == 0 && br == 0) break; par--; }
                                        else if (ch == '[') brk++;
                                        else if (ch == ']') { if (brk) brk--; }
                                        else if (ch == '{') br++;
                                        else if (ch == '}') { if (br) br--; }
                                        p2++;
                                    }
                                    if (*p2 == ')') {
                                        const char* expr_end = p2;
                                        while (expr_end > expr_start && (expr_end[-1] == ' ' || expr_end[-1] == '\t' || expr_end[-1] == '\n' || expr_end[-1] == '\r')) expr_end--;
                                        size_t expr_len = (size_t)(expr_end - expr_start);
                                        /* top-level comma split */
                                        int comma_pos[2] = { -1, -1 };
                                        int comma_n = 0;
                                        int dpar = 0, dbrk2 = 0, dbr2 = 0;
                                        int ins2 = 0; char q2 = 0;
                                        for (size_t ii = 0; ii < expr_len; ii++) {
                                            char ch = expr_start[ii];
                                            if (ins2) {
                                                if (ch == '\\' && ii + 1 < expr_len) { ii++; continue; }
                                                if (ch == q2) ins2 = 0;
                                                continue;
                                            }
                                            if (ch == '"' || ch == '\'') { ins2 = 1; q2 = ch; continue; }
                                            if (ch == '(') dpar++;
                                            else if (ch == ')') { if (dpar) dpar--; }
                                            else if (ch == '[') dbrk2++;
                                            else if (ch == ']') { if (dbrk2) dbrk2--; }
                                            else if (ch == '{') dbr2++;
                                            else if (ch == '}') { if (dbr2) dbr2--; }
                                            else if (ch == ',' && dpar == 0 && dbrk2 == 0 && dbr2 == 0) {
                                                if (comma_n < 2) comma_pos[comma_n++] = (int)ii;
                                            }
                                        }
                                        if (cur_nursery_id != 0) {
                                            int argc = expected_argc ? expected_argc : (comma_n + 1);
                                            if (argc < 1) argc = 1;
                                            if (argc > 3) argc = 3;

                                            if (argc == 1 && comma_n == 0) {
                                                const char* c0 = expr_start;
                                                size_t c0_len = expr_len;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c0_len > 0 && (*c0 == ' ' || *c0 == '\t')) { c0++; c0_len--; }
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure0 __c = %.*s; cc_nursery_spawn_closure0(__cc_nursery%d, __c); }\n",
                                                        (int)c0_len, c0, cur_nursery_id);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                            if (argc == 2 && comma_n >= 1) {
                                                const char* c0 = expr_start;
                                                const char* c1 = expr_start + comma_pos[0] + 1;
                                                size_t c0_len = (size_t)comma_pos[0];
                                                size_t c1_len = expr_len - (size_t)comma_pos[0] - 1;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                                while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure1 __c = %.*s; cc_nursery_spawn_closure1(__cc_nursery%d, __c, (intptr_t)(%.*s)); }\n",
                                                        (int)c0_len, c0, cur_nursery_id, (int)c1_len, c1);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                            if (argc == 3 && comma_n >= 2) {
                                                const char* c0 = expr_start;
                                                const char* c1 = expr_start + comma_pos[0] + 1;
                                                const char* c2 = expr_start + comma_pos[1] + 1;
                                                size_t c0_len = (size_t)comma_pos[0];
                                                size_t c1_len = (size_t)(comma_pos[1] - comma_pos[0] - 1);
                                                size_t c2_len = expr_len - (size_t)comma_pos[1] - 1;
                                                while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                                while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                                while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                                while (c2_len > 0 && (*c2 == ' ' || *c2 == '\t')) { c2++; c2_len--; }
                                                while (c2_len > 0 && (c2[c2_len - 1] == ' ' || c2[c2_len - 1] == '\t')) c2_len--;
                                                fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                                                fprintf(out, "{ CCClosure2 __c = %.*s; cc_nursery_spawn_closure2(__cc_nursery%d, __c, (intptr_t)(%.*s), (intptr_t)(%.*s)); }\n",
                                                        (int)c0_len, c0, cur_nursery_id,
                                                        (int)c1_len, c1,
                                                        (int)c2_len, c2);
                                                free(buf);
                                                fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                            /* Fallback: just emit buffered chunk */
                            fprintf(out, "#line %d \"%s\"\n", start_line, src_path);
                            fwrite(buf, 1, buf_len, out);
                            free(buf);
                            fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                            continue;
                        }
                    }
                }
                }
            }

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
                            if (cd->param_count != 0) {
                                /* Not supported in spawn yet. Fall back to other spawn lowering paths. */
                            } else {
                            fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                            fprintf(out, "{\n");
                            fprintf(out, "  CCClosure0 __c = __cc_closure_make_%d(", cd->id);
                            if (cd->cap_count == 0) {
                                fprintf(out, ");\n");
                            } else {
                                for (int ci = 0; ci < cd->cap_count; ci++) {
                                    if (ci) fprintf(out, ", ");
                                    int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                    if (mo) fprintf(out, "cc_move(");
                                    fprintf(out, "%s", cd->cap_names[ci] ? cd->cap_names[ci] : "0");
                                    if (mo) fprintf(out, ")");
                                }
                                fprintf(out, ");\n");
                            }
                            fprintf(out, "  cc_nursery_spawn_closure0(__cc_nursery%d, __c);\n", cur_nursery_id);
                            fprintf(out, "}\n");
                            /* Skip original closure text lines (multiline). */
                            while (src_line_no < cd->end_line && fgets(line, cc_line_cap, in)) {
                                src_line_no++;
                            }
                            /* Resync source mapping after eliding original closure text. */
                            fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                            continue;
                            }
                        }
                    }

                    /* spawn(<closure_expr>); where the expression is a CCClosure0 value.
                       Best-effort heuristic: accept identifiers and cc_closure0_make(...). */
                    {
                        const char* expr_start = s0;
                        const char* p2 = expr_start;
                        int par = 0;
                        while (*p2) {
                            if (*p2 == '(') par++;
                            else if (*p2 == ')') {
                                if (par == 0) break;
                                par--;
                            }
                            p2++;
                        }
                        if (*p2 == ')') {
                            const char* expr_end = p2;
                            while (expr_end > expr_start && (expr_end[-1] == ' ' || expr_end[-1] == '\t')) expr_end--;
                            size_t expr_len = (size_t)(expr_end - expr_start);
                            /* Support spawn(c, arg) for CCClosure1 and spawn(c, a, b) for CCClosure2 (nursery only). */
                            {
                                /* Find top-level commas */
                                int comma_pos[2] = { -1, -1 };
                                int comma_n = 0;
                                int dpar = 0, dbrk = 0, dbr = 0;
                                int ins = 0;
                                char qch = 0;
                                for (size_t i = 0; i < expr_len; i++) {
                                    char ch = expr_start[i];
                                    if (ins) {
                                        if (ch == '\\' && i + 1 < expr_len) { i++; continue; }
                                        if (ch == qch) ins = 0;
                                        continue;
                                    }
                                    if (ch == '"' || ch == '\'') { ins = 1; qch = ch; continue; }
                                    if (ch == '(') dpar++;
                                    else if (ch == ')') { if (dpar) dpar--; }
                                    else if (ch == '[') dbrk++;
                                    else if (ch == ']') { if (dbrk) dbrk--; }
                                    else if (ch == '{') dbr++;
                                    else if (ch == '}') { if (dbr) dbr--; }
                                    else if (ch == ',' && dpar == 0 && dbrk == 0 && dbr == 0) {
                                        if (comma_n < 2) comma_pos[comma_n++] = (int)i;
                                    }
                                }

                                if (cur_nursery_id != 0 && (comma_n == 1 || comma_n == 2)) {
                                    const char* c0 = expr_start;
                                    const char* c1 = expr_start + comma_pos[0] + 1;
                                    const char* c2 = (comma_n == 2) ? (expr_start + comma_pos[1] + 1) : NULL;

                                    size_t c0_len = (size_t)comma_pos[0];
                                    size_t c1_len = (comma_n == 2) ? (size_t)(comma_pos[1] - comma_pos[0] - 1) : (expr_len - (size_t)comma_pos[0] - 1);
                                    size_t c2_len = (comma_n == 2) ? (expr_len - (size_t)comma_pos[1] - 1) : 0;

                                    while (c0_len > 0 && (c0[c0_len - 1] == ' ' || c0[c0_len - 1] == '\t')) c0_len--;
                                    while (c1_len > 0 && (*c1 == ' ' || *c1 == '\t')) { c1++; c1_len--; }
                                    while (c1_len > 0 && (c1[c1_len - 1] == ' ' || c1[c1_len - 1] == '\t')) c1_len--;
                                    if (c2) {
                                        while (c2_len > 0 && (*c2 == ' ' || *c2 == '\t')) { c2++; c2_len--; }
                                        while (c2_len > 0 && (c2[c2_len - 1] == ' ' || c2[c2_len - 1] == '\t')) c2_len--;
                                    }

                                    fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                                    if (comma_n == 1) {
                                        fprintf(out, "{ CCClosure1 __c = %.*s; cc_nursery_spawn_closure1(__cc_nursery%d, __c, (intptr_t)(%.*s)); }\n",
                                                (int)c0_len, c0, cur_nursery_id,
                                                (int)c1_len, c1);
                                    } else {
                                        fprintf(out, "{ CCClosure2 __c = %.*s; cc_nursery_spawn_closure2(__cc_nursery%d, __c, (intptr_t)(%.*s), (intptr_t)(%.*s)); }\n",
                                                (int)c0_len, c0, cur_nursery_id,
                                                (int)c1_len, c1,
                                                (int)c2_len, c2 ? c2 : "0");
                                    }
                                    continue;
                                }
                            }
                            int looks_ident = 0;
                            if (expr_len > 0 && (isalpha((unsigned char)expr_start[0]) || expr_start[0] == '_')) {
                                looks_ident = 1;
                                for (size_t i = 1; i < expr_len; i++) {
                                    char c = expr_start[i];
                                    if (!(isalnum((unsigned char)c) || c == '_')) { looks_ident = 0; break; }
                                }
                            }
                            int looks_make = 0;
                            if (expr_len >= 15) {
                                for (size_t i = 0; i + 15 <= expr_len; i++) {
                                    if (memcmp(expr_start + i, "cc_closure0_make", 15) == 0) { looks_make = 1; break; }
                                }
                            }
                            if (looks_ident || looks_make) {
                                fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                                if (cur_nursery_id == 0) {
                                    fprintf(out, "/* TODO: spawn outside nursery */ %s", line);
                                } else {
                                    fprintf(out, "{ CCClosure0 __c = %.*s; cc_nursery_spawn_closure0(__cc_nursery%d, __c); }\n",
                                            (int)expr_len, expr_start, cur_nursery_id);
                                }
                                continue;
                            }
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

            /* Closure literal used as an expression:
               rewrite `() => { ... }` / `() => expr` to a CCClosure0 value. */
            if (closure_line_map && src_line_no > 0 && src_line_no < closure_line_cap) {
                int idx1 = closure_line_map[src_line_no];
                if (idx1 > 0 && idx1 <= closure_count) {
                    CCClosureDesc* cd = &closure_descs[idx1 - 1];
                    size_t line_len2 = strlen(line);
                    if (cd->start_col < 0 || (size_t)cd->start_col > line_len2) {
                        /* Unexpected; just pass through. */
                    } else {
                        fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                        fwrite(line, 1, (size_t)cd->start_col, out);
                        if (cd->cap_count > 0) {
                            fprintf(out, "__cc_closure_make_%d(", cd->id);
                            for (int ci = 0; ci < cd->cap_count; ci++) {
                                if (ci) fputs(", ", out);
                                int mo = (cd->cap_flags && (cd->cap_flags[ci] & 2) != 0);
                                if (mo) fputs("cc_move(", out);
                                fputs(cd->cap_names[ci] ? cd->cap_names[ci] : "0", out);
                                if (mo) fputs(")", out);
                            }
                            fputs(")", out);
                        } else {
                            fprintf(out, "__cc_closure_make_%d()", cd->id);
                        }

                        if (cd->end_line == src_line_no) {
                            if (cd->end_col >= 0 && (size_t)cd->end_col < line_len2) {
                                fputs(line + cd->end_col, out);
                            } else {
                                fputc('\n', out);
                            }
                            continue;
                        }

                        /* Multiline literal: skip until end_line, then emit suffix. */
                        fputc('\n', out);
                        while (src_line_no < cd->end_line && fgets(line, cc_line_cap, in)) {
                            src_line_no++;
                        }
                        line_len2 = strlen(line);
                        fprintf(out, "#line %d \"%s\"\n", src_line_no, src_path);
                        if (cd->end_col >= 0 && (size_t)cd->end_col < line_len2) {
                            fputs(line + cd->end_col, out);
                        } else {
                            fputc('\n', out);
                        }
                        fprintf(out, "#line %d \"%s\"\n", src_line_no + 1, src_path);
                        continue;
                    }
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
                size_t ll = strnlen(line, (size_t)cc_line_cap);
                if (buf_len + ll + 1 > buf_cap) {
                    while (buf_len + ll + 1 > buf_cap) buf_cap *= 2;
                    buf = (char*)realloc(buf, buf_cap);
                    if (!buf) continue;
                }
                memcpy(buf + buf_len, line, ll);
                buf_len += ll;
                buf[buf_len] = '\0';

                while (src_line_no < end_line && fgets(line, cc_line_cap, in)) {
                    src_line_no++;
                    ll = strnlen(line, (size_t)cc_line_cap);
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
                if (cc_ufcs_rewrite_line(line, rewritten, (size_t)cc_rewritten_cap) != 0) {
                    strncpy(rewritten, line, (size_t)cc_rewritten_cap - 1);
                    rewritten[(size_t)cc_rewritten_cap - 1] = '\0';
                }
                fputs(rewritten, out);
            } else {
                fputs(line, out);
            }
        }
        free(line);
        free(rewritten);
        free(ufcs_ml_end);
        free(ufcs_single);
        free(spawn_ml_end);
        free(spawn_argc);
        fclose(in);

        /* decl table cleanup */
        for (int d = 0; d < 256; d++) {
            for (int i = 0; i < decl_counts[d]; i++) free(decl_names[d][i]);
            free(decl_names[d]);
            for (int i = 0; i < decl_counts[d]; i++) free(decl_types[d][i]);
            free(decl_types[d]);
            free(decl_flags[d]);
        }

        if (closure_descs) {
            for (int i = 0; i < closure_count; i++) {
                for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_names[j]);
                free(closure_descs[i].cap_names);
                for (int j = 0; j < closure_descs[i].cap_count; j++) free(closure_descs[i].cap_types ? closure_descs[i].cap_types[j] : NULL);
                free(closure_descs[i].cap_types);
                free(closure_descs[i].cap_flags);
                    free(closure_descs[i].param1_name);
                    free(closure_descs[i].param0_name);
                    free(closure_descs[i].param0_type);
                    free(closure_descs[i].param1_type);
                free(closure_descs[i].body);
            }
            free(closure_descs);
        }
        free(closure_line_map);
        free(closure_protos);
        if (closure_defs && closure_defs_len > 0) {
            /* Emit closure definitions at end-of-file so global names are in scope. */
            fputs("\n/* --- CC generated closures --- */\n", out);
            fwrite(closure_defs, 1, closure_defs_len, out);
            fputs("/* --- end generated closures --- */\n", out);
        }
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

