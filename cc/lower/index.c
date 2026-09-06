/* Declaration index. See index.h for the contract.
 *
 * Everything the index knows comes from declarations: the unit's own, the
 * `.cch` headers it includes (parsed with the same parser in header mode),
 * `@typehooks` registrations, `*_DECL_UFCS(Name)` registrations, and the
 * declarations that file-scope macro invocations and generic factories
 * produce (their `#define` bodies and `@emit` templates are expanded here
 * and parsed as synthetic header units). There is no table of stdlib
 * type, function or method names in this file. */
#define _GNU_SOURCE 1
#include "index.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_INDEX_MAX_EXPANSION_DEPTH 4

/* ---- small helpers ----------------------------------------------------- */

static uint64_t cc__ix_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static CcName cc__in(CcIndex *ix, const char *s) { return cc_intern(ix->intern, s, strlen(s)); }
static CcName cc__inn(CcIndex *ix, const char *s, size_t n) { return cc_intern(ix->intern, s, n); }

static int cc__has_prefix(const char *s, const char *p) {
    size_t n = strlen(p);
    return strncmp(s, p, n) == 0;
}

static int cc__has_suffix(const char *s, const char *p) {
    size_t n = strlen(s), m = strlen(p);
    return n >= m && strcmp(s + n - m, p) == 0;
}

static const CcToken *cc__tok(const CcUnit *u, uint32_t i) {
    if (!u || !u->file || i >= u->file->n_toks) return NULL;
    return &u->file->toks[i];
}

/* Arena copy of a token's text. */
static const char *cc__tok_text(CcIndex *ix, const CcUnit *u, uint32_t i) {
    const CcToken *t = cc__tok(u, i);
    if (!t) return "";
    return cc_arena_strndup(ix->arena, u->file->src + t->off, t->len);
}

static int cc__tok_is_text(const CcUnit *u, uint32_t i, const char *s) {
    const CcToken *t = cc__tok(u, i);
    return t && cc_tok_is(u->file, t, s);
}

static CcLoc cc__span_loc(const CcUnit *u, CcSpan sp) {
    const CcToken *t = cc__tok(u, sp.first);
    CcLoc l;
    if (!t) {
        l.path = u && u->file ? u->file->path : NULL;
        l.line = 0;
        l.col = 0;
        return l;
    }
    return cc_lex_loc(u->file, t->off);
}

CcLoc cc_index_sym_loc(const CcSym *s) {
    CcLoc l = {NULL, 0, 0};
    if (!s || !s->decl || !s->unit) return l;
    return cc__span_loc(s->unit, s->decl->span);
}

/* The text of a token range, joined with a single space wherever the
 * source had whitespace. */
static void cc__span_text_buf(CcBuf *b, const CcUnit *u, uint32_t first, uint32_t last) {
    uint32_t i;
    for (i = first; i <= last && i < u->file->n_toks; i++) {
        const CcToken *t = &u->file->toks[i];
        if (t->kind == CC_TK_EOF) break;
        if (i > first && t->after_space) cc_buf_push_char(b, ' ');
        cc_buf_push(b, u->file->src + t->off, t->len);
    }
}

static const char *cc__span_text(CcIndex *ix, const CcUnit *u, uint32_t first, uint32_t last) {
    CcBuf b;
    const char *out;
    cc_buf_init(&b);
    cc__span_text_buf(&b, u, first, last);
    out = cc_arena_strdup(ix->arena, b.data);
    cc_buf_free(&b);
    return out;
}

/* Unquote a string literal token's text ("abc" -> abc; prefixes and simple
 * escapes handled). */
static const char *cc__unquote(CcIndex *ix, const char *text, size_t len) {
    CcBuf b;
    const char *out;
    size_t i = 0;
    cc_buf_init(&b);
    while (i < len && text[i] != '"') i++;
    if (i < len) i++;
    for (; i < len && text[i] != '"'; i++) {
        if (text[i] == '\\' && i + 1 < len) {
            i++;
            switch (text[i]) {
            case 'n': cc_buf_push_char(&b, '\n'); break;
            case 't': cc_buf_push_char(&b, '\t'); break;
            default: cc_buf_push_char(&b, text[i]); break;
            }
        } else {
            cc_buf_push_char(&b, text[i]);
        }
    }
    out = cc_arena_strdup(ix->arena, b.data);
    cc_buf_free(&b);
    return out;
}

static int cc__is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int cc__is_ident_char(int c) { return cc__is_ident_start(c) || (c >= '0' && c <= '9'); }

/* ---- symbol table ------------------------------------------------------ */

static CcSym *cc__sym_find_kind(const CcIndex *ix, const char *name, int kind) {
    CcSym *s;
    if (!ix->buckets) return NULL;
    for (s = ix->buckets[cc__ix_hash(name) & (ix->n_buckets - 1)]; s; s = s->next)
        if (strcmp(s->name, name) == 0 && (kind == 0 || (int)s->kind == kind)) return s;
    return NULL;
}

CcSym *cc_index_sym(const CcIndex *ix, CcName name) { return cc__sym_find_kind(ix, name, 0); }

/* A function-shaped callee: a declared function or a macro. */
static CcSym *cc__callee_sym(const CcIndex *ix, const char *name) {
    CcSym *s = cc__sym_find_kind(ix, name, CC_SYM_FUNC);
    if (s) return s;
    s = cc__sym_find_kind(ix, name, CC_SYM_MACRO);
    if (s && s->macro && s->macro->function_like) return s;
    return NULL;
}

static void cc__syms_grow(CcIndex *ix) {
    size_t ncap = ix->n_buckets ? ix->n_buckets * 2 : 1024, i;
    CcSym **nb = CC_NEW_N(ix->arena, CcSym *, ncap);
    for (i = 0; i < ix->n_buckets; i++) {
        CcSym *s = ix->buckets[i];
        while (s) {
            CcSym *next = s->next;
            size_t j = cc__ix_hash(s->name) & (ncap - 1);
            s->next = nb[j];
            nb[j] = s;
            s = next;
        }
    }
    ix->buckets = nb;
    ix->n_buckets = ncap;
}

static CcSym *cc__sym_add(CcIndex *ix, CcName name, int kind, CcDecl *decl, CcUnit *u,
                          CcType *type, int is_header, int is_definition) {
    CcSym *s = cc__sym_find_kind(ix, name, kind);
    size_t j;
    if (s) {
        /* A later declaration wins only when it carries more information:
         * a definition after a prototype, a struct body after a forward
         * declaration. */
        if (is_definition && !s->is_definition) {
            s->decl = decl;
            s->unit = u;
            s->type = type;
            s->is_header = is_header;
            s->is_definition = 1;
        }
        return s;
    }
    if (ix->syms.n * 2 >= ix->n_buckets) cc__syms_grow(ix);
    s = CC_NEW(ix->arena, CcSym);
    s->name = name;
    s->kind = kind;
    s->decl = decl;
    s->unit = u;
    s->type = type;
    s->is_header = is_header;
    s->is_definition = is_definition;
    j = cc__ix_hash(name) & (ix->n_buckets - 1);
    s->next = ix->buckets[j];
    ix->buckets[j] = s;
    CC_LIST_PUSH(ix->arena, &ix->syms, s);
    return s;
}

/* ---- type table -------------------------------------------------------- */

static CcTypeInfo *cc__type_find(const CcIndex *ix, const char *name) {
    CcTypeInfo *t;
    if (!ix->type_buckets) return NULL;
    for (t = ix->type_buckets[cc__ix_hash(name) & (ix->n_type_buckets - 1)]; t; t = t->next)
        if (strcmp(t->name, name) == 0) return t;
    return NULL;
}

CcTypeInfo *cc_index_type(const CcIndex *ix, CcName canonical) { return cc__type_find(ix, canonical); }

static void cc__types_grow(CcIndex *ix) {
    size_t ncap = ix->n_type_buckets ? ix->n_type_buckets * 2 : 256, i;
    CcTypeInfo **nb = CC_NEW_N(ix->arena, CcTypeInfo *, ncap);
    for (i = 0; i < ix->n_type_buckets; i++) {
        CcTypeInfo *t = ix->type_buckets[i];
        while (t) {
            CcTypeInfo *next = t->next;
            size_t j = cc__ix_hash(t->name) & (ncap - 1);
            t->next = nb[j];
            nb[j] = t;
            t = next;
        }
    }
    ix->type_buckets = nb;
    ix->n_type_buckets = ncap;
}

static void cc__type_resolve_hooks(CcIndex *ix, CcTypeInfo *info);
static void cc__type_instantiate(CcIndex *ix, CcTypeInfo *info);

static int cc__infer_family(CcIndex *ix, CcTypeInfo *t);

CcTypeInfo *cc_index_type_get(CcIndex *ix, CcName canonical) {
    CcTypeInfo *t = cc__type_find(ix, canonical);
    size_t j;
    if (t) return t;
    if (ix->types.n * 2 >= ix->n_type_buckets) cc__types_grow(ix);
    t = CC_NEW(ix->arena, CcTypeInfo);
    t->name = cc__in(ix, canonical);
    t->sym = cc__sym_find_kind(ix, canonical, CC_SYM_TYPE);
    j = cc__ix_hash(canonical) & (ix->n_type_buckets - 1);
    t->next = ix->type_buckets[j];
    ix->type_buckets[j] = t;
    CC_LIST_PUSH(ix->arena, &ix->types, t);
    cc__type_resolve_hooks(ix, t);
    if (!t->sym && cc__infer_family(ix, t)) cc__type_instantiate(ix, t);
    return t;
}

/* ---- primitive spellings and mangling ---------------------------------- */

/* Canonical spelling of a run of C primitive type keywords: "unsigned long
 * int" -> "unsigned long", "long int" -> "long", "int unsigned" ->
 * "unsigned int". Anything that is not entirely primitive keywords comes
 * back unchanged (with whitespace collapsed). */
static const char *cc__canon_primitive(CcIndex *ix, const char *spelling) {
    int n_long = 0, is_unsigned = 0, is_signed = 0, is_short = 0, is_char = 0, is_int = 0;
    int is_float = 0, is_double = 0, is_void = 0, is_bool = 0, other = 0, n = 0;
    CcBuf words;
    const char *p = spelling;
    const char *out;
    cc_buf_init(&words);
    while (*p) {
        const char *s;
        size_t len;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        len = (size_t)(p - s);
#define KW(k) (len == strlen(k) && memcmp(s, k, len) == 0)
        /* qualifiers are about the object, not the type's identity */
        if (KW("const") || KW("volatile") || KW("restrict") || KW("__restrict")) continue;
        n++;
        if (words.len) cc_buf_push_char(&words, ' ');
        cc_buf_push(&words, s, len);
        if (KW("long")) n_long++;
        else if (KW("unsigned")) is_unsigned = 1;
        else if (KW("signed")) is_signed = 1;
        else if (KW("short")) is_short = 1;
        else if (KW("char")) is_char = 1;
        else if (KW("int")) is_int = 1;
        else if (KW("float")) is_float = 1;
        else if (KW("double")) is_double = 1;
        else if (KW("void")) is_void = 1;
        else if (KW("_Bool") || KW("bool")) is_bool = 1;
        else other = 1;
#undef KW
    }
    if (other || n == 0) {
        out = cc_arena_strdup(ix->arena, words.data);
        cc_buf_free(&words);
        return out;
    }
    cc_buf_free(&words);
    if (is_void) return "void";
    if (is_bool) return "bool";
    if (is_float) return "float";
    if (is_double) return n_long ? "long double" : "double";
    if (is_char) return is_unsigned ? "unsigned char" : is_signed ? "signed char" : "char";
    if (is_short) return is_unsigned ? "unsigned short" : "short";
    if (n_long >= 2) return is_unsigned ? "unsigned long long" : "long long";
    if (n_long == 1) return is_unsigned ? "unsigned long" : "long";
    (void)is_int;
    return is_unsigned ? "unsigned int" : "int";
}

/* Is the spelling made only of C primitive type keywords? */
static int cc__primitive_words(const char *s) {
    const char *p = s;
    int n = 0;
    while (*p) {
        const char *w;
        size_t len;
        while (*p == ' ') p++;
        if (!*p) break;
        w = p;
        while (*p && *p != ' ') p++;
        len = (size_t)(p - w);
#define KW(k) (len == strlen(k) && memcmp(w, k, len) == 0)
        if (!(KW("long") || KW("unsigned") || KW("signed") || KW("short") || KW("char") || KW("int") || KW("float") ||
              KW("double") || KW("void") || KW("_Bool") || KW("bool")))
            return 0;
#undef KW
        n++;
    }
    return n > 0;
}

/* Identifier-safe token for a canonical type spelling: spaces -> `_`,
 * `*` -> `ptr`, other punctuation -> `_`, trailing `_` trimmed. This is the
 * recipe of GENERIC_MANGLING.md and of the current Result spec mangler. */
static const char *cc__mangle(CcIndex *ix, const char *s) {
    CcBuf b;
    const char *out;
    size_t i, n = strlen(s);
    cc_buf_init(&b);
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            size_t k = i + 1;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k < n && s[k] == '*') continue;
            if (b.len && b.data[b.len - 1] != '_') cc_buf_push_char(&b, '_');
        } else if (c == '*') {
            cc_buf_push_str(&b, "ptr");
        } else if (c == '[' && i + 2 < n && s[i + 1] == ':' && s[i + 2] == ']') {
            cc_buf_push_str(&b, "slice");
            i += 2;
        } else if (cc__is_ident_char((unsigned char)c)) {
            cc_buf_push_char(&b, c);
        } else {
            if (b.len && b.data[b.len - 1] != '_') cc_buf_push_char(&b, '_');
        }
    }
    while (b.len && b.data[b.len - 1] == '_') b.data[--b.len] = 0;
    out = cc_arena_strdup(ix->arena, b.data);
    cc_buf_free(&b);
    return out;
}

CcName cc_index_snake(CcIndex *ix, CcName type_name) {
    CcBuf b;
    const char *t = type_name;
    size_t n = strlen(t), i, body_start = 0;
    const char *out;
    cc_buf_init(&b);
    if (n > 2 && t[0] == 'C' && t[1] == 'C' && t[2] >= 'A' && t[2] <= 'Z') {
        cc_buf_push_str(&b, "cc_");
        body_start = 2;
    }
    for (i = body_start; i < n; i++) {
        char c = t[i];
        int upper = c >= 'A' && c <= 'Z';
        if (c == ' ' || c == '\t') {
            if (b.len && b.data[b.len - 1] != '_') cc_buf_push_char(&b, '_');
            continue;
        }
        if (upper && i > body_start && b.len && b.data[b.len - 1] != '_') cc_buf_push_char(&b, '_');
        cc_buf_push_char(&b, upper ? (char)(c + ('a' - 'A')) : c);
    }
    while (b.len && b.data[b.len - 1] == '_') b.data[--b.len] = 0;
    out = cc__in(ix, b.data);
    cc_buf_free(&b);
    return out;
}

/* ---- canonical type spelling ------------------------------------------- */

static int cc__is_char_family(const char *canon) {
    return strcmp(canon, "char") == 0 || strcmp(canon, "unsigned char") == 0 ||
           strcmp(canon, "signed char") == 0;
}

static const char *cc__type_arg_spelling(CcIndex *ix, const CcType *t);
static const char *cc__macro_expand(CcIndex *ix, const CcMacroDef *m, const char **args, size_t nargs);
static CcUnit *cc__parse_text(CcIndex *ix, const char *path, const char *text, CcUnitMode mode, CcDiag *diag);

/* The instance prefix a generic family uses: the family name, except that
 * the stdlib's `Vec` family names its instances `CCVec_<T>` (vec.cch: "The
 * concrete C name is CCVec_<T>"). Nothing else in the index maps a family
 * name; see INDEX_GAPS.md for the proposal to make that a declaration. */
static const char *cc__family_prefix(const char *family) {
    if (strcmp(family, "Vec") == 0) return "CCVec";
    return family;
}

CcName cc_index_canon(CcIndex *ix, const CcType *t) {
    if (!t) return cc__in(ix, "?");
    switch (t->kind) {
    case CC_T_NAMED: {
        const char *n = t->name ? t->name : "?";
        if (t->is_struct_kw && !cc__has_prefix(n, "struct ")) return cc__in(ix, cc_arena_printf(ix->arena, "struct %s", n));
        if (t->is_union_kw && !cc__has_prefix(n, "union ")) return cc__in(ix, cc_arena_printf(ix->arena, "union %s", n));
        if (t->is_enum_kw && !cc__has_prefix(n, "enum ")) return cc__in(ix, cc_arena_printf(ix->arena, "enum %s", n));
        return cc__in(ix, cc__canon_primitive(ix, n));
    }
    case CC_T_POINTER: {
        const char *base = cc_index_canon(ix, t->base);
        return cc__in(ix, cc_arena_printf(ix->arena, "%s*", base));
    }
    case CC_T_ARRAY: {
        const char *base = cc_index_canon(ix, t->base);
        return cc__in(ix, cc_arena_printf(ix->arena, "%s[]", base));
    }
    case CC_T_FUNC: {
        const char *base = cc_index_canon(ix, t->base);
        return cc__in(ix, cc_arena_printf(ix->arena, "%s(*)()", base));
    }
    case CC_T_STRUCT:
        if (t->name) return cc__in(ix, cc_arena_printf(ix->arena, "%s %s", t->is_union ? "union" : "struct", t->name));
        return cc__in(ix, t->is_union ? "union <anonymous>" : "struct <anonymous>");
    case CC_T_ENUM:
        if (t->name) return cc__in(ix, cc_arena_printf(ix->arena, "enum %s", t->name));
        return cc__in(ix, "enum <anonymous>");
    case CC_T_TYPEOF:
        if (t->typeof_type) return cc_index_canon(ix, t->typeof_type);
        return cc__in(ix, "__typeof__");
    case CC_T_ATOMIC:
        return cc_index_canon(ix, t->base);
    case CC_T_RESULT: {
        CcTypeInfo *info = cc_index_result(ix, t->base, t->err, t->optional);
        return info->name;
    }
    case CC_T_SLICE: {
        const char *base = cc_index_canon(ix, t->base);
        CcTypeInfo *info;
        CcName name;
        if (cc__is_char_family(base)) return cc__in(ix, t->unique ? "CCSliceUnique" : "CCSlice");
        name = cc__in(ix, cc_arena_printf(ix->arena, "CCSlice_%s", cc__mangle(ix, base)));
        info = cc_index_type_get(ix, name);
        if (!info->family) {
            info->family = cc__in(ix, "CCSlice");
            CC_LIST_PUSH(ix->arena, &info->targs, t->base);
        }
        cc__type_instantiate(ix, info);
        return name;
    }
    case CC_T_CHAN: {
        const char *base = cc_index_canon(ix, t->base);
        return cc__in(ix, cc_arena_printf(ix->arena, "%s_%s", t->dir == '<' ? "CCChanRx" : "CCChanTx",
                                          cc__mangle(ix, base)));
    }
    case CC_T_GENERIC: {
        CcBuf b;
        size_t i;
        CcName name;
        CcTypeInfo *info;
        cc_buf_init(&b);
        cc_buf_push_str(&b, cc__family_prefix(t->name ? t->name : "?"));
        for (i = 0; i < t->args.n; i++) {
            cc_buf_push_char(&b, '_');
            cc_buf_push_str(&b, cc__mangle(ix, cc__type_arg_spelling(ix, t->args.items[i])));
        }
        name = cc__in(ix, b.data);
        cc_buf_free(&b);
        info = cc_index_type_get(ix, name);
        if (!info->family) {
            info->family = t->name;
            for (i = 0; i < t->args.n; i++) CC_LIST_PUSH(ix->arena, &info->targs, t->args.items[i]);
        }
        cc__type_instantiate(ix, info);
        return name;
    }
    case CC_T_SCOPED:
    case CC_T_AUTO:
        return cc__in(ix, t->name ? t->name : "?");
    case CC_T_MACRO: {
        /* NAME(args) as a type: expand the macro when it is known and read the type back */
        CcSym *ms = t->name ? cc__sym_find_kind(ix, t->name, CC_SYM_MACRO) : NULL;
        if (ms && ms->macro && ms->macro->function_like) {
            const char **args = CC_NEW_N(ix->arena, const char *, t->args.n + 1);
            size_t i;
            const char *text;
            CcDiag scratch;
            CcUnit *u;
            for (i = 0; i < t->args.n; i++) args[i] = cc__type_arg_spelling(ix, t->args.items[i]);
            text = cc_arena_printf(ix->arena, "%s __cc_probe;", cc__macro_expand(ix, ms->macro, args, t->args.n));
            cc_diag_init(&scratch, ix->arena);
            u = cc__parse_text(ix, "<macro type>", text, CC_MODE_HEADER, &scratch);
            if (!scratch.n_errors && u->decls.n == 1 && u->decls.items[0]->type && u->decls.items[0]->type != t)
                return cc_index_canon(ix, u->decls.items[0]->type);
        }
        return cc__in(ix, cc_arena_printf(ix->arena, "%s(...)", t->name ? t->name : "?"));
    }
    case CC_T_VALUE:
        return cc__in(ix, cc__type_arg_spelling(ix, t));
    }
    return cc__in(ix, "?");
}

/* A type argument as the factory receives it: a C spelling. A numeric
 * argument (`SmallVec::[int, 8]`) is its digits. */
static const char *cc__type_arg_spelling(CcIndex *ix, const CcType *t) {
    if (!t) return "?";
    if (t->kind == CC_T_NAMED) return cc__canon_primitive(ix, t->name ? t->name : "?");
    if (t->kind == CC_T_VALUE) {
        /* a value argument (`SmallVec::[int, 8]`): its source text, read when
         * the unit was added (cc__value_on_type). A nested instance
         * (`Map::[int, Vec::[int]]`) arrives here as a value: read it as a type. */
        if (t->name && strstr(t->name, "::[")) {
            CcDiag scratch;
            CcUnit *u;
            cc_diag_init(&scratch, ix->arena);
            u = cc__parse_text(ix, "<type argument>", cc_arena_printf(ix->arena, "%s __cc_probe;", t->name), CC_MODE_HEADER, &scratch);
            if (!scratch.n_errors && u->decls.n == 1 && u->decls.items[0]->type && u->decls.items[0]->type->kind != CC_T_VALUE)
                return cc_index_canon(ix, u->decls.items[0]->type);
        }
        return t->name ? t->name : "value";
    }
    if (t->kind == CC_T_SLICE) {
        const char *base = cc_index_canon(ix, t->base);
        if (cc__is_char_family(base)) return "CCSlice";
    }
    return cc_index_canon(ix, t);
}

/* ---- Result specs -------------------------------------------------------- */

CcTypeInfo *cc_index_result(CcIndex *ix, CcType *value, CcType *err, int optional) {
    const char *mv = cc__mangle(ix, cc_index_canon(ix, value));
    const char *me = cc__mangle(ix, cc_index_canon(ix, err));
    CcName name = cc__in(ix, cc_arena_printf(ix->arena, "CCResult_%s_%s", mv, me));
    CcTypeInfo *info = cc_index_type_get(ix, name);
    size_t i;
    int listed = 0;
    if (!info->is_result) {
        info->is_result = 1;
        info->result_value = value;
        info->result_err = err;
        info->result_optional = optional;
    } else if (optional) {
        info->result_optional = 1;
    }
    for (i = 0; i < ix->result_specs.n; i++)
        if (ix->result_specs.items[i] == info) listed = 1;
    if (!listed) CC_LIST_PUSH(ix->arena, &ix->result_specs, info);
    cc__type_instantiate(ix, info);
    return info;
}

/* A CcType for a C spelling seen in a macro argument ("CCDirIter*", "int"). */
static CcType *cc__type_from_text(CcIndex *ix, const char *text) {
    size_t n = strlen(text);
    CcSpan sp = {0, 0};
    CcType *t;
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t')) n--;
    if (n && text[n - 1] == '*') {
        CcType *p = cc_type_new(ix->arena, CC_T_POINTER, sp);
        p->base = cc__type_from_text(ix, cc_arena_strndup(ix->arena, text, n - 1));
        return p;
    }
    t = cc_type_new(ix->arena, CC_T_NAMED, sp);
    if (cc__has_prefix(text, "struct ")) t->is_struct_kw = 1;
    if (cc__has_prefix(text, "union ")) t->is_union_kw = 1;
    if (cc__has_prefix(text, "enum ")) t->is_enum_kw = 1;
    t->name = cc__in(ix, cc__canon_primitive(ix, cc_arena_strndup(ix->arena, text, n)));
    return t;
}

static void cc__result_declared(CcIndex *ix, CcUnit *u, CcDecl *d, int is_header, const char *name,
                                const char *value_text, const char *err_text) {
    CcTypeInfo *info = cc_index_type_get(ix, cc__in(ix, name));
    CcLoc loc = cc__span_loc(u, d->span);
    info->is_result = 1;
    if (!info->result_value) info->result_value = value_text ? cc__type_from_text(ix, value_text) : NULL;
    if (!info->result_err) info->result_err = cc__type_from_text(ix, err_text);
    if (is_header) info->result_declared_in_header = 1;
    info->result_decl_unit = u;
    info->result_decl_line = loc.line;
}

/* ---- `#define` parsing and expansion ----------------------------------- */

/* Body text of a PP line with continuations joined and comments removed. */
static char *cc__pp_clean(CcIndex *ix, const char *text, size_t len) {
    CcBuf b;
    size_t i = 0;
    char *out;
    cc_buf_init(&b);
    while (i < len) {
        char c = text[i];
        if (c == '\\' && i + 1 < len && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < len && text[i + 2] == '\n'))) {
            i += text[i + 1] == '\r' ? 3 : 2;
            cc_buf_push_char(&b, ' ');
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            cc_buf_push_char(&b, ' ');
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            while (i < len && text[i] != '\n') i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            cc_buf_push_char(&b, c);
            i++;
            while (i < len && text[i] != q) {
                if (text[i] == '\\' && i + 1 < len) { cc_buf_push_char(&b, text[i]); i++; }
                cc_buf_push_char(&b, text[i]);
                i++;
            }
            if (i < len) { cc_buf_push_char(&b, text[i]); i++; }
            continue;
        }
        cc_buf_push_char(&b, c);
        i++;
    }
    out = cc_arena_strdup(ix->arena, b.data);
    cc_buf_free(&b);
    return out;
}

static CcMacroDef *cc__parse_define(CcIndex *ix, CcUnit *u, CcDecl *d) {
    const CcToken *t = cc__tok(u, d->tok);
    const char *text, *p, *name_s;
    char *clean;
    CcMacroDef *m;
    if (!t) return NULL;
    clean = cc__pp_clean(ix, u->file->src + t->off, t->len);
    p = clean;
    if (*p != '#') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (!cc__has_prefix(p, "define")) return NULL;
    p += 6;
    if (!(*p == ' ' || *p == '\t')) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (!cc__is_ident_start((unsigned char)*p)) return NULL;
    name_s = p;
    while (cc__is_ident_char((unsigned char)*p)) p++;
    m = CC_NEW(ix->arena, CcMacroDef);
    m->name = cc__inn(ix, name_s, (size_t)(p - name_s));
    m->decl = d;
    m->unit = u;
    if (*p == '(') {
        CC_LIST(const char) params = {0};
        p++;
        m->function_like = 1;
        for (;;) {
            const char *s;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ')') { p++; break; }
            if (!*p) break;
            if (cc__has_prefix(p, "...")) {
                m->variadic = 1;
                p += 3;
                continue;
            }
            s = p;
            while (cc__is_ident_char((unsigned char)*p)) p++;
            if (p == s) { p++; continue; }
            CC_LIST_PUSH(ix->arena, &params, cc_arena_strndup(ix->arena, s, (size_t)(p - s)));
            while (*p == ' ' || *p == '\t') p++;
            if (cc__has_prefix(p, "...")) { m->variadic = 1; p += 3; }
            if (*p == ',') p++;
        }
        m->params = params.items;
        m->n_params = params.n;
    }
    while (*p == ' ' || *p == '\t') p++;
    text = p;
    m->body = cc_arena_strdup(ix->arena, text);
    return m;
}

static int cc__macro_param_index(const CcMacroDef *m, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < m->n_params; i++)
        if (strlen(m->params[i]) == n && memcmp(m->params[i], s, n) == 0) return (int)i;
    return -1;
}

/* Expand a function-like macro body with the given argument texts. `#x`
 * stringifies, `a ## b` pastes, `__VA_ARGS__` is the rest of the list. */
static const char *cc__macro_expand(CcIndex *ix, const CcMacroDef *m, const char **args, size_t nargs) {
    CC_LIST(const char) pieces = {0};
    CC_LIST(const char) glue = {0}; /* "" = space, "#" = paste */
    const char *p = m->body;
    CcBuf out;
    size_t i;
    int paste_next = 0, stringify_next = 0;
    const char *result;
    while (*p) {
        const char *s;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        s = p;
        if (p[0] == '#' && p[1] == '#') {
            p += 2;
            paste_next = 1;
            continue;
        }
        if (p[0] == '#' && m->function_like) {
            p++;
            stringify_next = 1;
            continue;
        }
        if (cc__is_ident_start((unsigned char)*p)) {
            int pi;
            const char *val;
            while (cc__is_ident_char((unsigned char)*p)) p++;
            pi = cc__macro_param_index(m, s, (size_t)(p - s));
            if (pi >= 0 && (size_t)pi < nargs) {
                val = args[pi];
            } else if (pi >= 0) {
                val = "";
            } else if ((size_t)(p - s) == 11 && memcmp(s, "__VA_ARGS__", 11) == 0) {
                CcBuf va;
                size_t k;
                cc_buf_init(&va);
                for (k = m->n_params; k < nargs; k++) {
                    if (k > m->n_params) cc_buf_push_str(&va, ", ");
                    cc_buf_push_str(&va, args[k]);
                }
                val = cc_arena_strdup(ix->arena, va.data);
                cc_buf_free(&va);
            } else {
                val = cc_arena_strndup(ix->arena, s, (size_t)(p - s));
            }
            if (stringify_next) {
                CcBuf q;
                const char *c;
                cc_buf_init(&q);
                cc_buf_push_char(&q, '"');
                for (c = val; *c; c++) {
                    if (*c == '"' || *c == '\\') cc_buf_push_char(&q, '\\');
                    cc_buf_push_char(&q, *c);
                }
                cc_buf_push_char(&q, '"');
                val = cc_arena_strdup(ix->arena, q.data);
                cc_buf_free(&q);
                stringify_next = 0;
            }
            CC_LIST_PUSH(ix->arena, &pieces, val);
            CC_LIST_PUSH(ix->arena, &glue, paste_next ? "#" : "");
            paste_next = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p) p++;
        } else {
            /* one punctuation character (multi-character operators stay
             * adjacent because pieces are joined with a space only where the
             * body had one) */
            const char *q = p + 1;
            while (*q && !(*q == ' ' || *q == '\t') && !cc__is_ident_char((unsigned char)*q) && *q != '"' &&
                   *q != '\'' && *q != '#')
                q++;
            p = q;
        }
        CC_LIST_PUSH(ix->arena, &pieces, cc_arena_strndup(ix->arena, s, (size_t)(p - s)));
        CC_LIST_PUSH(ix->arena, &glue, paste_next ? "#" : "");
        paste_next = 0;
    }
    cc_buf_init(&out);
    for (i = 0; i < pieces.n; i++) {
        if (i > 0 && glue.items[i][0] != '#') cc_buf_push_char(&out, ' ');
        cc_buf_push_str(&out, pieces.items[i]);
    }
    result = cc_arena_strdup(ix->arena, out.data);
    cc_buf_free(&out);
    return result;
}

/* ---- synthetic units: expansions parsed as headers --------------------- */

typedef struct CcIncludeRef {
    const char *path;
    int quoted;
    uint32_t off;
} CcIncludeRef;
static size_t cc__scan_includes(CcIndex *ix, const CcLexFile *f, CcIncludeRef **out);
static const char *cc__resolve_include(CcIndex *ix, const char *from_path, const CcIncludeRef *r,
                                       const CcIndexOpts *opts, CcBuf *searched);
static void cc__load_header(CcIndex *ix, const char *path, const CcIndexOpts *opts, const CcParseOpts *popts);

const char **cc_index_known_types(CcIndex *ix) {
    const char **list = CC_NEW_N(ix->arena, const char *, ix->typedef_names.n + 1);
    size_t i;
    for (i = 0; i < ix->typedef_names.n; i++) list[i] = ix->typedef_names.items[i];
    list[i] = NULL;
    return list;
}

static CcUnit *cc__parse_text(CcIndex *ix, const char *path, const char *text, CcUnitMode mode, CcDiag *diag) {
    CcParseOpts po;
    CcLexFile *f;
    memset(&po, 0, sizeof po);
    po.mode = mode;
    po.known_types = cc_index_known_types(ix);
    f = cc_lex(ix->arena, diag, path, cc_arena_strdup(ix->arena, text), strlen(text));
    return cc_parse(ix->arena, diag, ix->intern, f, &po);
}

/* Parse `text` (an expansion) as a header unit and index it. Parse errors
 * inside an expansion are counted, not reported: the expansion is stdlib
 * text the user did not write, and a construct the parser does not model
 * there is a parser gap, not a user error. */
static void cc__index_expansion(CcIndex *ix, const char *label, const char *text, int is_header) {
    CcDiag scratch;
    CcUnit *u;
    if (ix->expansion_depth >= CC_INDEX_MAX_EXPANSION_DEPTH) return;
    cc_diag_init(&scratch, ix->arena);
    ix->n_expansions++;
    if (strstr(text, "#include")) {
        /* a factory template may include a lowered header (`<ccc/std/map.h>`):
         * its source is the `.cch` of the same name */
        CcLexFile *f = cc_lex(ix->arena, &scratch, label, cc_arena_strdup(ix->arena, text), strlen(text));
        CcIncludeRef *refs = NULL;
        size_t n = cc__scan_includes(ix, f, &refs), i;
        for (i = 0; i < n; i++) {
            CcBuf searched;
            const char *found;
            size_t pl = strlen(refs[i].path);
            if (!(pl > 2 && strcmp(refs[i].path + pl - 2, ".h") == 0 && cc__has_prefix(refs[i].path, "ccc/"))) continue;
            refs[i].path = cc_arena_printf(ix->arena, "%.*s.cch", (int)(pl - 2), refs[i].path);
            cc_buf_init(&searched);
            found = cc__resolve_include(ix, label, &refs[i], &ix->opts, &searched);
            cc_buf_free(&searched);
            if (found) cc__load_header(ix, found, &ix->opts, NULL);
        }
    }
    u = cc__parse_text(ix, label, text, CC_MODE_HEADER, &scratch);
    ix->n_expansion_errors += scratch.n_errors;
    if (scratch.n_errors && getenv("CC_INDEX_DEBUG_EXPANSIONS")) {
        fprintf(stderr, "--- expansion %s\n%s\n", label, text);
        cc_diag_print(&scratch, stderr);
    }
    ix->expansion_depth++;
    cc_index_add_unit(ix, u, is_header);
    ix->expansion_depth--;
}

/* ---- file-scope macro invocations -------------------------------------- */

/* Split the arguments of `NAME(a, b, c)` in a token range into texts. */
static size_t cc__call_args(CcIndex *ix, const CcUnit *u, CcSpan sp, const char ***out_args, CcName *out_name) {
    uint32_t i = sp.first;
    int depth = 0;
    uint32_t arg_first = 0;
    CC_LIST(const char) args = {0};
    const CcToken *t = cc__tok(u, i);
    *out_name = NULL;
    *out_args = NULL;
    if (!t || t->kind != CC_TK_IDENT) return 0;
    *out_name = cc__in(ix, cc__tok_text(ix, u, i));
    i++;
    t = cc__tok(u, i);
    if (!t || !cc_tok_is_punct(t, CC_P_LPAREN)) return 0;
    i++;
    arg_first = i;
    for (; i <= sp.last; i++) {
        t = cc__tok(u, i);
        if (!t || t->kind == CC_TK_EOF) break;
        if (t->kind == CC_TK_PUNCT) {
            if (t->punct == CC_P_LPAREN || t->punct == CC_P_LBRACKET || t->punct == CC_P_LBRACE) depth++;
            else if (t->punct == CC_P_RPAREN || t->punct == CC_P_RBRACKET || t->punct == CC_P_RBRACE) {
                if (depth == 0) {
                    if (i > arg_first || args.n) CC_LIST_PUSH(ix->arena, &args, i > arg_first ? cc__span_text(ix, u, arg_first, i - 1) : "");
                    break;
                }
                depth--;
            } else if (t->punct == CC_P_COMMA && depth == 0) {
                CC_LIST_PUSH(ix->arena, &args, i > arg_first ? cc__span_text(ix, u, arg_first, i - 1) : "");
                arg_first = i + 1;
            }
        }
    }
    *out_args = args.items;
    return args.n;
}

static void cc__macro_call(CcIndex *ix, CcUnit *u, CcDecl *d, int is_header) {
    const char **args = NULL;
    CcName name = NULL;
    size_t nargs = cc__call_args(ix, u, d->span, &args, &name);
    CcSym *ms;
    CcMacroDef *m;
    if (!name) return;
    if (strcmp(name, "CC_DECL_RESULT_SPEC") == 0 && nargs == 3) {
        cc__result_declared(ix, u, d, is_header, args[0], args[1], args[2]);
    } else if (strcmp(name, "CC_DECL_RESULT_SPEC_VOID") == 0 && nargs == 2) {
        cc__result_declared(ix, u, d, is_header, args[0], NULL, args[1]);
    } else if (cc__has_suffix(name, "_DECL_UFCS") && nargs == 1) {
        CcTypeInfo *info = cc_index_type_get(ix, cc__in(ix, args[0]));
        info->ufcs_registered_by = name;
    }
    ms = cc__sym_find_kind(ix, name, CC_SYM_MACRO);
    if (!ms || !ms->macro) return;
    m = ms->macro;
    if (!m->function_like) return;
    if (nargs < m->n_params) return;
    {
        CcLoc loc = cc__span_loc(u, d->span);
        const char *label = cc_arena_printf(ix->arena, "%s:%u <%s expansion>", loc.path ? loc.path : "?", loc.line, name);
        if (cc__has_prefix(name, "CC_DECL_RESULT_SPEC") && nargs >= 1) {
            CcTypeInfo *info = cc__type_find(ix, args[0]);
            if (info) info->instantiated = 1; /* this expansion declares the methods */
        }
        cc__index_expansion(ix, label, cc__macro_expand(ix, m, args, nargs), is_header);
    }
}

const char *cc_index_expand_call(CcIndex *ix, const CcUnit *u, CcSpan sp) {
    const char **args = NULL;
    CcName name = NULL;
    size_t nargs = cc__call_args(ix, u, sp, &args, &name);
    CcSym *ms;
    if (!name) return NULL;
    ms = cc__sym_find_kind(ix, name, CC_SYM_MACRO);
    if (!ms || !ms->macro || !ms->macro->function_like || nargs < ms->macro->n_params) return NULL;
    return cc__macro_expand(ix, ms->macro, args, nargs);
}

/* ---- generic factories ---------------------------------------------------- */

typedef struct CcSlotBind {
    const char *local;
    int arg;
    struct CcSlotBind *next;
} CcSlotBind;

/* Which factory local holds which type argument: `CCSlice t = arg(0);`,
 * `memcpy(k, arg(0).ptr, ...)`. Every local named in a statement that
 * mentions `arg(N)` or `type_args.items[N]` is bound to N. */
static void cc__factory_bindings(CcIndex *ix, const CcUnit *u, const CcStmt *body, CcSlotBind **out) {
    size_t i;
    if (!body || body->kind != CC_S_BLOCK) return;
    for (i = 0; i < body->stmts.n; i++) {
        const CcStmt *s = body->stmts.items[i];
        uint32_t k;
        int argn = -1;
        CC_LIST(const char) locals = {0};
        if (s->kind == CC_S_BLOCK) {
            cc__factory_bindings(ix, u, s, out);
            continue;
        }
        for (k = s->span.first; k <= s->span.last; k++) {
            const CcToken *t = cc__tok(u, k);
            if (!t || t->kind != CC_TK_IDENT) continue;
            if (cc_tok_is(u->file, t, "arg") && cc__tok_is_text(u, k + 1, "(")) {
                const CcToken *n = cc__tok(u, k + 2);
                if (n && n->kind == CC_TK_NUMBER) argn = atoi(u->file->src + n->off);
            } else if (cc_tok_is(u->file, t, "type_args") && cc__tok_is_text(u, k + 1, ".") &&
                       cc__tok_is_text(u, k + 2, "items") && cc__tok_is_text(u, k + 3, "[")) {
                const CcToken *n = cc__tok(u, k + 4);
                if (n && n->kind == CC_TK_NUMBER) argn = atoi(u->file->src + n->off);
            }
        }
        if (argn < 0) continue;
        if (s->kind == CC_S_DECL && s->decl && s->decl->name) CC_LIST_PUSH(ix->arena, &locals, s->decl->name);
        for (k = s->span.first; k <= s->span.last; k++) {
            const CcToken *t = cc__tok(u, k);
            const CcSlotBind *b;
            const char *txt;
            if (!t || t->kind != CC_TK_IDENT) continue;
            txt = cc__tok_text(ix, u, k);
            for (b = *out; b; b = b->next)
                if (strcmp(b->local, txt) == 0) break;
            if (b) continue;
            /* a local declared earlier in the body */
            {
                size_t j;
                int declared = 0;
                for (j = 0; j < body->stmts.n; j++) {
                    const CcStmt *ds = body->stmts.items[j];
                    if (ds->kind == CC_S_DECL && ds->decl && ds->decl->name && strcmp(ds->decl->name, txt) == 0) declared = 1;
                }
                if (declared) CC_LIST_PUSH(ix->arena, &locals, txt);
            }
        }
        {
            size_t j;
            for (j = 0; j < locals.n; j++) {
                CcSlotBind *b = CC_NEW(ix->arena, CcSlotBind);
                b->local = locals.items[j];
                b->arg = argn;
                b->next = *out;
                *out = b;
            }
        }
    }
}

/* Substitute the `${...}` slots of a template's text for an instance. */
static const char *cc__template_instance(CcIndex *ix, const char *text, size_t len, CcTypeInfo *info,
                                         const CcSlotBind *binds, const char **args, size_t nargs) {
    CcBuf b;
    size_t i = 0;
    const char *out;
    cc_buf_init(&b);
    while (i < len) {
        if (text[i] == '$' && i + 1 < len && text[i + 1] == '{') {
            size_t j = i + 2, depth = 1, s;
            const char *inner;
            size_t inner_len;
            while (j < len && depth) {
                if (text[j] == '{') depth++;
                else if (text[j] == '}') depth--;
                if (depth) j++;
            }
            s = i + 2;
            inner = text + s;
            inner_len = j - s;
            while (inner_len && (inner[0] == ' ' || inner[0] == '\t')) { inner++; inner_len--; }
            while (inner_len && (inner[inner_len - 1] == ' ' || inner[inner_len - 1] == '\t')) inner_len--;
            if (inner_len == 7 && memcmp(inner, "mangled", 7) == 0) {
                cc_buf_push_str(&b, info->name);
            } else if (inner_len > 4 && memcmp(inner, "arg(", 4) == 0) {
                int n = atoi(inner + 4);
                if (n >= 0 && (size_t)n < nargs) cc_buf_push_str(&b, args[n]);
                else cc_buf_push(&b, inner, inner_len);
            } else if (inner_len > 12 && memcmp(inner, "arg_mangled(", 12) == 0) {
                /* the identifier-safe spelling of a type argument */
                int n = atoi(inner + 12);
                if (n >= 0 && (size_t)n < nargs) cc_buf_push_str(&b, cc__mangle(ix, args[n]));
                else cc_buf_push(&b, inner, inner_len);
            } else if (inner_len > 16 && memcmp(inner, "type_args.items[", 16) == 0) {
                int n = atoi(inner + 16);
                if (n >= 0 && (size_t)n < nargs) cc_buf_push_str(&b, args[n]);
                else cc_buf_push(&b, inner, inner_len);
            } else {
                const CcSlotBind *bd;
                int found = 0;
                for (bd = binds; bd; bd = bd->next) {
                    if (strlen(bd->local) == inner_len && memcmp(bd->local, inner, inner_len) == 0) {
                        if (bd->arg >= 0 && (size_t)bd->arg < nargs) cc_buf_push_str(&b, args[bd->arg]);
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    /* a slot that is the whole line holds computed text (`${fwd}`):
                     * drop it; inside a line it is a name the factory computed
                     * (`${hash}`): keep the slot's spelling as an identifier */
                    size_t ls = i, le = j + 1;
                    int alone = 1;
                    while (ls > 0 && text[ls - 1] != '\n') { if (text[ls - 1] != ' ' && text[ls - 1] != '\t') alone = 0; ls--; }
                    while (le < len && text[le] != '\n') { if (text[le] != ' ' && text[le] != '\t') alone = 0; le++; }
                    if (!alone) cc_buf_push(&b, inner, inner_len);
                }
            }
            i = j + 1;
            continue;
        }
        cc_buf_push_char(&b, text[i]);
        i++;
    }
    out = cc_arena_strdup(ix->arena, b.data);
    cc_buf_free(&b);
    return out;
}

/* Is `text` the mangled spelling of a type the index can name: a declared
 * type, a primitive, a `...ptr` of one, or a nested family instance? */
static CcType *cc__unmangle_arg(CcIndex *ix, const char *text, int depth);

static int cc__split_args(CcIndex *ix, const char *rest, size_t arity, CcTypeList *out, int depth) {
    const char *p;
    if (arity == 1) {
        CcType *t = cc__unmangle_arg(ix, rest, depth);
        if (!t) return 0;
        CC_LIST_PUSH(ix->arena, out, t);
        return 1;
    }
    for (p = strchr(rest, '_'); p; p = strchr(p + 1, '_')) {
        CcType *first = cc__unmangle_arg(ix, cc_arena_strndup(ix->arena, rest, (size_t)(p - rest)), depth);
        CcTypeList tail = {0};
        size_t i;
        if (!first) continue;
        if (!cc__split_args(ix, p + 1, arity - 1, &tail, depth)) continue;
        CC_LIST_PUSH(ix->arena, out, first);
        for (i = 0; i < tail.n; i++) CC_LIST_PUSH(ix->arena, out, tail.items[i]);
        return 1;
    }
    return 0;
}

static CcType *cc__unmangle_arg(CcIndex *ix, const char *text, int depth) {
    size_t n = strlen(text), f;
    CcSpan sp = {0, 0};
    if (!n || depth > 4) return NULL;
    /* a declared or primitive type name */
    if (cc__sym_find_kind(ix, text, CC_SYM_TYPE) || cc__primitive_words(text) ||
        (n > 2 && strcmp(text + n - 2, "_t") == 0 && !strchr(text, ' ')) /* a C typedef (<stdint.h>, <stddef.h>) */ ||
        strcmp(text, "long_long") == 0 || strcmp(text, "unsigned_long") == 0 || strcmp(text, "unsigned_char") == 0 ||
        strcmp(text, "unsigned_int") == 0 || strcmp(text, "unsigned_long_long") == 0 || strcmp(text, "unsigned_short") == 0) {
        CcType *t = cc_type_new(ix->arena, CC_T_NAMED, sp);
        const char *sp2 = cc_arena_strdup(ix->arena, text);
        char *q;
        /* only a multi-word primitive spelling (`long_long`) has `_` for a space */
        if (!cc__sym_find_kind(ix, text, CC_SYM_TYPE) && !(n > 2 && strcmp(text + n - 2, "_t") == 0))
            for (q = (char *)sp2; *q; q++) if (*q == '_') *q = ' ';
        t->name = cc__in(ix, cc__canon_primitive(ix, sp2));
        return t;
    }
    /* a nested instance: Family_args (before the `ptr` reading, so that
     * `Map_int_intptr` is Map::[int, int*] and not Map::[int, int]*) */
    for (f = 0; f < ix->factories.n; f++) {
        CcDecl *d = ix->factories.items[f];
        const char *prefix = d->name ? cc__family_prefix(d->name) : NULL;
        size_t arity = 1;
        CcTypeList args = {0};
        size_t i;
        if (!prefix || d->factory_extend || !cc__has_prefix(text, prefix) || text[strlen(prefix)] != '_') continue;
        if (d->factory_arity && d->factory_arity->kind == CC_E_NUMBER && ix->factory_units.items[f]) {
            const CcUnit *fu = ix->factory_units.items[f];
            if (d->factory_arity->span.first < fu->file->n_toks)
                arity = (size_t)atoi(fu->file->src + fu->file->toks[d->factory_arity->span.first].off);
        }
        if (!cc__split_args(ix, text + strlen(prefix) + 1, arity, &args, depth + 1)) continue;
        {
            CcType *g = cc_type_new(ix->arena, CC_T_GENERIC, sp);
            g->name = d->name;
            for (i = 0; i < args.n; i++) CC_LIST_PUSH(ix->arena, &g->args, args.items[i]);
            return g;
        }
    }
    if (n > 3 && strcmp(text + n - 3, "ptr") == 0) {
        CcType *base = cc__unmangle_arg(ix, cc_arena_strndup(ix->arena, text, n - 3), depth + 1);
        if (base) {
            CcType *p = cc_type_new(ix->arena, CC_T_POINTER, sp);
            p->base = base;
            return p;
        }
    }
    return NULL;
}

/* `CCVec_int` spelled directly (not through `Vec::[int]`): find the family
 * whose instance prefix it carries and read the type arguments back. */
static int cc__infer_family(CcIndex *ix, CcTypeInfo *t) {
    CcType *g = cc__unmangle_arg(ix, t->name, 0);
    size_t i;
    if (!g || g->kind != CC_T_GENERIC) return 0;
    t->family = g->name;
    for (i = 0; i < g->args.n; i++) CC_LIST_PUSH(ix->arena, &t->targs, g->args.items[i]);
    return 1;
}

/* Expand the family's factory for one instance: every template in the
 * factory body that mentions `${mangled}` is instantiated and indexed. */
static void cc__type_instantiate(CcIndex *ix, CcTypeInfo *info) {
    size_t f, i;
    CcDecl *factory = NULL;
    CcUnit *fu = NULL;
    const char **args;
    CcSlotBind *binds = NULL;
    if (info->instantiated) return;
    if (info->is_result) {
        /* `CCResult_T_E` gets its methods from the spec macro, as the TU
         * emission of the spec will. */
        CcSym *ms = cc__sym_find_kind(ix, "CC_DECL_RESULT_SPEC", CC_SYM_MACRO);
        CcSym *mv = cc__sym_find_kind(ix, "CC_DECL_RESULT_SPEC_VOID", CC_SYM_MACRO);
        const char *a[3];
        if (!info->result_err) return;
        info->instantiated = 1;
        if (!info->result_value || strcmp(cc_index_canon(ix, info->result_value), "void") == 0) {
            if (!mv || !mv->macro) return;
            a[0] = info->name;
            a[1] = cc_index_canon(ix, info->result_err);
            cc__index_expansion(ix, cc_arena_printf(ix->arena, "<CC_DECL_RESULT_SPEC_VOID(%s) expansion>", info->name),
                                cc__macro_expand(ix, mv->macro, a, 2), 0);
        } else {
            if (!ms || !ms->macro) return;
            a[0] = info->name;
            a[1] = cc_index_canon(ix, info->result_value);
            a[2] = cc_index_canon(ix, info->result_err);
            cc__index_expansion(ix, cc_arena_printf(ix->arena, "<CC_DECL_RESULT_SPEC(%s) expansion>", info->name),
                                cc__macro_expand(ix, ms->macro, a, 3), 0);
        }
        return;
    }
    if (!info->family) return;
    for (f = 0; f < ix->factories.n; f++) {
        CcDecl *d = ix->factories.items[f];
        if (d->name && strcmp(d->name, info->family) == 0 && !d->factory_extend) {
            factory = d;
            fu = ix->factory_units.items[f];
            break;
        }
    }
    info->instantiated = 1;
    if (!factory) return;
    if (cc__sym_find_kind(ix, info->name, CC_SYM_TYPE)) return; /* hand-declared instance suppresses the splice */
    args = CC_NEW_N(ix->arena, const char *, info->targs.n + 1);
    for (i = 0; i < info->targs.n; i++) args[i] = cc__type_arg_spelling(ix, info->targs.items[i]);
    cc__factory_bindings(ix, fu, factory->body, &binds);
    for (i = factory->span.first; i <= factory->span.last; i++) {
        const CcToken *t = cc__tok(fu, i);
        const char *text;
        if (!t || t->kind != CC_TK_TEMPLATE) continue;
        if (t->len < 2) continue;
        text = fu->file->src + t->off + 1;
        if (!memmem(text, t->len - 2, "${mangled}", 10)) continue;
        cc__index_expansion(ix, cc_arena_printf(ix->arena, "<CC_GENERIC_FACTORY(%s) instance %s>", info->family, info->name),
                            cc__template_instance(ix, text, t->len - 2, info, binds, args, info->targs.n), 0);
    }
    /* extensions */
    for (f = 0; f < ix->factories.n; f++) {
        CcDecl *d = ix->factories.items[f];
        if (d->name && strcmp(d->name, info->family) == 0 && d->factory_extend) {
            CcUnit *eu = ix->factory_units.items[f];
            CcSlotBind *eb = NULL;
            cc__factory_bindings(ix, eu, d->body, &eb);
            for (i = d->span.first; i <= d->span.last; i++) {
                const CcToken *t = cc__tok(eu, i);
                const char *text;
                if (!t || t->kind != CC_TK_TEMPLATE || t->len < 2) continue;
                text = eu->file->src + t->off + 1;
                if (!memmem(text, t->len - 2, "${mangled}", 10)) continue;
                cc__index_expansion(ix, cc_arena_printf(ix->arena, "<CC_GENERIC_FACTORY_EXTEND(%s) instance %s>", info->family, info->name),
                                    cc__template_instance(ix, text, t->len - 2, info, eb, args, info->targs.n), 0);
            }
        }
    }
}

/* ---- expression helpers for hook bodies -------------------------------- */

static int cc__expr_mentions(const CcExpr *e, const char *name) {
    size_t i;
    if (!e) return 0;
    if (e->kind == CC_E_IDENT && e->name && strcmp(e->name, name) == 0) return 1;
    if (cc__expr_mentions(e->a, name) || cc__expr_mentions(e->b, name) || cc__expr_mentions(e->c, name)) return 1;
    for (i = 0; i < e->args.n; i++)
        if (cc__expr_mentions(e->args.items[i], name)) return 1;
    if (e->kind == CC_E_TEMPLATE) {
        const CcTplPart *pt;
        for (pt = e->tpl_parts; pt; pt = pt->next)
            if (pt->is_slot && cc__expr_mentions(pt->expr, name)) return 1;
    }
    return 0;
}

/* The first string literal in an expression (also the literal run of a
 * template before its first slot, and the text of @slice("...")). */
static const char *cc__expr_string(CcIndex *ix, const CcUnit *u, const CcExpr *e) {
    size_t i;
    const char *s;
    if (!e) return NULL;
    if (e->kind == CC_E_STRING) {
        const CcToken *t = cc__tok(u, e->tok);
        if (t) return cc__unquote(ix, u->file->src + t->off, t->len);
        return NULL;
    }
    if (e->kind == CC_E_TEMPLATE && e->tpl_parts) {
        const CcTplPart *pt = e->tpl_parts;
        if (!pt->is_slot) return cc_arena_strndup(ix->arena, u->file->src + pt->off, pt->len);
        return NULL;
    }
    if ((s = cc__expr_string(ix, u, e->a))) return s;
    if ((s = cc__expr_string(ix, u, e->b))) return s;
    if ((s = cc__expr_string(ix, u, e->c))) return s;
    for (i = 0; i < e->args.n; i++)
        if ((s = cc__expr_string(ix, u, e->args.items[i]))) return s;
    return NULL;
}

static const char *cc__callee_name(const CcExpr *e) {
    if (!e) return NULL;
    if (e->kind == CC_E_CALL && e->a && e->a->kind == CC_E_IDENT) return e->a->name;
    if (e->kind == CC_E_PAREN) return cc__callee_name(e->a);
    return NULL;
}

static int cc__expr_calls_prefix(const CcExpr *e, const char *prefix) {
    size_t i;
    const char *c;
    if (!e) return 0;
    c = cc__callee_name(e);
    if (c && cc__has_prefix(c, prefix)) return 1;
    if (cc__expr_calls_prefix(e->a, prefix) || cc__expr_calls_prefix(e->b, prefix) || cc__expr_calls_prefix(e->c, prefix)) return 1;
    for (i = 0; i < e->args.n; i++)
        if (cc__expr_calls_prefix(e->args.items[i], prefix)) return 1;
    return 0;
}

/* ---- `.ufcs` hook bodies as rules ------------------------------------- */

typedef struct CcHookCtx {
    CcIndex *ix;
    const CcUnit *unit;
    const char *method_param;
    const char *mode_param;
    CcHookReg *reg;
} CcHookCtx;

static void cc__hook_add_rule(CcHookCtx *c, const char *method, const CcExpr *ret) {
    const char *callee = cc__expr_string(c->ix, c->unit, ret);
    CcUfcsRule *r;
    if (!callee) {
        c->reg->ufcs_opaque = 1;
        return;
    }
    r = CC_NEW(c->ix->arena, CcUfcsRule);
    r->method = cc__in(c->ix, method);
    r->callee = cc__in(c->ix, callee);
    r->by_value = cc__expr_calls_prefix(ret, "cc_ufcs_emit_value");
    r->next = c->reg->rules;
    c->reg->rules = r;
}

/* The expression a branch returns for the plain (non-`mode`) call: the
 * last unconditional `return` of the branch; a `mode ? a : b` returns `b`. */
static const CcExpr *cc__branch_result(CcHookCtx *c, const CcStmt *s) {
    const CcExpr *found = NULL;
    size_t i;
    if (!s) return NULL;
    if (s->kind == CC_S_RETURN) {
        const CcExpr *e = s->expr;
        while (e && e->kind == CC_E_PAREN) e = e->a;
        if (e && e->kind == CC_E_TERNARY && c->mode_param && cc__expr_mentions(e->a, c->mode_param)) return e->c;
        return e;
    }
    if (s->kind == CC_S_BLOCK) {
        for (i = 0; i < s->stmts.n; i++) {
            const CcStmt *t = s->stmts.items[i];
            if (t->kind == CC_S_IF && c->mode_param && cc__expr_mentions(t->expr, c->mode_param)) {
                if (t->else_body) found = cc__branch_result(c, t->else_body);
                continue;
            }
            if (t->kind == CC_S_RETURN) return cc__branch_result(c, t);
            if (t->kind == CC_S_BLOCK) {
                const CcExpr *r = cc__branch_result(c, t);
                if (r) found = r;
            }
        }
    }
    return found;
}

static void cc__hook_default(CcHookCtx *c, const CcExpr *e) {
    const char *callee;
    while (e && e->kind == CC_E_PAREN) e = e->a;
    if (!e) return;
    if (e->kind == CC_E_TERNARY && c->mode_param && cc__expr_mentions(e->a, c->mode_param)) e = e->c;
    callee = cc__callee_name(e);
    if (callee && strcmp(callee, "cc_slice_empty") == 0) {
        c->reg->ufcs_rejects = 1;
        return;
    }
    if (callee && strcmp(callee, "cc_ufcs_pass") == 0) return;
    if (e->kind == CC_E_IDENT && strcmp(e->name, "CC_UFCS_PASS_TAG") == 0) return;
    if (cc__expr_mentions(e, c->method_param)) {
        const char *prefix = cc__expr_string(c->ix, c->unit, e);
        if (prefix) {
            c->reg->ufcs_prefix = cc__in(c->ix, prefix);
            c->reg->ufcs_prefix_by_value = cc__expr_calls_prefix(e, "cc_ufcs_emit_value");
            return;
        }
    }
    c->reg->ufcs_opaque = 1;
}

static void cc__hook_walk(CcHookCtx *c, const CcStmt *s) {
    size_t i;
    if (!s) return;
    switch (s->kind) {
    case CC_S_BLOCK:
        for (i = 0; i < s->stmts.n; i++) cc__hook_walk(c, s->stmts.items[i]);
        return;
    case CC_S_IF: {
        const char *lit = cc__expr_string(c->ix, c->unit, s->expr);
        if (lit && cc__expr_mentions(s->expr, c->method_param)) {
            const CcExpr *r = cc__branch_result(c, s->body);
            if (r) cc__hook_add_rule(c, lit, r);
            else c->reg->ufcs_opaque = 1;
            if (s->else_body) cc__hook_walk(c, s->else_body);
            return;
        }
        if (c->mode_param && cc__expr_mentions(s->expr, c->mode_param)) {
            if (s->else_body) cc__hook_walk(c, s->else_body);
            return;
        }
        /* a guard that is not about the method (`if (!p)`): look inside */
        cc__hook_walk(c, s->body);
        if (s->else_body) cc__hook_walk(c, s->else_body);
        return;
    }
    case CC_S_SWITCH: {
        const CcStmt *body = s->body;
        CC_LIST(const char) pending = {0};
        if (!cc__expr_mentions(s->expr, c->method_param) || !body || body->kind != CC_S_BLOCK) {
            c->reg->ufcs_opaque = 1;
            return;
        }
        for (i = 0; i < body->stmts.n; i++) {
            const CcStmt *t = body->stmts.items[i];
            const CcStmt *inner = t;
            int is_default = 0;
            while (inner && inner->kind == CC_S_CASE) {
                if (inner->is_default) is_default = 1;
                else {
                    const char *lit = cc__expr_string(c->ix, c->unit, inner->expr);
                    if (lit) CC_LIST_PUSH(c->ix->arena, &pending, lit);
                }
                inner = inner->body;
            }
            if (!inner) continue;
            if (inner->kind == CC_S_RETURN || inner->kind == CC_S_BLOCK) {
                const CcExpr *r = cc__branch_result(c, inner);
                size_t k;
                if (r) {
                    for (k = 0; k < pending.n; k++) cc__hook_add_rule(c, pending.items[k], r);
                    if (is_default) cc__hook_default(c, r);
                } else {
                    c->reg->ufcs_opaque = 1;
                }
                pending.n = 0;
            }
        }
        return;
    }
    case CC_S_RETURN:
        cc__hook_default(c, s->expr);
        return;
    default:
        return;
    }
}

/* Read the handler (a named function or a closure) into rules. */
static void cc__hook_read_ufcs(CcIndex *ix, const CcUnit *u, CcHookReg *reg) {
    CcHookCtx c;
    const CcExpr *v = reg->ufcs_value;
    memset(&c, 0, sizeof c);
    c.ix = ix;
    c.reg = reg;
    while (v && v->kind == CC_E_PAREN) v = v->a;
    if (!v) return;
    if (v->kind == CC_E_CLOSURE) {
        c.unit = u;
        if (v->params.n > 1) c.method_param = v->params.items[1]->name;
        if (v->params.n > 2) c.mode_param = v->params.items[2]->name;
        if (!c.method_param) { reg->ufcs_opaque = 1; return; }
        cc__hook_walk(&c, v->body);
        return;
    }
    if (v->kind == CC_E_IDENT) {
        CcSym *fs = cc__sym_find_kind(ix, v->name, CC_SYM_FUNC);
        if (!fs || !fs->decl || !fs->decl->body || !fs->decl->type) { reg->ufcs_opaque = 1; return; }
        c.unit = fs->unit;
        if (fs->decl->type->params.n > 1) c.method_param = fs->decl->type->params.items[1]->name;
        if (fs->decl->type->params.n > 2) c.mode_param = fs->decl->type->params.items[2]->name;
        if (!c.method_param) { reg->ufcs_opaque = 1; return; }
        cc__hook_walk(&c, fs->decl->body);
        return;
    }
    reg->ufcs_opaque = 1;
}

/* ---- @typehooks ---------------------------------------------------------- */

static void cc__hook_entry(CcIndex *ix, const CcUnit *u, CcHookReg *reg, const char *field, const CcExpr *value) {
    const char *f = field;
    const char *callee;
    if (f[0] == '.') f++;
    if (!value) return;
    callee = cc__callee_name(value);
    if (strcmp(f, "create") == 0) {
        if (value->kind == CC_E_IDENT) reg->create_fn = value->name;
        else if (value->kind == CC_E_CLOSURE) reg->create_fn = cc__in(ix, "<lambda>");
        else {
            size_t i;
            int n = 0;
            for (i = 0; i < value->args.n; i++) {
                const char *s = cc__expr_string(ix, u, value->args.items[i]);
                if (!s) continue;
                if (n == 0) reg->create_fn = cc__in(ix, s);
                else if (n == 1) reg->create_fn2 = cc__in(ix, s);
                n++;
            }
        }
    } else if (strcmp(f, "destroy") == 0) {
        if (value->kind == CC_E_IDENT) reg->destroy_fn = value->name;
        else if (value->kind == CC_E_CLOSURE) reg->destroy_fn = cc__in(ix, "<lambda>");
        else if (callee && strcmp(callee, "cc_type_destroy_hooks") == 0 && value->args.n >= 2) {
            const char *a = cc__expr_string(ix, u, value->args.items[0]);
            const char *b = cc__expr_string(ix, u, value->args.items[1]);
            if (a) reg->pre_destroy_fn = cc__in(ix, a);
            if (b) reg->destroy_fn = cc__in(ix, b);
        } else if (callee && strcmp(callee, "cc_type_pre_destroy_call") == 0) {
            const char *a = cc__expr_string(ix, u, value);
            if (a) reg->pre_destroy_fn = cc__in(ix, a);
        } else {
            const char *a = cc__expr_string(ix, u, value);
            if (a) reg->destroy_fn = cc__in(ix, a);
        }
    } else if (strcmp(f, "pre_destroy") == 0) {
        const char *a = value->kind == CC_E_IDENT ? value->name : cc__expr_string(ix, u, value);
        if (a) reg->pre_destroy_fn = cc__in(ix, a);
    } else if (strcmp(f, "ufcs") == 0) {
        reg->ufcs_value = (CcExpr *)value;
        reg->ufcs_fn = value->kind == CC_E_IDENT ? value->name : cc__in(ix, "<lambda>");
        cc__hook_read_ufcs(ix, u, reg);
    } else if (strcmp(f, "len") == 0) reg->has_len = 1;
    else if (strcmp(f, "access") == 0) reg->has_access = 1;
    else if (strcmp(f, "cast") == 0) reg->has_cast = 1;
    else if (strcmp(f, "niche") == 0) reg->has_niche = 1;
    else if (cc__has_prefix(f, "ufcs_sink") || cc__has_prefix(f, "ufcs_dynamic")) {
        const char *a = value->kind == CC_E_IDENT ? value->name : cc__expr_string(ix, u, value);
        reg->has_sink = 1;
        if (a) reg->sink_fn = cc__in(ix, a);
    }
}

static CcHookReg *cc__hook_new(CcIndex *ix, CcUnit *u, CcDecl *d, const char *subject) {
    CcHookReg *reg = CC_NEW(ix->arena, CcHookReg);
    size_t n = strlen(subject);
    size_t i;
    reg->subject = cc__in(ix, subject);
    reg->decl = d;
    reg->unit = u;
    if (strcmp(subject, "*") == 0) {
        reg->any = 1;
        reg->base = cc__in(ix, "");
    } else if (n > 2 && subject[n - 1] == '*' && subject[n - 2] == '_') {
        reg->family = 1;
        reg->base = cc__inn(ix, subject, n - 1); /* keep the `_` */
    } else if (n > 1 && subject[n - 1] == '*') {
        reg->ptr = 1;
        reg->base = cc__inn(ix, subject, n - 1);
    } else {
        reg->base = reg->subject;
    }
    CC_LIST_PUSH(ix->arena, &ix->hooks, reg);
    /* registrations may arrive after the types they govern were created */
    for (i = 0; i < ix->types.n; i++) cc__type_resolve_hooks(ix, ix->types.items[i]);
    return reg;
}

static void cc__typehooks_decl(CcIndex *ix, CcUnit *u, CcDecl *d) {
    CcBuf b;
    uint32_t i = d->span.first;
    const CcToken *t;
    CcHookReg *reg;
    CcHookEntry *e;
    /* subject text: the tokens between `on` and `{` */
    cc_buf_init(&b);
    for (; i <= d->span.last; i++) {
        t = cc__tok(u, i);
        if (!t) break;
        if (t->kind == CC_TK_IDENT && cc_tok_is(u->file, t, "on")) { i++; break; }
    }
    for (; i <= d->span.last; i++) {
        t = cc__tok(u, i);
        if (!t || (t->kind == CC_TK_PUNCT && t->punct == CC_P_LBRACE)) break;
        if (b.len && t->after_space && t->kind == CC_TK_IDENT) cc_buf_push_char(&b, ' ');
        cc_buf_push(&b, u->file->src + t->off, t->len);
    }
    if (!b.len) {
        cc_diag_emit(ix->diag, CC_SEV_ERROR, cc__span_loc(u, d->span), "@typehooks without a subject type");
        cc_buf_free(&b);
        return;
    }
    reg = cc__hook_new(ix, u, d, b.data);
    cc_buf_free(&b);
    for (e = d->entries; e; e = e->next)
        if (e->field) cc__hook_entry(ix, u, reg, e->field, e->value);
}

/* `(void)cc_type_register("Name", (CCTypeHooks){ .ufcs = f, ... })` inside
 * a @comptime block: the marker form of the same registration. */
static void cc__comptime_register_walk(CcIndex *ix, CcUnit *u, const CcStmt *s) {
    size_t i;
    const CcExpr *e;
    if (!s) return;
    if (s->kind == CC_S_BLOCK || s->kind == CC_S_COMPTIME_BLOCK) {
        for (i = 0; i < s->stmts.n; i++) cc__comptime_register_walk(ix, u, s->stmts.items[i]);
        return;
    }
    if (s->kind != CC_S_EXPR) return;
    e = s->expr;
    while (e && (e->kind == CC_E_PAREN || e->kind == CC_E_CAST)) e = e->a;
    if (!e || e->kind != CC_E_CALL) return;
    {
        const char *c = cc__callee_name(e);
        const char *name;
        const CcExpr *hooks;
        const CcInit *in;
        CcHookReg *reg;
        if (!c || !(strcmp(c, "cc_type_register") == 0 || strcmp(c, "cc_type_define") == 0)) return;
        if (e->args.n < 2) return;
        name = cc__expr_string(ix, u, e->args.items[0]);
        hooks = e->args.items[1];
        if (!name) return;
        while (hooks && hooks->kind == CC_E_PAREN) hooks = hooks->a;
        if (!hooks || hooks->kind != CC_E_COMPOUND || !hooks->init) return;
        reg = cc__hook_new(ix, u, (CcDecl *)NULL, name);
        for (i = 0; i < hooks->init->list.n; i++) {
            in = hooks->init->list.items[i];
            if (in->designators && in->designators->field && in->expr)
                cc__hook_entry(ix, u, reg, in->designators->field, in->expr);
        }
    }
}

/* Narrowest registration whose subject matches `name`; `want_ufcs` asks
 * for one carrying a `.ufcs` entry. Exact beats pointer-subject beats
 * family beats `*`. */
/* `struct Point` and `Point` name the same subject for a registration. */
static const char *cc__strip_tag(const char *name) {
    if (cc__has_prefix(name, "struct ")) return name + 7;
    if (cc__has_prefix(name, "union ")) return name + 6;
    if (cc__has_prefix(name, "enum ")) return name + 5;
    return name;
}

static CcHookReg *cc__hook_match(const CcIndex *ix, const char *name, int want_ufcs) {
    CcHookReg *best = NULL;
    int best_score = -1;
    size_t i;
    name = cc__strip_tag(name);
    for (i = 0; i < ix->hooks.n; i++) {
        CcHookReg *r = ix->hooks.items[i];
        int score;
        if (want_ufcs && !r->ufcs_fn) continue;
        if (r->any) score = 0;
        else if (r->family) score = cc__has_prefix(name, r->base) && strlen(name) > strlen(r->base) ? 1 + (int)strlen(r->base) : -1;
        else if (r->ptr) score = strcmp(name, r->base) == 0 ? 2000 : -1;
        else score = strcmp(name, r->base) == 0 ? 3000 : -1;
        if (score < 0) continue;
        if (score >= best_score) { /* later registration of equal specificity wins (user after stdlib) */
            best_score = score;
            best = r;
        }
    }
    return best;
}

static CcName cc__suffix_callee(CcIndex *ix, const CcTypeInfo *info, CcName fn) {
    if (fn && fn[0] == '_') return cc__in(ix, cc_arena_printf(ix->arena, "%s%s", info->name, fn));
    return fn;
}

static void cc__type_resolve_hooks(CcIndex *ix, CcTypeInfo *info) {
    CcHookReg *r = cc__hook_match(ix, info->name, 0);
    CcHookReg *create = NULL, *destroy = NULL;
    size_t i;
    const char *name = cc__strip_tag(info->name);
    info->hooks = r;
    /* create / destroy come from the narrowest registration that has them */
    for (i = 0; i < ix->hooks.n; i++) {
        CcHookReg *h = ix->hooks.items[i];
        int score;
        if (h->any) score = 0;
        else if (h->family) score = cc__has_prefix(name, h->base) && strlen(name) > strlen(h->base) ? 1 + (int)strlen(h->base) : -1;
        else if (h->ptr) score = strcmp(name, h->base) == 0 ? 2000 : -1;
        else score = strcmp(name, h->base) == 0 ? 3000 : -1;
        if (score < 0) continue;
        if (h->create_fn && (!create || score >= (create->any ? 0 : create->family ? 1 + (int)strlen(create->base) : create->ptr ? 2000 : 3000)))
            create = h;
        if ((h->destroy_fn || h->pre_destroy_fn) && (!destroy || score >= (destroy->any ? 0 : destroy->family ? 1 + (int)strlen(destroy->base) : destroy->ptr ? 2000 : 3000)))
            destroy = h;
    }
    info->create_fn = create ? cc__suffix_callee(ix, info, create->create_fn) : NULL;
    info->destroy_fn = destroy ? cc__suffix_callee(ix, info, destroy->destroy_fn) : NULL;
    info->pre_destroy_fn = destroy ? cc__suffix_callee(ix, info, destroy->pre_destroy_fn) : NULL;
}

/* ---- noreturn ------------------------------------------------------------- */

static int cc__attrs_noreturn(const CcAttr *a) {
    for (; a; a = a->next) {
        if (!a->name) continue;
        if (strcmp(a->name, "_Noreturn") == 0 || strcmp(a->name, "noreturn") == 0) return 1;
        if (a->value && strcmp(a->value, "noreturn") == 0) return 1;
        if (a->value && strcmp(a->value, "__noreturn__") == 0) return 1;
    }
    return 0;
}

int cc_index_sym_noreturn(const CcSym *s) {
    const CcDecl *d;
    uint32_t i;
    if (!s || !s->decl) return 0;
    d = s->decl;
    if (d->specs & CC_F_NORETURN) return 1;
    if (cc__attrs_noreturn(d->attrs)) return 1;
    if (!s->unit) return 0;
    /* specifier tokens before the name: `_Noreturn`, `noreturn`, or a macro
     * whose definition spells one (CC_NORETURN) */
    for (i = d->span.first; i <= d->span.last; i++) {
        const CcToken *t = cc__tok(s->unit, i);
        if (!t || t->kind == CC_TK_EOF) break;
        if (t->kind != CC_TK_IDENT) continue;
        if (d->name && cc_tok_is(s->unit->file, t, d->name)) break;
        if (cc_tok_is(s->unit->file, t, "_Noreturn") || cc_tok_is(s->unit->file, t, "noreturn")) return 1;
    }
    return 0;
}

/* The macro form (`CC_NORETURN`) needs the macro table: the index sets
 * CC_F_NORETURN on the declaration when it adds the symbol, so the plain
 * query above answers it afterwards. */

static int cc__sym_noreturn_ix(const CcIndex *ix, const CcSym *s) {
    const CcDecl *d;
    uint32_t i;
    if (cc_index_sym_noreturn(s)) return 1;
    if (!s || !s->decl || !s->unit) return 0;
    d = s->decl;
    for (i = d->span.first; i <= d->span.last; i++) {
        const CcToken *t = cc__tok(s->unit, i);
        CcSym *ms;
        const CcMacroDef *m;
        char name[128];
        if (!t || t->kind == CC_TK_EOF) break;
        if (t->kind != CC_TK_IDENT) continue;
        if (d->name && cc_tok_is(s->unit->file, t, d->name)) break;
        if (t->len >= sizeof name) continue;
        memcpy(name, s->unit->file->src + t->off, t->len);
        name[t->len] = 0;
        ms = cc__sym_find_kind(ix, name, CC_SYM_MACRO);
        if (!ms) continue;
        for (m = ms->macro; m; m = m->alt)
            if (m->body && (strstr(m->body, "noreturn") || strstr(m->body, "_Noreturn"))) return 1;
    }
    return 0;
}

/* ---- adding units ---------------------------------------------------------- */

static void cc__add_typedef_name(CcIndex *ix, CcName name) {
    size_t i;
    for (i = 0; i < ix->typedef_names.n; i++)
        if (ix->typedef_names.items[i] == name || strcmp(ix->typedef_names.items[i], name) == 0) return;
    CC_LIST_PUSH(ix->arena, &ix->typedef_names, name);
}

static void cc__index_tag_type(CcIndex *ix, CcUnit *u, CcDecl *d, CcType *t, int is_header) {
    const CcEnumerator *en;
    if (!t) return;
    if (t->kind == CC_T_STRUCT && t->name) {
        CcName tag = cc__in(ix, cc_arena_printf(ix->arena, "%s %s", t->is_union ? "union" : "struct", t->name));
        cc__sym_add(ix, tag, CC_SYM_TYPE, d, u, t, is_header, t->fields != NULL);
    } else if (t->kind == CC_T_ENUM && t->name) {
        CcName tag = cc__in(ix, cc_arena_printf(ix->arena, "enum %s", t->name));
        cc__sym_add(ix, tag, CC_SYM_TYPE, d, u, t, is_header, t->enumerators != NULL);
    }
    if (t->kind == CC_T_ENUM)
        for (en = t->enumerators; en; en = en->next)
            if (en->name) cc__sym_add(ix, en->name, CC_SYM_ENUMERATOR, d, u, t, is_header, 1);
    /* nested definitions in fields */
    if (t->kind == CC_T_STRUCT) {
        const CcField *f;
        for (f = t->fields; f; f = f->next)
            if (f->type && (f->type->kind == CC_T_STRUCT || f->type->kind == CC_T_ENUM))
                cc__index_tag_type(ix, u, d, f->type, is_header);
    }
    if (t->kind == CC_T_POINTER || t->kind == CC_T_ARRAY) cc__index_tag_type(ix, u, d, t->base, is_header);
}

static void cc__index_decl(CcIndex *ix, CcUnit *u, CcDecl *d, int is_header) {
    size_t i;
    if (!d) return;
    switch (d->kind) {
    case CC_D_VAR:
        if (d->name) cc__sym_add(ix, d->name, CC_SYM_VAR, d, u, d->type, is_header, d->init != NULL);
        cc__index_tag_type(ix, u, d, d->type, is_header);
        return;
    case CC_D_FUNC:
    case CC_D_COMPTIME_FN:
        if (d->name) {
            CcSym *s = cc__sym_add(ix, d->name, CC_SYM_FUNC, d, u, d->type, is_header, d->body != NULL);
            if (s->decl == d && cc__sym_noreturn_ix(ix, s)) d->specs |= CC_F_NORETURN;
        }
        if (d->type) cc__index_tag_type(ix, u, d, d->type->base, is_header);
        return;
    case CC_D_TYPEDEF:
        if (d->name) {
            cc__sym_add(ix, d->name, CC_SYM_TYPE, d, u, d->type, is_header,
                        d->type && (d->type->kind != CC_T_STRUCT || d->type->fields != NULL));
            cc__add_typedef_name(ix, d->name);
        }
        cc__index_tag_type(ix, u, d, d->type, is_header);
        return;
    case CC_D_TAGGED:
        cc__index_tag_type(ix, u, d, d->type, is_header);
        return;
    case CC_D_PP: {
        CcMacroDef *m = cc__parse_define(ix, u, d);
        if (m) {
            CcSym *s = cc__sym_add(ix, m->name, CC_SYM_MACRO, d, u, NULL, is_header, m->body[0] != 0);
            if (!s->macro) s->macro = m;
            else {
                CcMacroDef *a = s->macro;
                while (a->alt) a = a->alt;
                a->alt = m;
            }
        }
        return;
    }
    case CC_D_TYPEHOOKS:
        cc__typehooks_decl(ix, u, d);
        return;
    case CC_D_VARIANT:
    case CC_D_SCOPED_TYPE:
        if (d->name) {
            cc__sym_add(ix, d->name, CC_SYM_TYPE, d, u, d->type, is_header, 1);
            cc__add_typedef_name(ix, d->name);
        }
        return;
    case CC_D_GENERIC_FACTORY:
        CC_LIST_PUSH(ix->arena, &ix->factories, d);
        CC_LIST_PUSH(ix->arena, &ix->factory_units, u);
        return;
    case CC_D_COMPTIME_IF:
        for (i = 0; i < d->then_decls.n; i++) cc__index_decl(ix, u, d->then_decls.items[i], is_header);
        for (i = 0; i < d->else_decls.n; i++) cc__index_decl(ix, u, d->else_decls.items[i], is_header);
        return;
    case CC_D_COMPTIME_BLOCK:
        cc__comptime_register_walk(ix, u, d->body);
        return;
    case CC_D_MACRO_CALL:
        cc__macro_call(ix, u, d, is_header);
        return;
    case CC_D_TYPEVIEW:
        /* `@typeview Mode on T { ... }` names the value view `T_Restrict_Mode`
         * (spec/draft_facets.md): an alias of T for method lookup */
        if (d->name && d->type) {
            CcName alias = cc__in(ix, cc_arena_printf(ix->arena, "%s_Restrict_%s", cc__strip_tag(cc_index_canon(ix, d->type)), d->name));
            cc__sym_add(ix, alias, CC_SYM_TYPE, d, u, d->type, is_header, 1);
            cc__add_typedef_name(ix, alias);
        }
        return;
    default:
        return;
    }
}

/* `SmallVec::[int, 8]`: a value argument's spelling is read from the
 * unit's tokens once, while the unit is known, and kept on the node. */
typedef struct CcValueCtx { CcIndex *ix; CcUnit *u; } CcValueCtx;
static int cc__value_on_type(CcVisitor *v, CcType *t) {
    CcValueCtx *c = (CcValueCtx *)v->ctx;
    if (t->kind == CC_T_VALUE && !t->name && t->value) {
        const CcExpr *e = t->value;
        if (e->span.first <= e->span.last && e->span.last < c->u->file->n_toks)
            t->name = cc__in(c->ix, cc__span_text(c->ix, c->u, e->span.first, e->span.last));
    }
    return 0;
}

void cc_index_add_unit(CcIndex *ix, CcUnit *u, int is_header) {
    size_t i;
    {
        CcVisitor v;
        CcValueCtx c;
        memset(&v, 0, sizeof v);
        c.ix = ix;
        c.u = u;
        v.ctx = &c;
        v.on_type = cc__value_on_type;
        cc_ast_walk(u, &v);
    }
    CC_LIST_PUSH(ix->arena, &ix->units, u);
    CC_LIST_PUSH(ix->arena, &ix->unit_is_header, is_header ? "1" : "0");
    for (i = 0; i < u->decls.n; i++) cc__index_decl(ix, u, u->decls.items[i], is_header);
    /* the parser's forward-referenced type names are typedef names for later units */
    for (i = 0; i < u->typedef_names.n; i++) cc__add_typedef_name(ix, cc__in(ix, u->typedef_names.items[i]));
}

CcIndex *cc_index_new(CcArena *a, CcDiag *d, CcIntern *in) {
    CcIndex *ix = CC_NEW(a, CcIndex);
    ix->arena = a;
    ix->diag = d;
    ix->intern = in;
    cc__syms_grow(ix);
    cc__types_grow(ix);
    return ix;
}

/* ---- includes ---------------------------------------------------------------- */

/* `#include <x>` / `#include "x"` lines of a lexed file. */
static size_t cc__scan_includes(CcIndex *ix, const CcLexFile *f, CcIncludeRef **out) {
    CC_LIST(CcIncludeRef) refs = {0};
    uint32_t i;
    for (i = 0; i < f->n_toks; i++) {
        const CcToken *t = &f->toks[i];
        const char *p, *end, *s;
        char close;
        CcIncludeRef *r;
        if (t->kind != CC_TK_PP) continue;
        p = f->src + t->off;
        end = p + t->len;
        if (*p != '#') continue;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if ((size_t)(end - p) < 7 || memcmp(p, "include", 7) != 0) continue;
        p += 7;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || !(*p == '<' || *p == '"')) continue;
        close = *p == '<' ? '>' : '"';
        s = ++p;
        while (p < end && *p != close) p++;
        if (p >= end) continue;
        r = CC_NEW(ix->arena, CcIncludeRef);
        r->path = cc_arena_strndup(ix->arena, s, (size_t)(p - s));
        r->quoted = close == '"';
        r->off = t->off;
        CC_LIST_PUSH(ix->arena, &refs, r);
    }
    /* the list holds pointers; hand them back as an array of refs */
    {
        CcIncludeRef *arr = CC_NEW_N(ix->arena, CcIncludeRef, refs.n + 1);
        for (i = 0; i < refs.n; i++) arr[i] = *refs.items[i];
        *out = arr;
    }
    return refs.n;
}

static const char *cc__dirname(CcIndex *ix, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return ".";
    if (slash == path) return "/";
    return cc_arena_strndup(ix->arena, path, (size_t)(slash - path));
}

static int cc__file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *cc__resolve_include(CcIndex *ix, const char *from_path, const CcIncludeRef *r,
                                       const CcIndexOpts *opts, CcBuf *searched) {
    const char *cand;
    size_t i;
    if (r->path[0] == '/') return cc__file_exists(r->path) ? r->path : NULL;
    if (r->quoted) {
        cand = cc_arena_printf(ix->arena, "%s/%s", cc__dirname(ix, from_path), r->path);
        if (cc__file_exists(cand)) return cand;
        cc_buf_printf(searched, " %s", cand);
        if (opts && opts->quote_dir) {
            cand = cc_arena_printf(ix->arena, "%s/%s", opts->quote_dir, r->path);
            if (cc__file_exists(cand)) return cand;
            cc_buf_printf(searched, " %s", cand);
        }
    }
    if (opts && opts->include_dirs) {
        for (i = 0; opts->include_dirs[i]; i++) {
            cand = cc_arena_printf(ix->arena, "%s/%s", opts->include_dirs[i], r->path);
            if (cc__file_exists(cand)) return cand;
            cc_buf_printf(searched, " %s", cand);
        }
    }
    return NULL;
}

static const char *cc__canonical_path(CcIndex *ix, const char *path) {
    char buf[PATH_MAX];
    if (realpath(path, buf)) return cc_arena_strdup(ix->arena, buf);
    return cc_arena_strdup(ix->arena, path);
}

static int cc__path_listed(const void *list_items, size_t n, const char *path) {
    const char *const *items = (const char *const *)list_items;
    size_t i;
    for (i = 0; i < n; i++)
        if (strcmp(items[i], path) == 0) return 1;
    return 0;
}

static void cc__load_includes_of(CcIndex *ix, const CcLexFile *f, const CcIndexOpts *opts, const CcParseOpts *popts);

static void cc__load_header(CcIndex *ix, const char *path, const CcIndexOpts *opts, const CcParseOpts *popts) {
    const char *canon = cc__canonical_path(ix, path);
    size_t len = 0;
    char *src;
    CcLexFile *f;
    CcUnit *u;
    CcParseOpts po;
    if (cc__path_listed(ix->loaded_paths.items, ix->loaded_paths.n, canon)) return;
    if (cc__path_listed(ix->loading_paths.items, ix->loading_paths.n, canon)) return; /* include cycle */
    CC_LIST_PUSH(ix->arena, &ix->loading_paths, canon);
    src = cc_read_file(ix->arena, path, &len);
    if (!src) {
        CcLoc loc = {path, 0, 0};
        cc_diag_emit(ix->diag, CC_SEV_ERROR, loc, "cannot read header: %s", strerror(errno));
        CC_LIST_PUSH(ix->arena, &ix->loaded_paths, canon);
        return;
    }
    f = cc_lex(ix->arena, ix->diag, path, src, len);
    cc__load_includes_of(ix, f, opts, popts);
    if (popts) po = *popts;
    else memset(&po, 0, sizeof po);
    po.mode = CC_MODE_HEADER;
    po.allow_top_level_stmts = 0;
    po.known_types = cc_index_known_types(ix);
    u = cc_parse(ix->arena, ix->diag, ix->intern, f, &po);
    cc_index_add_unit(ix, u, 1);
    CC_LIST_PUSH(ix->arena, &ix->loaded_paths, canon);
}

static void cc__load_includes_of(CcIndex *ix, const CcLexFile *f, const CcIndexOpts *opts, const CcParseOpts *popts) {
    CcIncludeRef *refs = NULL;
    size_t n = cc__scan_includes(ix, f, &refs), i;
    for (i = 0; i < n; i++) {
        CcBuf searched;
        const char *found;
        if (cc__has_suffix(refs[i].path, ".h")) {
            /* `<x.h>` names a lowered header when `x.cch` is on the path: index
             * its source. Otherwise it is a C header and stays C. */
            CcIncludeRef src = refs[i];
            size_t pl = strlen(refs[i].path);
            src.path = cc_arena_printf(ix->arena, "%.*s.cch", (int)(pl - 2), refs[i].path);
            cc_buf_init(&searched);
            found = cc__resolve_include(ix, f->path, &src, opts, &searched);
            cc_buf_free(&searched);
            if (found) cc__load_header(ix, found, opts, popts);
            continue;
        }
        if (!cc__has_suffix(refs[i].path, ".cch")) continue; /* C headers stay C */
        cc_buf_init(&searched);
        found = cc__resolve_include(ix, f->path, &refs[i], opts, &searched);
        if (!found) {
            cc_diag_emit(ix->diag, CC_SEV_ERROR, cc_lex_loc(f, refs[i].off), "include not found: %c%s%c (searched:%s)",
                         refs[i].quoted ? '"' : '<', refs[i].path, refs[i].quoted ? '"' : '>',
                         searched.len ? searched.data : " nothing");
            cc_buf_free(&searched);
            continue;
        }
        cc_buf_free(&searched);
        cc__load_header(ix, found, opts, popts);
    }
}

void cc_index_preload_includes(CcIndex *ix, CcLexFile *f, const CcIndexOpts *opts, const CcParseOpts *popts) {
    if (opts) ix->opts = *opts;
    cc__load_includes_of(ix, f, opts, popts);
}

void cc_index_load_includes(CcIndex *ix, CcUnit *u, const CcIndexOpts *opts, const CcParseOpts *popts) {
    if (opts) ix->opts = *opts;
    cc__load_includes_of(ix, u->file, opts, popts);
}

/* ---- method resolution ------------------------------------------------------- */

static int cc__first_param_is_pointer(const CcSym *callee) {
    const CcType *ft;
    if (!callee || callee->kind != CC_SYM_FUNC || !callee->type || callee->type->kind != CC_T_FUNC) return 0;
    ft = callee->type;
    if (ft->params.n == 0 || !ft->params.items[0]->type) return 0;
    return ft->params.items[0]->type->kind == CC_T_POINTER;
}

static CcMethod *cc__method_new(CcIndex *ix, CcTypeInfo *info, CcName method, CcName callee, const char *source,
                                const char *origin, CcSym *sym, int by_value_hint) {
    CcMethod *m = CC_NEW(ix->arena, CcMethod);
    m->method = method;
    m->callee = callee;
    m->source = source;
    m->origin = origin;
    m->sym = sym;
    /* a function-like macro from a hook has no parameters to read: a hook's
     * callee is address-style unless it said by value */
    m->recv_by_ptr = by_value_hint ? 0 : (sym && sym->kind == CC_SYM_MACRO) ? 1 : cc__first_param_is_pointer(sym);
    m->next = info->methods;
    info->methods = m;
    return m;
}

static const char *cc__sym_origin(CcIndex *ix, const CcSym *s) {
    CcLoc l = cc_index_sym_loc(s);
    if (!s) return "";
    if (s->kind == CC_SYM_MACRO) return cc_arena_printf(ix->arena, "macro %s (%s:%u)", s->name, l.path ? l.path : "?", l.line);
    return cc_arena_printf(ix->arena, "%s:%u", l.path ? l.path : "?", l.line);
}

/* The struct definition behind a type name: through typedefs and tags. */
static const CcType *cc__struct_of(const CcIndex *ix, const char *name, int depth) {
    CcSym *s;
    const CcType *t;
    if (depth > 8) return NULL;
    s = cc__sym_find_kind(ix, name, CC_SYM_TYPE);
    if (!s || !s->type) return NULL;
    t = s->type;
    if (t->kind == CC_T_STRUCT) {
        if (t->fields) return t;
        if (t->name) {
            CcSym *tag = cc__sym_find_kind(ix, cc_arena_printf((CcArena *)ix->arena, "%s %s", t->is_union ? "union" : "struct", t->name), CC_SYM_TYPE);
            if (tag && tag->type && tag->type->fields) return tag->type;
        }
        return NULL;
    }
    if (t->kind == CC_T_NAMED && t->name) return cc__struct_of(ix, t->name, depth + 1);
    return NULL;
}

/* `@typeview on Subject { as: field; }` faces of a type: the field names. */
static size_t cc__view_faces(CcIndex *ix, const char *name, const char ***out) {
    CC_LIST(const char) faces = {0};
    size_t i;
    name = cc__strip_tag(name);
    for (i = 0; i < ix->units.n; i++) {
        CcUnit *u = ix->units.items[i];
        size_t k;
        for (k = 0; k < u->decls.n; k++) {
            CcDecl *d = u->decls.items[k];
            CcBuf b;
            uint32_t ti;
            int matches;
            CcHookEntry *e;
            if (d->kind != CC_D_TYPEVIEW) continue;
            cc_buf_init(&b);
            for (ti = d->span.first; ti <= d->span.last; ti++) {
                const CcToken *t = cc__tok(u, ti);
                if (!t) break;
                if (t->kind == CC_TK_IDENT && cc_tok_is(u->file, t, "on")) { ti++; break; }
            }
            for (; ti <= d->span.last; ti++) {
                const CcToken *t = cc__tok(u, ti);
                if (!t || (t->kind == CC_TK_PUNCT && t->punct == CC_P_LBRACE)) break;
                if (b.len && t->after_space && t->kind == CC_TK_IDENT) cc_buf_push_char(&b, ' ');
                cc_buf_push(&b, u->file->src + t->off, t->len);
            }
            matches = 0;
            if (b.len && b.data[b.len - 1] == '*' && b.len > 1 && b.data[b.len - 2] == '_')
                matches = strncmp(name, b.data, b.len - 1) == 0 && strlen(name) > b.len - 1;
            else
                matches = strcmp(name, b.data) == 0;
            cc_buf_free(&b);
            if (!matches) continue;
            for (e = d->entries; e; e = e->next) {
                const CcViewItem *vi;
                if (!e->field || strcmp(e->field, "as") != 0) continue;
                for (vi = e->items; vi; vi = vi->next)
                    if (vi->name) CC_LIST_PUSH(ix->arena, &faces, vi->name);
            }
        }
    }
    *out = faces.items;
    return faces.n;
}

static CcMethod *cc__resolve(CcIndex *ix, CcTypeInfo *recv, CcName method, CcBuf *tried, int depth, unsigned recv_shape);

/* Do two canonical spellings name one type: the same name, a typedef of
 * the other (`Point` / `struct Point`), or two arithmetic types (a C call
 * converts the receiver)? */
static int cc__same_type(CcIndex *ix, const char *a, const char *b) {
    int depth;
    const char *x;
    if (strcmp(a, b) == 0) return 1;
    for (x = a, depth = 0; x && depth < 8; depth++) {
        CcSym *s = cc__sym_find_kind(ix, x, CC_SYM_TYPE);
        if (!s || !s->type || !s->decl || s->decl->kind != CC_D_TYPEDEF) break;
        x = cc_index_canon(ix, s->type);
        if (strcmp(x, b) == 0) return 1;
    }
    for (x = b, depth = 0; x && depth < 8; depth++) {
        CcSym *s = cc__sym_find_kind(ix, x, CC_SYM_TYPE);
        if (!s || !s->type || !s->decl || s->decl->kind != CC_D_TYPEDEF) break;
        x = cc_index_canon(ix, s->type);
        if (strcmp(x, a) == 0) return 1;
    }
    {
        /* both arithmetic: the call converts */
        static const char *const arith[] = {"char", "signed char", "unsigned char", "short", "unsigned short", "int",
                                            "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
                                            "float", "double", "long double", "bool", "size_t", "ssize_t", "ptrdiff_t",
                                            "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                                            "uint64_t", "intptr_t", "uintptr_t", NULL};
        int ia = 0, ib = 0, i;
        for (i = 0; arith[i]; i++) {
            if (strcmp(arith[i], a) == 0) ia = 1;
            if (strcmp(arith[i], b) == 0) ib = 1;
        }
        if (ia && ib) return 1;
    }
    return 0;
}

/* The receiver type an `as:` face or a typedef alias delegates to. */
static CcMethod *cc__resolve_via(CcIndex *ix, CcTypeInfo *recv, CcName method, CcBuf *tried, int depth,
                                 CcType *target, const char *how, unsigned recv_shape) {
    CcName tn;
    CcTypeInfo *ti;
    CcMethod *m;
    if (!target) return NULL;
    while (target && (target->kind == CC_T_POINTER || target->kind == CC_T_ATOMIC)) target = target->base;
    tn = cc_index_canon(ix, target);
    if (strcmp(tn, recv->name) == 0) return NULL;
    ti = cc_index_type_get(ix, tn);
    m = cc__resolve(ix, ti, method, tried, depth + 1, recv_shape);
    if (!m) return NULL;
    return cc__method_new(ix, recv, method, m->callee, m->source,
                          cc_arena_printf(ix->arena, "%s %s; %s", how, tn, m->origin ? m->origin : ""), m->sym, !m->recv_by_ptr);
}

static CcMethod *cc__resolve(CcIndex *ix, CcTypeInfo *recv, CcName method, CcBuf *tried, int depth, unsigned recv_shape) {
    CcMethod *m;
    CcHookReg *hook;
    CcSym *cs;
    CcName composed;
    const char *base_name;
    if (depth > 6) return NULL;
    for (m = recv->methods; m; m = m->next)
        if (strcmp(m->method, method) == 0) return m;

    /* 1. @typehooks .ufcs */
    hook = cc__hook_match(ix, recv->name, 1);
    if (hook) {
        const CcUfcsRule *r;
        const char *src = cc_arena_printf(ix->arena, "@typehooks on %s .ufcs = %s", hook->subject, hook->ufcs_fn);
        for (r = hook->rules; r; r = r->next) {
            if (strcmp(r->method, method) != 0) continue;
            cs = cc__callee_sym(ix, r->callee);
            if (cs) return cc__method_new(ix, recv, method, r->callee, "typehooks", cc_arena_printf(ix->arena, "%s -> %s", src, cc__sym_origin(ix, cs)), cs, r->by_value);
            cc_buf_printf(tried, " %s (hook %s: not declared)", r->callee, hook->ufcs_fn);
            return NULL;
        }
        if (hook->ufcs_prefix) {
            composed = cc__in(ix, cc_arena_printf(ix->arena, "%s%s", hook->ufcs_prefix, method));
            cs = cc__callee_sym(ix, composed);
            if (cs) return cc__method_new(ix, recv, method, composed, "typehooks", cc_arena_printf(ix->arena, "%s -> %s", src, cc__sym_origin(ix, cs)), cs, hook->ufcs_prefix_by_value);
            cc_buf_printf(tried, " %s (hook %s: not declared)", composed, hook->ufcs_fn);
            return NULL;
        }
        /* rejecting (empty slice) or opaque handlers fall through to the composed names */
    }

    /* 1b. `x.destroy()` on a type with a destroy hook: the hook's callee
     * (the spec: `Type_destroy` when that function exists, else the chain) */
    if (strcmp(method, "destroy") == 0 && recv->destroy_fn && recv->destroy_fn[0] != '<') {
        CcSym *d1 = cc__callee_sym(ix, cc_arena_printf(ix->arena, "%s_destroy", recv->name));
        if (!d1) {
            cs = cc__callee_sym(ix, recv->destroy_fn);
            if (cs) return cc__method_new(ix, recv, method, recv->destroy_fn, "typehooks", cc_arena_printf(ix->arena, "@typehooks .destroy -> %s", cc__sym_origin(ix, cs)), cs, 0);
            cc_buf_printf(tried, " %s (.destroy hook: not declared)", recv->destroy_fn);
        }
    }

    /* 2. *_DECL_UFCS(Name) registration: Name_<method> */
    if (recv->ufcs_registered_by) {
        composed = cc__in(ix, cc_arena_printf(ix->arena, "%s_%s", recv->name, method));
        cs = cc__callee_sym(ix, composed);
        if (cs) return cc__method_new(ix, recv, method, composed, "DECL_UFCS", cc_arena_printf(ix->arena, "%s(%s) -> %s", recv->ufcs_registered_by, recv->name, cc__sym_origin(ix, cs)), cs, 0);
        cc_buf_printf(tried, " %s (%s)", composed, recv->ufcs_registered_by);
    }

    /* 3. Type_method (a Result spec's methods are this, from the spec macro);
     * a tagged receiver composes with its tag (`struct Point` -> Point_sum) */
    base_name = recv->name;
    if (cc__has_prefix(base_name, "struct ")) base_name += 7;
    else if (cc__has_prefix(base_name, "union ")) base_name += 6;
    else if (cc__has_prefix(base_name, "enum ")) base_name += 5;
    composed = cc__in(ix, cc_arena_printf(ix->arena, "%s_%s", base_name, method));
    cs = cc__callee_sym(ix, composed);
    if (cs) {
        const char *source = recv->is_result ? "Result" : recv->family ? "Type_method (factory instance)" : "Type_method";
        return cc__method_new(ix, recv, method, composed, source, cc__sym_origin(ix, cs), cs, 0);
    }
    if (recv->is_result && (strcmp(method, "is_ok") == 0 || strcmp(method, "is_err") == 0 || strcmp(method, "value") == 0 ||
                            strcmp(method, "error") == 0 || strcmp(method, "unwrap_or") == 0)) {
        /* the spec macro defines these; a spec the unit itself emits has no symbol yet */
        return cc__method_new(ix, recv, method, composed, "Result", "the Result spec", NULL, 1);
    }
    if (!recv->ufcs_registered_by) cc_buf_printf(tried, " %s", composed);

    /* 4. cc_<snake>_method / <snake>_method (a bare type also tries the cc_ prefix) */
    {
        CcName snake = cc_index_snake(ix, base_name);
        composed = cc__in(ix, cc_arena_printf(ix->arena, "%s_%s", snake, method));
        if (strcmp(composed, cc_arena_printf(ix->arena, "%s_%s", base_name, method)) != 0) {
            cs = cc__callee_sym(ix, composed);
            if (cs) return cc__method_new(ix, recv, method, composed, cc__has_prefix(snake, "cc_") ? "cc_snake_method" : "snake_method", cc__sym_origin(ix, cs), cs, 0);
            cc_buf_printf(tried, " %s", composed);
        }
        if (!cc__has_prefix(snake, "cc_")) {
            composed = cc__in(ix, cc_arena_printf(ix->arena, "cc_%s_%s", snake, method));
            cs = cc__callee_sym(ix, composed);
            if (cs) return cc__method_new(ix, recv, method, composed, "cc_snake_method", cc__sym_origin(ix, cs), cs, 0);
            cc_buf_printf(tried, " %s", composed);
        }
    }

    /* 5. the bare-name tier: a declared `method(T, ...)` is callable as `x.method(...)` */
    cs = cc__sym_find_kind(ix, method, CC_SYM_FUNC);
    if (cs && cs->type && cs->type->kind == CC_T_FUNC && cs->type->params.n > 0 && cs->type->params.items[0]->type) {
        CcParam *pm = cs->type->params.items[0];
        CcType *p0 = pm->type;
        int p0_ptr = p0->kind == CC_T_POINTER;
        CcType *base = p0_ptr ? p0->base : p0;
        int recv_ptr = (recv_shape & CC_RECV_PTR) != 0;
        int recv_const = (recv_shape & CC_RECV_CONST) != 0;
        int p0_const = base && (base->quals & CC_Q_CONST) != 0;
        int names_it = base && (cc__same_type(ix, cc_index_canon(ix, base), recv->name) ||
                                (p0_ptr && recv_ptr && strcmp(cc_index_canon(ix, base), "void") == 0));
        /* the address of a receiver may be taken; a pointer is never
         * dereferenced, and const is never dropped */
        if (names_it && !(recv_ptr && !p0_ptr) && !(recv_const && p0_ptr && !p0_const))
            return cc__method_new(ix, recv, method, method, "bare_name", cc__sym_origin(ix, cs), cs, 0);
        cc_buf_printf(tried, "; candidate %s (bare): declared, but first parameter '%s%s%s' does not take '%s'",
                      method, cc_index_canon(ix, p0), pm->name ? " " : "", pm->name ? pm->name : "", recv->name);
    }

    /* 6. a typedef alias uses the aliased type's methods */
    if (recv->sym && recv->sym->kind == CC_SYM_TYPE && recv->sym->decl &&
        (recv->sym->decl->kind == CC_D_TYPEDEF || recv->sym->decl->kind == CC_D_TYPEVIEW) && recv->sym->type) {
        CcType *t = recv->sym->type;
        if (t->kind == CC_T_NAMED || t->kind == CC_T_POINTER || t->kind == CC_T_GENERIC || t->kind == CC_T_SLICE ||
            t->kind == CC_T_RESULT || t->kind == CC_T_CHAN) {
            m = cc__resolve_via(ix, recv, method, tried, depth, t, "via typedef", recv_shape);
            if (m) return m;
        }
    }

    /* 7. `@typeview as:` faces: retry through an embedded field */
    {
        const char **faces = NULL;
        size_t n = cc__view_faces(ix, recv->name, &faces), i;
        const CcType *st = n ? cc__struct_of(ix, recv->name, 0) : NULL;
        for (i = 0; i < n && st; i++) {
            const CcField *f;
            for (f = st->fields; f; f = f->next) {
                if (!f->name || strcmp(f->name, faces[i]) != 0) continue;
                m = cc__resolve_via(ix, recv, method, tried, depth, f->type, cc_arena_printf(ix->arena, "via @typeview as: %s ->", f->name), 0u);
                if (m) return m;
            }
        }
    }

    /* 8. `.ufcs_sink`: the last resort of a dynamic family */
    {
        size_t i;
        CcHookReg *best = NULL;
        for (i = 0; i < ix->hooks.n; i++) {
            CcHookReg *h = ix->hooks.items[i];
            if (!h->sink_fn) continue;
            if ((h->any) || (h->family && cc__has_prefix(recv->name, h->base) && strlen(recv->name) > strlen(h->base)) ||
                (!h->family && strcmp(recv->name, h->base) == 0))
                best = h;
        }
        if (best) {
            cs = cc__callee_sym(ix, best->sink_fn);
            if (cs) return cc__method_new(ix, recv, method, best->sink_fn, "ufcs_sink", cc_arena_printf(ix->arena, "@typehooks on %s .ufcs_sink = %s -> %s", best->subject, best->sink_fn, cc__sym_origin(ix, cs)), cs, 0);
            cc_buf_printf(tried, " %s (.ufcs_sink: not declared)", best->sink_fn);
        }
    }
    return NULL;
}

size_t cc_index_methods_of(CcIndex *ix, CcTypeInfo *info, CcMethod ***out) {
    CC_LIST(CcMethod) list = {0};
    size_t i;
    CcHookReg *hook = cc__hook_match(ix, info->name, 1);
    const char *prefix_type = cc_arena_printf(ix->arena, "%s_", info->name);
    const char *prefix_snake = cc_arena_printf(ix->arena, "%s_", cc_index_snake(ix, info->name));
    if (hook) {
        const CcUfcsRule *r;
        for (r = hook->rules; r; r = r->next) {
            CcMethod *m = CC_NEW(ix->arena, CcMethod);
            m->method = r->method;
            m->callee = r->callee;
            m->source = "typehooks";
            m->origin = cc_arena_printf(ix->arena, "@typehooks on %s .ufcs = %s", hook->subject, hook->ufcs_fn);
            m->sym = cc__callee_sym(ix, r->callee);
            m->recv_by_ptr = r->by_value ? 0 : cc__first_param_is_pointer(m->sym);
            CC_LIST_PUSH(ix->arena, &list, m);
        }
    }
    for (i = 0; i < ix->syms.n; i++) {
        CcSym *s = ix->syms.items[i];
        const char *rest = NULL;
        const char *source = NULL;
        CcMethod *m;
        if (s->kind != CC_SYM_FUNC && s->kind != CC_SYM_MACRO) continue;
        if (s->kind == CC_SYM_MACRO && !(s->macro && s->macro->function_like)) continue;
        if (hook && hook->ufcs_prefix && cc__has_prefix(s->name, hook->ufcs_prefix)) {
            rest = s->name + strlen(hook->ufcs_prefix);
            source = "typehooks";
        } else if (cc__has_prefix(s->name, prefix_type)) {
            rest = s->name + strlen(prefix_type);
            source = info->is_result ? "Result" : info->ufcs_registered_by ? "DECL_UFCS" : "Type_method";
        } else if (strcmp(prefix_snake, prefix_type) != 0 && cc__has_prefix(s->name, prefix_snake)) {
            rest = s->name + strlen(prefix_snake);
            source = cc__has_prefix(prefix_snake, "cc_") ? "cc_snake_method" : "snake_method";
        }
        if (!rest || !*rest) continue;
        m = CC_NEW(ix->arena, CcMethod);
        m->method = cc__in(ix, rest);
        m->callee = s->name;
        m->source = source;
        m->origin = cc__sym_origin(ix, s);
        m->sym = s;
        m->recv_by_ptr = cc__first_param_is_pointer(s);
        CC_LIST_PUSH(ix->arena, &list, m);
    }
    *out = list.items;
    return list.n;
}

CcMethod *cc_index_method_recv(CcIndex *ix, CcTypeInfo *recv, CcName method, unsigned recv_shape, const char **candidates) {
    CcBuf tried;
    CcMethod *m;
    cc_buf_init(&tried);
    m = cc__resolve(ix, recv, method, &tried, 0, recv_shape);
    if (candidates) {
        if (m) *candidates = NULL;
        else {
            CcBuf b;
            CcMethod **have = NULL;
            size_t n = cc_index_methods_of(ix, recv, &have), i, shown = 0;
            cc_buf_init(&b);
            cc_buf_printf(&b, "no UFCS method '%s' for receiver type '%s'; tried:%s", method, recv->name, tried.len ? tried.data : " nothing");
            if (n) {
                cc_buf_push_str(&b, "; the type has:");
                for (i = 0; i < n && shown < 24; i++) {
                    size_t k;
                    int dup = 0;
                    for (k = 0; k < i; k++)
                        if (strcmp(have[k]->method, have[i]->method) == 0) dup = 1;
                    if (dup) continue;
                    cc_buf_printf(&b, " %s", have[i]->method);
                    shown++;
                }
                if (shown < n) cc_buf_push_str(&b, " ...");
            }
            *candidates = cc_arena_strdup(ix->arena, b.data);
            cc_buf_free(&b);
        }
    }
    cc_buf_free(&tried);
    return m;
}
