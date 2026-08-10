/* Whitelist AST types, keywords, Parser, and token/spell helpers.
 * Included by pp_ast.cch before pp_ast_parse.cch. */
#pragma once

/* Emit-plan factory registry (libshadow_comptime) — used when mangling
 * snake `family::[T]` spellings such as py_expose::[Counter]. Declarations
 * stay even when stub bodies below are omitted under SHADOW_HAVE_LIBTCC. */
int cc_emit_plan_has_generic_factory(const char* name);
int cc_emit_plan_generic_factory_names_csv(char* out, size_t cap);
size_t cc_emit_plan_comptime_fragment_count(void);
const char* cc_emit_plan_comptime_fragment_text(size_t frag_index);
/* Lower-then-TCC field registry (libshadow_comptime). */
int cc_ct_field_reg_put(const char* type_name, const char* const* names,
                        const char* const* types, const int* is_as, int n);

/* ---- closed parse keywords (comptime perfect hash) ---------------------- */

typedef enum {
    SHADOW_KW_NONE = 0,
    SHADOW_KW_INT,
    SHADOW_KW_VOID,
    SHADOW_KW_RETURN,
    SHADOW_KW_TYPEDEF,
    SHADOW_KW_STRUCT,
    SHADOW_KW_PRINTLN,
    SHADOW_KW_ERRHANDLER,
    SHADOW_KW_DESTROY,
    SHADOW_KW_SPAWN,
    SHADOW_KW_ERR,
    SHADOW_KW_IF,
    SHADOW_KW_STRING,
    SHADOW_KW_DEFER,
        SHADOW_KW_STATIC,
    SHADOW_KW_CHAR,
    SHADOW_KW_FOR,
    SHADOW_KW_WHILE,
    SHADOW_KW_WITH_DEADLINE,
    SHADOW_KW_AS,
    SHADOW_KW_ASYNC,
    SHADOW_KW_AWAIT,
    SHADOW_KW_CREATE,
    SHADOW_KW_DETACH,
    SHADOW_KW_BOOL,
    SHADOW_KW_SIZE_T,
    SHADOW_KW_INLINE,
    SHADOW_KW_CONST,
    SHADOW_KW_ORDERED,
    SHADOW_KW_BREAK,
    SHADOW_KW_CONTINUE,
    SHADOW_KW_ELSE,
    SHADOW_KW_DO,
    SHADOW_KW_EPRINTLN,
    SHADOW_KW_ENUM,
    SHADOW_KW_SWITCH
} ShadowKwKind;

typedef struct {
    ShadowKwKind kind;
} ShadowKwSpec;

typedef struct {
    const char* key;
    ShadowKwSpec value;
} ShadowKwEntry;

           
                                         
                                     
                                       
                                           
                                             
                                           
                                             
                                                   
                                             
                                         
                                     
                                   
                                           
                                         
                                           
                                       
                                     
                                         
                                                         
                                   
                                         
                                         
                                           
                                           
                                       
                                           
                                           
                                         
                                             
                                         
                                               
                                       
                                   
                                               
                                       
                                           
      
                                                                             
 

/* Forward decl for in-header callers; body is TU-spliced from the map above. */
static const ShadowKwSpec* shadow_kw_get(CCSlice key);

static ShadowKwKind shadow_kw(Token t) {
    if (t.kind != TK_IDENT) return SHADOW_KW_NONE;
    const ShadowKwSpec* s = shadow_kw_get(t.spell);
    return s ? s->kind : SHADOW_KW_NONE;
}

/* ---- typedef scopes + tiny AST ------------------------------------------ */

/* Large tables are heap-backed (parser_ensure_storage) so stack Parser stays
 * small in spike smokes. Grammar TUs need headroom beyond the old 512. */
/* redis_idiomatic TU (db + resp + mem includes) exceeds 2048 nodes. */
enum { SCOPE_CAP = 32, NAME_CAP = 64, AST_CAP = 8192 };

typedef struct {
    char names[NAME_CAP][128];
    int n;
} Scope;

typedef enum {
    AST_TYPEDEF_INT,   /* typedef int Name; */
    AST_VAR_INT,       /* int Name; */
    AST_PTR_DECL,      /* TypeName * Name; */
    AST_MUL_EXPR,      /* Ident * Ident;  (expression statement) */
    /* Bare expression statement: a=expr text (closure/fn bodies). */
    AST_EXPR_STMT,
    AST_STRUCT,        /* struct Name { ...fields... } */
    AST_FIELD_INT,     /* int Name; inside struct */
    /* Concurrent-C surface (post-expand; stage 1 kept digraphs intact) */
    AST_SLICE_VAR,     /* int[:] Name; */
    /* CHAN_VAR: a=name b=capacity c=">"|"<" d=elem(ty[*])
     * e: "" | "o" | "t:TOPO" | "o:TOPO" (ordered / schedule) */
    AST_CHAN_VAR,
    /* RESULT_FN: a=fname, b=err, c=ok, d=param text or "" for void; kids=body */
    AST_RESULT_FN,
    AST_CLOSURE_LIT,   /* () => { }   (expression-level; counted as external) */
    AST_AT_STMT,       /* @ident ... ;  (a = attr name, e.g. defer/async) */
    AST_FN,            /* int Name(void) { stmts } — a=name, kids=stmts */
    AST_RETURN_INT,    /* return N; — a=literal */
    /* return cc_ok|cc_err(...); — a="ok"|"err", b=args text (inside parens) */
    AST_RETURN_CC,
    /* return <expr> [!>]; — a=expr text; e="bang" when trailing !> unwrap */
    AST_RETURN_EXPR,
    AST_ERRHANDLER,    /* @errhandler(Type bind) …; a=type b=bind c=handler or ""+body */
    AST_PRINTLN_BANG,  /* println("...") !>; — a=string spell including quotes */
    /* println("...") !>(bind) { stmts }; — a=string b=bind, body=handler stmts */
    AST_PRINTLN_BANG_BIND,
    /* println(@string(`tpl`, @scratch)) !>; — a=tpl body without ticks */
    AST_PRINTLN_TPL,
    AST_ERR_FWD,       /* @err(bind); — a=bind name; emits handler(bind) */
    /* lhs =<! expr [: default] @err …;  OR  expr @err …;
     * a=lhs|"" b=fallible expr c="colon"|"" d=local bind spec e=default */
    AST_ERR_SYNTAX,
    /* Type * name = callee(NULL) !> @destroy { stmts }; — a=type b=name c=callee */
    AST_NURSERY_DESTROY,
    /* recv->spawn(() => [caps] { stmts }); — a=recv b=spawn d=id e=caps
     * caps: "x" / "&x" / "@safe&x" (intentional share); empty → inferred */
    AST_SPAWN_CLOSURE,
    /* Ident ( NUM ) ; — e.g. usleep(100000); a=callee b=literal */
    AST_CALL_NUM,
    /* Ident ( args ) ; — a=callee b=args text */
    AST_CALL_ARGS,
    /* if (cond) stmt — a=cond text, body[0]=stmt */
    AST_IF,
    /* { stmts } — body=stmts */
    AST_BLOCK,
    /* int name = call(...) !>|?> …; a=name b=call c=mode d=bind e=default */
    AST_VAR_UNWRAP,
    /* Type * name = call(...); — a=type b=name c=call */
    AST_PTR_INIT,
    /* call(...) !>; — a=call text */
    AST_STMT_UNWRAP,
    /* Ok!>(Err) name = call(...); — a=name b=err c=ok d=call */
    AST_RESULT_LOCAL,
    /* @defer[(ok|err)] stmt — c=""|"ok"|"err", body=stmts */
    AST_DEFER,
    /* Type* name = call !>|?> … [@destroy]; a=type b=name c=call d=mode e=bind/default
     * mode: bang_nobind|bang_block|bang_eh|qmark[+_D|_Dbare]
     * body=handler; dbody=destroy body (if _D) — not kids_storage (fn mid-append) */
    AST_PTR_UNWRAP,
    /* (void)ident; — a=ident */
    AST_VOID_CAST,
    /* static Ret name(params) { … } — a=ret b=name c=params; kids=stmts OR d=raw body */
    AST_STATIC_FN,
    /* static char name[] = "lit"; — a=name b=string lit incl quotes */
    AST_STATIC_ARR,
    /* Type[*] name = expr @destroy [ {D} ]; — a=type b=name c=init
     * d="*"|"", e unused; cleanup from registered destroy / arena_release */
    AST_VAL_DESTROY,
    /* for (hdr) { stmts } — a=hdr text, body=stmts */
    AST_FOR,
    /* Type[*] name = expr; — a=type b=name c=expr d="*"|"", UFCS rewritten on emit */
    AST_TYPED_INIT,
    /* Type name[:] = {…}; — a=elem ty b=name c=brace init */
    AST_SLICE_INIT,
    /* recv.method[::[T]](args); — a=recv b=method c=args
     * e: "" | "->" | "::T" | "->::T" (arrow + optional member type args)
     * d: bang|bang_block (STMT only) */
    AST_UFCS_STMT,
    /* recv.method[::[T]](args) as expression — same a/b/c/e; d unused */
    AST_UFCS_EXPR,
    /* lhs = rhs; — a=lhs b=rhs; optional kids[0]=AST_UFCS_EXPR when RHS is UFCS */
    AST_ASSIGN,
    /* Type name; — a=type b=name (no init) */
    AST_VAR_DECL,
    /* while (cond) { stmts } — a=cond, body=stmts */
    AST_WHILE,
    /* @with_deadline(expr) [as name] { stmts } — a=expr b=bind|"" */
    AST_WITH_DEADLINE,
    /* name++; — a=name */
    AST_INC,
    /* typedef struct Tag { fields } Alias; — a=tag|"" b=alias; kids=fields
     * Opaque form (no `{…}`): nkids==0, a=tag, b=alias → `typedef struct Tag Alias;` */
    AST_TYPEDEF_STRUCT,
    /* field: Type name[, name2]*; — a=type b=names csv */
    AST_FIELD_SIMPLE,
    /* @async Ret name(params) { stmts } — a=ret b=name c=params; kids=body */
    AST_ASYNC_FN,
    /* static Type[*] name[[N]] [= expr]; — a=type b=name c="*"|"[N]"|"" d=init|"" */
    AST_STATIC_VAR,
    /* typedef Ret (*Name)(params); — a=ret b=name c=params */
    AST_TYPEDEF_FN_PTR,
    /* Ret[*] name(params); — a=ret(+optional *) b=name c=params */
    AST_FN_PROTO,
    /* break; */
    AST_BREAK,
    /* continue; */
    AST_CONTINUE,
    /* goto label; — a=label */
    AST_GOTO,
    /* label: — a=label (statement position) */
    AST_LABEL,
    /* do { stmts } while (cond); — a=cond, body=stmts */
    AST_DO_WHILE,
    /* const Type* name[] = { init_list }; — a=type b=name c=init text */
    AST_GLOBAL_ARR,
    /* typedef enum [Tag]? { enumerators } Alias; — a=tag b=alias d=body */
    AST_TYPEDEF_ENUM,
    /* switch (expr) { body } — a=expr d=body text */
    AST_SWITCH,
    /* File-scope macro invocation kept as text — a=full `NAME(...);` span.
     * Beachhead: CC_DECL_RESULT_SPEC(...); */
    AST_RAW_LINE,
    /* typedef Elem[~N [ordered] >|<] Alias; — a=alias b=cap c=">"|"<" d=elem */
    AST_TYPEDEF_CHAN,
    /* CC_GENERIC_FACTORY(Name, N) { return @emit(`tpl`, arena); }
     * a=family b=arity d=template (ticks stripped). */
    AST_GENERIC_FACTORY
} AstKind;

typedef struct AstNode AstNode;
struct AstNode {
    AstKind kind;
    /* Long exprs / @string unwrap args / grammar rows (py + templates need >512). */
    char a[2048];
    char b[2048];
    /* Params / long spans — nursery lower_c protos and bang binders. */
    char c[2048];
    /* Long raw spans: static-fn / switch / enum bodies (was 256; errno maps overflow). */
    char d[4096];
    char e[2048];
    /* Attachment roles (never mix):
     *   kids  — open parent lists (fn/struct members still being filled)
     *   body  — nested stmt lists (block / if / loop / bang bodies)
     *   dbody — destroy bodies + UFCS/create/closure attachments on a stmt */
    AstNode** kids;
    int nkids;
    /* On-node (not kids_storage): nested under fn bodies that also use kids.
     * Cap covers fat Result-fn switches (redis execute / switch_body_cap smoke). */
    AstNode* body[256];
    int nbody;
    AstNode* dbody[64];
    int ndbody;
    /* Source trivia: gap before this node (comments / blank lines) + line indent.
     * lead spans tape bytes [lead_off, lead_off+lead_len); indent is the
     * whitespace after the last newline in that gap (hand-lower nest base).
     * tok_off is the first token of the node (for #line provenance). */
    int file_id;
    size_t lead_off;
    size_t lead_len;
    size_t tok_off;
    /* Opaque switch/enum-style body larger than d[]: tape bytes
     * [span_off, span_off+span_len) in file_id. d[0]==0 when set. */
    size_t span_off;
    size_t span_len;
    char indent[64];
};

typedef struct {
    Token* toks;
    int n;
    int i;
    TapeCache* cache; /* for file:line diagnostics */
    Scope scopes[SCOPE_CAP];
    int ns;
    AstNode* nodes; /* heap — AST_CAP entries */
    int nn;
    AstNode** kids_storage; /* heap — AST_CAP * 4 */
    int nkstore;
    AstNode** tu_items; /* heap — AST_CAP */
    int ntu;
    int err;
    char err_msg[192];
    int in_async; /* 1 while parsing @async fn body (await strip vs reject). */
    unsigned pending_fn_attrs; /* @noblock/@blocking before fn decl */
    /* 1 = parse_stmt returns NULL on miss without parser_fail (static-fn soft). */
    int soft_stmt;
    int own_storage; /* 1 if nodes/kids/tu were calloc'd by ensure_storage */
} Parser;

static Token p_peek(Parser* p);

/* Process-local tables so stack `Parser p = {0}` stays small. One active
 * parse_tu at a time (shadow_lower / spike smokes). TCC has no _Thread_local. */
static AstNode* g_parser_nodes;
static AstNode** g_parser_kids;
static AstNode** g_parser_tu;
static int g_parser_storage_cap;

static int parser_ensure_storage(Parser* p) {
    if (!p) return 0;
    if (p->nodes && p->kids_storage && p->tu_items) return 1;
    if (!g_parser_nodes || g_parser_storage_cap < AST_CAP) {
        free(g_parser_nodes);
        free(g_parser_kids);
        free(g_parser_tu);
        g_parser_nodes = (AstNode*)calloc((size_t)AST_CAP, sizeof(AstNode));
        g_parser_kids =
            (AstNode**)calloc((size_t)AST_CAP * 4, sizeof(AstNode*));
        g_parser_tu = (AstNode**)calloc((size_t)AST_CAP, sizeof(AstNode*));
        g_parser_storage_cap =
            (g_parser_nodes && g_parser_kids && g_parser_tu) ? AST_CAP : 0;
        if (!g_parser_storage_cap) {
            free(g_parser_nodes);
            free(g_parser_kids);
            free(g_parser_tu);
            g_parser_nodes = NULL;
            g_parser_kids = NULL;
            g_parser_tu = NULL;
            return 0;
        }
    }
    p->nodes = g_parser_nodes;
    p->kids_storage = g_parser_kids;
    p->tu_items = g_parser_tu;
    p->own_storage = 0; /* TLS — do not free with Parser */
    return 1;
}

static void parser_fail(Parser* p, Token at, const char* msg) {
    /* Sticky: keep the first (most specific) diagnostic. */
    if (p->err) return;
    p->err = 1;
    snprintf(p->err_msg, sizeof(p->err_msg), "%s", msg);
    diag_at(p->cache, at, msg);
}

/* Named capacity overflow — always includes the numeric limit. */
static void parser_fail_cap(Parser* p, Token at, const char* what, int limit) {
    char msg[192];
    snprintf(msg, sizeof(msg), "%s capacity exceeded (%d)",
             what ? what : "parser", limit);
    parser_fail(p, at, msg);
}

/* Cap for body[] (256) / dbody[] (64). `what` names the list, e.g. "block". */
enum { SHADOW_BODY_CAP = 256, SHADOW_DBODY_CAP = 64 };
static void parser_fail_body_cap(Parser* p, Token at, const char* what) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s too large for shadow beachhead (cap %d)",
             what ? what : "body", SHADOW_BODY_CAP);
    parser_fail(p, at, msg);
}
static void parser_fail_dbody_cap(Parser* p, Token at, const char* what) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s too large for shadow beachhead (cap %d)",
             what ? what : "dbody", SHADOW_DBODY_CAP);
    parser_fail(p, at, msg);
}

static void scope_push(Parser* p) {
    if (p->ns >= SCOPE_CAP) {
        parser_fail_cap(p, p_peek(p), "typedef scope depth", SCOPE_CAP);
        return;
    }
    p->scopes[p->ns++] = (Scope){0};
}

static void __attribute__((unused)) scope_pop(Parser* p) {
    if (p->ns > 0) p->ns--;
}

static int scope_is_typedef(Parser* p, CCSlice name) {
    for (int s = p->ns - 1; s >= 0; s--) {
        Scope* sc = &p->scopes[s];
        for (int i = 0; i < sc->n; i++) {
            if (spell_eq(name, sc->names[i])) return 1;
        }
    }
    return 0;
}

static int scope_add_typedef(Parser* p, CCSlice name) {
    if (p->ns == 0) scope_push(p);
    Scope* sc = &p->scopes[p->ns - 1];
    if (name.len >= 128) {
        Token at = p_peek(p);
        if (p->i < p->n) at = p->toks[p->i];
        parser_fail_cap(p, at, "typedef name", 127);
        return 0;
    }
    if (sc->n >= NAME_CAP) {
        parser_fail_cap(p, p_peek(p), "typedef names per scope", NAME_CAP);
        return 0;
    }
    memcpy(sc->names[sc->n], name.ptr, name.len);
    sc->names[sc->n][name.len] = 0;
    sc->n++;
    return 1;
}

/* From libshadow_comptime (emit_plan). Stubs for spike TUs that do not link
 * the comptime engine. Omitted when SHADOW_HAVE_LIBTCC — the production
 * shadow_lower link always provides strong defs, and TinyCC does not honor
 * __attribute__((weak)) (reports "defined twice"). */
#if !defined(SHADOW_HAVE_LIBTCC)
#if defined(__GNUC__) && !defined(__TINYC__)
#define CC__EMIT_PLAN_STUB __attribute__((weak))
#else
#define CC__EMIT_PLAN_STUB static
#endif
CC__EMIT_PLAN_STUB size_t cc_emit_plan_comptime_fragment_count(void) {
    return 0;
}
CC__EMIT_PLAN_STUB const char* cc_emit_plan_comptime_fragment_text(
    size_t frag_index) {
    (void)frag_index;
    return NULL;
}
CC__EMIT_PLAN_STUB int cc_emit_plan_has_generic_factory(const char* name) {
    (void)name;
    return 0;
}
CC__EMIT_PLAN_STUB int cc_emit_plan_generic_factory_names_csv(char* out,
                                                              size_t cap) {
    if (out && cap) out[0] = 0;
    return 0;
}
#undef CC__EMIT_PLAN_STUB
#endif

/* Seed a CamelCase / mangled typedef name into the current scope (idempotent). */
static void shadow_seed_typedef_name(Parser* p, const char* name, size_t n) {
    char buf[128];
    CCSlice s;
    if (!p || !name || n == 0 || n >= sizeof(buf)) return;
    if (!(name[0] >= 'A' && name[0] <= 'Z')) return;
    memcpy(buf, name, n);
    buf[n] = 0;
    s.ptr = buf;
    s.len = n;
    if (!scope_is_typedef(p, s)) (void)scope_add_typedef(p, s);
}

/* Scan comptime-emitted C for `typedef … Alias;` and seed Alias into scope.
 * Late splice still supplies the real definitions into the emit buffer. */
static void shadow_scan_fragment_typedefs(Parser* p, const char* text) {
    const char* s = text;
    size_t n;
    size_t i;
    if (!p || !text) return;
    n = strlen(text);
    for (i = 0; i + 7 < n;) {
        int bound_l = (i == 0) ||
                      !((s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                        (s[i - 1] >= 'a' && s[i - 1] <= 'z') ||
                        (s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '_');
        int bound_r = (i + 7 >= n) ||
                      !((s[i + 7] >= 'A' && s[i + 7] <= 'Z') ||
                        (s[i + 7] >= 'a' && s[i + 7] <= 'z') ||
                        (s[i + 7] >= '0' && s[i + 7] <= '9') || s[i + 7] == '_');
        if (bound_l && bound_r && memcmp(s + i, "typedef", 7) == 0) {
            size_t j = i + 7;
            size_t last_s = 0, last_e = 0;
            int depth = 0;
            while (j < n) {
                char c = s[j];
                if (c == '{') {
                    depth++;
                    j++;
                    continue;
                }
                if (c == '}') {
                    if (depth) depth--;
                    j++;
                    continue;
                }
                if (c == '"' || c == '\'' || c == '`') {
                    char q = c;
                    j++;
                    while (j < n && s[j] != q) {
                        if (s[j] == '\\' && j + 1 < n) j++;
                        j++;
                    }
                    if (j < n) j++;
                    continue;
                }
                if (depth == 0 && c == ';') {
                    if (last_e > last_s)
                        shadow_seed_typedef_name(p, s + last_s, last_e - last_s);
                    j++;
                    break;
                }
                if (depth == 0 &&
                    ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     c == '_')) {
                    size_t a = j++;
                    while (j < n &&
                           ((s[j] >= 'A' && s[j] <= 'Z') ||
                            (s[j] >= 'a' && s[j] <= 'z') ||
                            (s[j] >= '0' && s[j] <= '9') || s[j] == '_'))
                        j++;
                    last_s = a;
                    last_e = j;
                    continue;
                }
                j++;
            }
            i = j;
            continue;
        }
        i++;
    }
}

static void shadow_seed_comptime_emitted_types(Parser* p) {
    size_t i, n;
    n = cc_emit_plan_comptime_fragment_count();
    for (i = 0; i < n; i++)
        shadow_scan_fragment_typedefs(p, cc_emit_plan_comptime_fragment_text(i));
}

/* When tool umbrellas (c_pp_spike / shadow_build) passthrough instead of
 * splice, whitelist parse still needs their type names for the driver TU.
 * Real layouts come from the emitted #include .h faces. */
static void shadow_seed_tool_umbrella_types(Parser* p) {
    static const char* names[] = {
        "AstNode",       "CEmit",          "FileTape", "Frame",
        "Macro",         "Parser",         "Pp",       "ShadowEmitKind",
        "ShadowFileSig", "ShadowHostOpts", "TapeCache", "TokKind",
        "Token",         "TokBuild",       "Scope",
        NULL
    };
    size_t i;
    for (i = 0; names[i]; i++)
        shadow_seed_typedef_name(p, names[i], strlen(names[i]));
}

/* Seed a spelled type when it is a plain identifier (Pair_int_double). */
static void shadow_seed_spelled_type_name(Parser* p, const char* tytxt) {
    size_t nl;
    size_t k;
    int ok;
    if (!tytxt || !tytxt[0]) return;
    nl = strlen(tytxt);
    ok = 1;
    for (k = 0; ok && k < nl; k++) {
        char c = tytxt[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            ok = 0;
    }
    if (ok) shadow_seed_typedef_name(p, tytxt, nl);
}

static void slice_to(char* dst, size_t cap, CCSlice s) {
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    memcpy(dst, s.ptr, n);
    dst[n] = 0;
}

/* Byte length of toks[i0 .. i1_excl) without copying (0 when empty/invalid). */
static size_t __attribute__((unused)) span_text_len(Parser* p, int i0, int i1_excl) {
    int i;
    if (!p || i0 < 0 || i1_excl > p->n || i0 >= i1_excl) return 0;
    {
        Token a = p->toks[i0];
        Token b = p->toks[i1_excl - 1];
        size_t start = a.offset;
        size_t end = b.offset + b.spell.len;
        FileTape* ft = tape_by_id(p->cache, a.file_id);
        if (ft && ft->bytes && end <= ft->len && end >= start)
            return end - start;
    }
    {
        size_t need = 0;
        for (i = i0; i < i1_excl; i++) {
            if (i > i0) need++;
            need += p->toks[i].spell.len;
        }
        return need;
    }
}

/* Exact source span covering toks[i0 .. i1_excl).
 * Fails loud (no silent truncate) when the span does not fit `cap`. */
static int span_text(Parser* p, int i0, int i1_excl, char* dst, size_t cap) {
    size_t need = 0;
    int i;
    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    if (!p || i0 < 0 || i1_excl > p->n || i0 >= i1_excl) return 1;
    {
        Token a = p->toks[i0];
        Token b = p->toks[i1_excl - 1];
        size_t start = a.offset;
        size_t end = b.offset + b.spell.len;
        FileTape* ft = tape_by_id(p->cache, a.file_id);
        int same_file = 1;
        size_t spell_sum = 0;
        /* Macro expand keeps define-site offsets on body tokens while the
         * call's `)` / `!>` stay at the use site — a byte memcpy then slurps
         * everything between (including `@errhandler`). Spell-concat instead. */
        for (i = i0; i < i1_excl; i++) {
            Token ti = p->toks[i];
            spell_sum += ti.spell.len;
            if (ti.file_id != a.file_id) {
                same_file = 0;
                break;
            }
            if (i > i0) {
                Token prev = p->toks[i - 1];
                size_t prev_end = prev.offset + prev.spell.len;
                /* Object-like macro bodies keep define-site offsets; the next
                 * use-site token can sit far later in the same file. */
                if (ti.offset < prev.offset || ti.offset > prev_end + 512u) {
                    same_file = 0;
                    break;
                }
            }
        }
        if (same_file && end >= start &&
            (end - start) > spell_sum + (size_t)(i1_excl - i0) * 64u + 64u)
            same_file = 0;
        if (same_file && ft && ft->bytes && end <= ft->len && end >= start) {
            size_t n = end - start;
            if (n + 1 > cap) {
                if (!p->err) {
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                             "source span exceeds %zu-byte AST text field "
                             "(need %zu)",
                             cap - 1, n);
                    parser_fail(p, a, msg);
                }
                return 0;
            }
            memcpy(dst, ft->bytes + start, n);
            dst[n] = 0;
            return 1;
        }
    }
    for (i = i0; i < i1_excl; i++) {
        if (i > i0) need++;
        need += p->toks[i].spell.len;
    }
    if (need + 1 > cap) {
        if (!p->err) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "source span exceeds %zu-byte AST text field (need %zu)",
                     cap - 1, need);
            parser_fail(p, p->toks[i0], msg);
        }
        return 0;
    }
    {
        size_t o = 0;
        for (i = i0; i < i1_excl; i++) {
            if (i > i0) dst[o++] = ' ';
            memcpy(dst + o, p->toks[i].spell.ptr, p->toks[i].spell.len);
            o += p->toks[i].spell.len;
        }
        dst[o] = 0;
    }
    return 1;
}

static Token p_peek(Parser* p) {
    if (p->i >= p->n) return (Token){ .kind = TK_EOF };
    return p->toks[p->i];
}

static Token p_next(Parser* p) {
    if (p->i >= p->n) return (Token){ .kind = TK_EOF };
    return p->toks[p->i++];
}

static int p_accept(Parser* p, TokKind k, const char* lit) {
    Token t = p_peek(p);
    if (!tok_eq(t, k, lit)) return 0;
    p_next(p);
    return 1;
}

/* Skip a `(...)` starting at p->i on `(`. Returns 1 and leaves i past `)`. */
static int skip_parens(Parser* p) {
    if (!tok_eq(p_peek(p), TK_PUNCT, "(")) return 0;
    int depth = 0;
    while (p->i < p->n) {
        Token t = p_next(p);
        if (tok_eq(t, TK_PUNCT, "(")) depth++;
        else if (tok_eq(t, TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) return 1;
        }
    }
    return 0;
}

static AstNode* ast_new(Parser* p, AstKind k) {
    if (p->nn >= AST_CAP) {
        parser_fail_cap(p, p_peek(p), "AST node table", AST_CAP);
        return NULL;
    }
    AstNode* n = &p->nodes[p->nn++];
    memset(n, 0, sizeof(*n));
    n->kind = k;
    return n;
}

/* Push onto kids_storage; diagnose when the table is full. */
static int ast_kids_push(Parser* p, AstNode* child) {
    if (!p || !child) return 0;
    if (p->nkstore >= AST_CAP * 4) {
        parser_fail_cap(p, p_peek(p), "AST kids storage", AST_CAP * 4);
        return 0;
    }
    p->kids_storage[p->nkstore++] = child;
    return 1;
}

/* Attach leading trivia + line indent from tape using token index `start_i`.
 * Sticky: lead_off/lead_len/tok_off/file_id are set once here for emit + diags.
 * Emit must print them — never re-derive comments after string rewrite. */
static void shadow_attach_lead(Parser* p, AstNode* n, int start_i) {
    if (!n || !p || start_i < 0 || start_i >= p->n) return;
    Token t = p->toks[start_i];
    n->file_id = t.file_id;
    n->tok_off = t.offset;
    if (start_i > 0) {
        Token prev = p->toks[start_i - 1];
        if (prev.file_id == t.file_id) {
            size_t end = prev.offset + prev.spell.len;
            FileTape* pft = tape_by_id(p->cache, prev.file_id);
            /* Injected tokens (stage2 umbrella #include → <.h>) keep a tape
             * offset at the original `"….h"` string but point spell at a
             * side buffer. Treating spell.len as a tape advance walks into
             * the next line and leaves a lead like `de` (from `include`),
             * which emit glues into `de#line`. End at EOL on the tape. */
            if (pft && pft->bytes && prev.spell.ptr &&
                (prev.spell.ptr < pft->bytes ||
                 prev.spell.ptr >= pft->bytes + pft->len)) {
                end = prev.offset;
                while (end < pft->len && pft->bytes[end] != '\n') end++;
                if (end < pft->len) end++;
            }
            n->lead_off = end;
            n->lead_len = (t.offset >= end) ? (t.offset - end) : 0;
        }
    } else {
        n->lead_off = 0;
        n->lead_len = t.offset;
    }
    n->indent[0] = 0;
    FileTape* ft = tape_by_id(p->cache, n->file_id);
    if (!ft || !ft->bytes) return;
    size_t i = t.offset;
    while (i > 0 && ft->bytes[i - 1] != '\n') i--;
    size_t j = 0;
    while (i < t.offset && j + 1 < sizeof(n->indent)) {
        char c = ft->bytes[i++];
        if (c == ' ' || c == '\t') n->indent[j++] = c;
        else break;
    }
    n->indent[j] = 0;
}

static int parse_struct_fields(Parser* p, AstNode* st);
static AstNode* parse_external(Parser* p);
static AstNode* parse_stmt(Parser* p);
static AstNode* parse_switch_stmt(Parser* p);
static AstNode* parse_at_stmt(Parser* p);
static AstNode* parse_with_deadline(Parser* p);
static AstNode* parse_inc(Parser* p);
static AstNode* parse_async_fn(Parser* p);
static AstNode* parse_static_var(Parser* p);
static AstNode* parse_result_fn(Parser* p);

/* Peek type prefix "int" then optional CC shape before a name. */
static int peek_int_base(Parser* p) { return shadow_kw(p_peek(p)) == SHADOW_KW_INT; }
static int peek_c_int_type_end(Parser* p, int start);
static int peek_slice_brack_end(Parser* p, int lb, int lim, int* out_unique);

/* Exclusive end after Result ok-type base + optional `[:]` / `[:!]`, or -1. */
static int peek_result_ok_type_end(Parser* p, int start) {
    int j;
    Token t;
    if (!p || start < 0 || start >= p->n) return -1;
    j = peek_c_int_type_end(p, start);
    if (j < 0) {
        t = p->toks[start];
        if (!(t.kind == TK_IDENT || shadow_kw(t) == SHADOW_KW_VOID ||
              shadow_kw(t) == SHADOW_KW_BOOL || shadow_kw(t) == SHADOW_KW_INT ||
              shadow_kw(t) == SHADOW_KW_CHAR || shadow_kw(t) == SHADOW_KW_SIZE_T))
            return -1;
        j = start + 1;
    }
    /* Type-position slice sugar: `char[:] !>(E)` / `T[:!] !>(E)`. */
    if (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "[")) {
        int end = peek_slice_brack_end(p, j, p->n, NULL);
        if (end < 0) return -1;
        j = end;
    }
    return j;
}

static int peek_result_shape(Parser* p) {
    int j;
    if (!p || p->i >= p->n) return 0;
    j = peek_result_ok_type_end(p, p->i);
    if (j < 0) return 0;
    while (j < p->n && tok_eq(p->toks[j], TK_PUNCT, "*")) j++;
    return j < p->n && tok_eq(p->toks[j], TK_PUNCT, "!>");
}

/* After ok base spelling: consume `[:]` / `[:!]` into okty (type-position). */
static int shadow_parse_result_ok_slice_suffix(Parser* p, char* okty, size_t cap) {
    size_t al;
    int unique = 0;
    if (!p || !okty || !cap) return 0;
    if (!tok_eq(p_peek(p), TK_PUNCT, "[")) return 1;
    if (p->i + 2 >= p->n || !tok_eq(p->toks[p->i + 1], TK_PUNCT, ":")) return 0;
    if (tok_eq(p->toks[p->i + 2], TK_PUNCT, "]")) {
        unique = 0;
    } else if (p->i + 3 < p->n && tok_eq(p->toks[p->i + 2], TK_PUNCT, "!") &&
               tok_eq(p->toks[p->i + 3], TK_PUNCT, "]")) {
        unique = 1;
    } else
        return 0;
    al = strlen(okty);
    if (unique) {
        if (al + 4 >= cap) return 0;
        memcpy(okty + al, "[:!]", 5);
    } else {
        if (al + 3 >= cap) return 0;
        memcpy(okty + al, "[:]", 4);
    }
    p_next(p); /* [ */
    p_next(p); /* : */
    if (unique) p_next(p); /* ! */
    p_next(p); /* ] */
    return 1;
}

/* Parse-time Result mangle: `char[:]` → CCSlice (emit uses rewrite_slice_types). */
static void shadow_result_ok_ty_host(char* okty, size_t cap) {
    size_t n;
    if (!okty || !cap || !okty[0]) return;
    n = strlen(okty);
    if (strcmp(okty, "char[:]") == 0 || strcmp(okty, "char[:0]") == 0 ||
        strcmp(okty, "char[0:]") == 0 || strcmp(okty, "char[::]") == 0) {
        snprintf(okty, cap, "CCSlice");
        return;
    }
    if (strcmp(okty, "char[:!]") == 0 || strcmp(okty, "char[:0!]") == 0) {
        snprintf(okty, cap, "CCSliceUnique");
        return;
    }
    if (n > 4 && strcmp(okty + n - 4, "[:!]") == 0) {
        snprintf(okty, cap, "CCSliceUnique");
        return;
    }
    if (n > 3 && strcmp(okty + n - 3, "[:]") == 0) {
        char elem[96];
        size_t el = n - 3;
        if (el >= sizeof(elem)) el = sizeof(elem) - 1;
        memcpy(elem, okty, el);
        elem[el] = 0;
        snprintf(okty, cap, "CCSlice_%s", elem);
    }
}

/* Beachhead Result name: CCResult_<ok>_<err> (int/CCError etc.; no full mangle). */
static void ast_result_name(const char* ok, const char* err, char* out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "CCResult_%s_%s", ok ? ok : "void", err ? err : "CCError");
}

/* Ident :: [ … ] — advances nothing; returns end index after `]` or -1. */
static int peek_generic_type_end(Parser* p, int start) {
    if (start + 3 >= p->n) return -1;
    if (p->toks[start].kind != TK_IDENT) return -1;
    if (!tok_eq(p->toks[start + 1], TK_PUNCT, "::")) return -1;
    if (!tok_eq(p->toks[start + 2], TK_PUNCT, "[")) return -1;
    int j = start + 2;
    int depth = 0;
    while (j < p->n) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "[")) depth++;
        else if (tok_eq(t, TK_PUNCT, "]")) {
            depth--;
            if (depth == 0) return j + 1;
        }
        j++;
    }
    return -1;
}

/* unsigned? (long long | long | short | int | char | …) — exclusive end, or -1. */
static int peek_c_int_type_end(Parser* p, int start) {
    int i = start;
    int saw = 0;
    if (!p || start < 0 || start >= p->n) return -1;
    if (p->toks[i].kind == TK_IDENT && spell_eq(p->toks[i].spell, "unsigned")) {
        i++;
        saw = 1;
    }
    if (i >= p->n) return saw ? i : -1;
    if (p->toks[i].kind == TK_IDENT && spell_eq(p->toks[i].spell, "long")) {
        i++;
        if (i < p->n && p->toks[i].kind == TK_IDENT &&
            spell_eq(p->toks[i].spell, "long"))
            i++;
        else if (i < p->n && shadow_kw(p->toks[i]) == SHADOW_KW_INT)
            i++;
        return i;
    }
    if (p->toks[i].kind == TK_IDENT && spell_eq(p->toks[i].spell, "short")) {
        i++;
        if (i < p->n && shadow_kw(p->toks[i]) == SHADOW_KW_INT) i++;
        return i;
    }
    if (saw) {
        ShadowKwKind k = shadow_kw(p->toks[i]);
        if (k == SHADOW_KW_INT || k == SHADOW_KW_CHAR || k == SHADOW_KW_BOOL ||
            k == SHADOW_KW_VOID || k == SHADOW_KW_SIZE_T ||
            p->toks[i].kind == TK_IDENT)
            return i + 1;
        return i; /* bare `unsigned` */
    }
    return -1;
}

/* Exclusive end after `]` for a slice marker at `lb` (`[`), or -1.
 * Allows `:`, lexer `::` (nested char[::]/[:::]), digits, and at most one
 * `!` (unique). */
static int peek_slice_brack_end(Parser* p, int lb, int lim, int* out_unique) {
    int j;
    int saw_colon = 0;
    int bang = 0;
    if (!p || lb < 0 || lb >= lim || !tok_eq(p->toks[lb], TK_PUNCT, "["))
        return -1;
    if (out_unique) *out_unique = 0;
    for (j = lb + 1; j < lim; j++) {
        Token t = p->toks[j];
        if (tok_eq(t, TK_PUNCT, "]")) {
            if (!saw_colon) return -1;
            if (out_unique) *out_unique = bang > 0;
            return j + 1;
        }
        /* `:` or fused `::` from the punct lexer (nested slice sugar). */
        if (tok_eq(t, TK_PUNCT, ":") || tok_eq(t, TK_PUNCT, "::")) {
            saw_colon = 1;
            continue;
        }
        if (tok_eq(t, TK_PUNCT, "!")) {
            bang++;
            if (bang > 1) return -1;
            continue;
        }
        if (t.kind == TK_NUM) continue;
        return -1;
    }
    return -1;
}

/* Exclusive end of a C type token span at `start`, or -1. */
static int peek_slice_elem_type_end(Parser* p, int start, int lim) {
    int iend;
    ShadowKwKind k;
    if (!p || start < 0 || start >= lim) return -1;
    iend = peek_c_int_type_end(p, start);
    if (iend > start && iend <= lim) return iend;
    k = shadow_kw(p->toks[start]);
    if (k == SHADOW_KW_CHAR || k == SHADOW_KW_INT || k == SHADOW_KW_BOOL ||
        k == SHADOW_KW_SIZE_T || k == SHADOW_KW_VOID)
        return start + 1;
    if (p->toks[start].kind == TK_IDENT) return start + 1;
    return -1;
}

/* Mangle element type tokens [ty0, ty1) → CCSlice name suffix (spaces → `_`). */
static void ast_mangle_slice_elem(Parser* p, int ty0, int ty1, char* dst,
                                  size_t cap) {
    size_t o = 0;
    int i;
    if (!dst || !cap) return;
    dst[0] = 0;
    for (i = ty0; i < ty1 && o + 1 < cap; i++) {
        Token t = p->toks[i];
        size_t n = t.spell.len;
        if (o && o + 1 < cap) dst[o++] = '_';
        if (o + n >= cap) n = cap - 1 - o;
        memcpy(dst + o, t.spell.ptr, n);
        o += n;
        dst[o] = 0;
    }
}

/* Spell a slice type covering tokens [start, *out_end): type-position
 * `T[:…]` / `char[:0]` / `char[::]` or declarator `T name[:…]`.
 * On declarator form, `*out_end` is after `]` and `piece` is `CCSlice[_T] name`. */
static int ast_try_spell_slice_at(Parser* p, int start, int lim, char* piece,
                                  size_t cap, int* out_end) {
    int ty1;
    int j;
    int unique = 0;
    int mend;
    int name_i = -1;
    char elem[64];
    char sty[96];
    if (!p || !piece || !cap || !out_end || start < 0 || start >= lim)
        return 0;
    ty1 = peek_slice_elem_type_end(p, start, lim);
    if (ty1 < 0) return 0;
    j = ty1;
    if (j < lim && p->toks[j].kind == TK_IDENT) {
        /* Declarator `T name[:…]` — only when `[` follows the name. */
        if (j + 1 < lim && tok_eq(p->toks[j + 1], TK_PUNCT, "[")) {
            name_i = j;
            j++;
        }
    }
    if (j >= lim || !tok_eq(p->toks[j], TK_PUNCT, "[")) return 0;
    mend = peek_slice_brack_end(p, j, lim, &unique);
    if (mend < 0) return 0;
    if (shadow_kw(p->toks[start]) == SHADOW_KW_CHAR && ty1 == start + 1)
        snprintf(sty, sizeof(sty), "%s", unique ? "CCSliceUnique" : "CCSlice");
    else {
        ast_mangle_slice_elem(p, start, ty1, elem, sizeof(elem));
        if (!elem[0]) return 0;
        snprintf(sty, sizeof(sty), "CCSlice_%s", elem);
    }
    if (name_i >= 0) {
        char nm[64];
        slice_to(nm, sizeof(nm), p->toks[name_i].spell);
        snprintf(piece, cap, "%s %s", sty, nm);
    } else {
        snprintf(piece, cap, "%s", sty);
    }
    *out_end = mend;
    return 1;
}

/* Forward: nested Family::[args] inside type-args uses instance spelling. */
static int ast_spell_type_tokens(Parser* p, int start, int end, char* dst,
                                 size_t cap);

/* Compact type args inside `[`…`]` (token range exclusive of brackets).
 * `char[:]` → CCSlice; trailing `*` → `ptr` (int* → intptr).
 * Nested surface generics recurse through ast_spell_type_tokens so Vec
 * canonicalizes to CCVec_T (joining bare idents would yield Vec_T). */
static void ast_compact_type_args(Parser* p, int a0, int a1, char* compact,
                                  size_t cap) {
    size_t ci = 0;
    int i;
    compact[0] = 0;
    for (i = a0; i < a1 && ci + 1 < cap; ) {
        Token t = p->toks[i];
        int gend = -1;
        char piece[96];
        if (tok_eq(t, TK_PUNCT, ",")) {
            compact[ci++] = '_';
            compact[ci] = 0;
            i++;
            continue;
        }
        /* char[:…] / T[:…] inside type-args */
        if (ast_try_spell_slice_at(p, i, a1, piece, sizeof(piece), &gend) &&
            gend > i && strchr(piece, ' ') == NULL) {
            size_t n = strlen(piece);
            if (ci + n >= cap) n = cap - 1 - ci;
            memcpy(compact + ci, piece, n);
            ci += n;
            compact[ci] = 0;
            i = gend;
            continue;
        }
        /* Nested Family::[args] — instance spelling (Vec → CCVec_…). */
        if ((gend = peek_generic_type_end(p, i)) > i && gend <= a1) {
            if (ast_spell_type_tokens(p, i, gend, piece, sizeof(piece))) {
                size_t n = strlen(piece);
                while (n > 0 && piece[n - 1] == '*')
                    piece[--n] = 0;
                if (ci && compact[ci - 1] != '_' && ci + 1 < cap)
                    compact[ci++] = '_';
                if (ci + n >= cap) n = cap - 1 - ci;
                memcpy(compact + ci, piece, n);
                ci += n;
                compact[ci] = 0;
                i = gend;
                continue;
            }
            /* Spell refused — leave tokens for the ident join path. */
            gend = -1;
        }
        if (t.kind == TK_IDENT || shadow_kw(t) == SHADOW_KW_INT ||
            shadow_kw(t) == SHADOW_KW_BOOL || shadow_kw(t) == SHADOW_KW_CHAR ||
            shadow_kw(t) == SHADOW_KW_SIZE_T || shadow_kw(t) == SHADOW_KW_VOID) {
            size_t n = t.spell.len;
            int stars = 0;
            if (ci && compact[ci - 1] != '_' && ci + 1 < cap)
                compact[ci++] = '_';
            if (ci + n >= cap) n = cap - 1 - ci;
            memcpy(compact + ci, t.spell.ptr, n);
            ci += n;
            compact[ci] = 0;
            i++;
            while (i < a1 && tok_eq(p->toks[i], TK_PUNCT, "*")) {
                stars++;
                i++;
            }
            while (stars-- > 0 && ci + 3 < cap) {
                memcpy(compact + ci, "ptr", 3);
                ci += 3;
                compact[ci] = 0;
            }
            continue;
        }
        i++;
    }
}

/* Structured Map/Vec/Family/char[:] → C emit spelling (from tokens, not blob rewrite). */
static int ast_spell_type_tokens(Parser* p, int start, int end, char* dst,
                                 size_t cap) {
    char family[64];
    char compact[96];
    int slice_end = -1;
    if (!p || !dst || !cap || start < 0 || end > p->n || start >= end)
        return 0;
    /* char[:…] / T[:…] (type-position only for this helper). */
    if (ast_try_spell_slice_at(p, start, end, dst, cap, &slice_end) &&
        slice_end == end && strchr(dst, ' ') == NULL)
        return 1;
    if (peek_generic_type_end(p, start) != end) return 0;
    slice_to(family, sizeof(family), p->toks[start].spell);
    ast_compact_type_args(p, start + 3, end - 1, compact, sizeof(compact));
    if (!compact[0]) return 0;
    if (strcmp(family, "Map") == 0)
        snprintf(dst, cap, "Map_%s*", compact);
    else if (strcmp(family, "ArrayMap") == 0)
        snprintf(dst, cap, "ArrayMap_%s*", compact);
    else if (strcmp(family, "Vec") == 0)
        snprintf(dst, cap, "CCVec_%s", compact);
    else if (strcmp(family, "map_new") == 0 ||
             strcmp(family, "cc_map_new") == 0)
        snprintf(dst, cap, "Map_%s_new", compact);
    else if (strcmp(family, "array_map_new") == 0)
        snprintf(dst, cap, "array_map_new_%s", compact);
    else if (strcmp(family, "array_map_new_count") == 0)
        snprintf(dst, cap, "array_map_new_count_%s", compact);
    else if (strcmp(family, "vec_new") == 0 ||
             strcmp(family, "cc_vec_new") == 0)
        snprintf(dst, cap, "CCVec_%s_new", compact);
    else if (strcmp(family, "CCVec") == 0)
        snprintf(dst, cap, "CCVec_%s", compact);
    else {
        size_t fl = strlen(family);
        /* snake_make::[A,B] → Family_A_B_make (lru_cache_make → LruCache_…). */
        if (fl > 5 && strcmp(family + fl - 5, "_make") == 0) {
            char base[64];
            char pascal[64];
            size_t bl = fl - 5;
            size_t bi, po = 0;
            int up = 1;
            if (bl >= sizeof(base)) bl = sizeof(base) - 1;
            memcpy(base, family, bl);
            base[bl] = 0;
            for (bi = 0; base[bi] && po + 1 < sizeof(pascal); bi++) {
                char c = base[bi];
                if (c == '_') {
                    up = 1;
                    continue;
                }
                if (up && c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                pascal[po++] = c;
                up = 0;
            }
            pascal[po] = 0;
            if (!pascal[0]) snprintf(pascal, sizeof(pascal), "%s", base);
            snprintf(dst, cap, "%s_%s_make", pascal, compact);
        } else if (family[0] >= 'A' && family[0] <= 'Z') {
            /* PascalCase type-style: Pair::[T] / Box::[T] / user factories. */
            snprintf(dst, cap, "%s_%s", family, compact);
        } else if (cc_emit_plan_has_generic_factory(family)) {
            /* Registered snake factory (py_expose::[T] → py_expose_T). */
            snprintf(dst, cap, "%s_%s", family, compact);
        } else {
            /* Free snake name that is not a known ctor / factory — refuse
             * host invent (unknown_fn::[int] → unknown_fn_int). */
            char msg[192];
            char fams[512];
            snprintf(msg, sizeof(msg),
                     "unknown generic name '%s' before '::[...]'", family);
            parser_fail(p, p->toks[start], msg);
            if (cc_emit_plan_generic_factory_names_csv(fams, sizeof(fams)) > 0)
                fprintf(stderr,
                        "  note: registered generic factory families: %s\n",
                        fams);
            return 0;
        }
    }
    return 1;
}

/* Token spell matches bytes at its file offset (not a macro-body mint). */
static int ast_tok_verbatim(Parser* p, Token t) {
    FileTape* ft;
    if (!p || !t.spell.ptr || !t.spell.len) return 0;
    ft = tape_by_id(p->cache, t.file_id);
    if (!ft || !ft->bytes || t.offset + t.spell.len > ft->len) return 0;
    return memcmp(ft->bytes + t.offset, t.spell.ptr, t.spell.len) == 0;
}

/* Copy [lo, hi) when it is only whitespace (source gaps, incl. newlines). */
static int ast_copy_ws_gap(Parser* p, Token t, size_t lo, size_t hi, char* dst,
                           size_t* o, size_t cap) {
    FileTape* ft;
    size_t i;
    size_t n;
    if (hi <= lo) return 1;
    if (hi - lo > 512) return 0;
    ft = tape_by_id(p->cache, t.file_id);
    if (!ft || !ft->bytes || hi > ft->len) return 0;
    for (i = lo; i < hi; i++) {
        char c = ft->bytes[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return 0;
    }
    n = hi - lo;
    if (*o + n + 1 >= cap) return 0;
    memcpy(dst + *o, ft->bytes + lo, n);
    *o += n;
    dst[*o] = 0;
    return 1;
}

static int ast_append_spell(char* dst, size_t* o, size_t cap, CCSlice spell,
                            int need_sp) {
    size_t need = spell.len + (need_sp && *o ? 1 : 0);
    if (*o + need + 1 > cap) return 0;
    if (need_sp && *o) dst[(*o)++] = ' ';
    memcpy(dst + *o, spell.ptr, spell.len);
    *o += spell.len;
    dst[*o] = 0;
    return 1;
}

/* Like ast_append_spell, but diagnoses a named field-cap overflow once. */
static int ast_append_spell_diag(Parser* p, Token at, char* dst, size_t* o,
                                 size_t cap, CCSlice spell, int need_sp) {
    if (ast_append_spell(dst, o, cap, spell, need_sp)) return 1;
    if (p && !p->err) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "token spelling exceeds %zu-byte AST text field",
                 cap > 0 ? cap - 1 : 0);
        parser_fail(p, at, msg);
    }
    return 0;
}

static int ast_need_lex_space(char prev, char next) {
    int na = (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
             (next >= '0' && next <= '9') || next == '_';
    int pa = (prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
             (prev >= '0' && prev <= '9') || prev == '_';
    if (pa && na) return 1;
    /* `,const` / `;int` — keep C surfaces readable when gaps are non-ws. */
    if ((prev == ',' || prev == ';') && na) return 1;
    return 0;
}

/* @async name → return type (for `@await fname(...)` → `cc_block_on`). */
typedef struct {
    char name[64];
    char ret[64];
} ShadowAsyncFn;
static ShadowAsyncFn g_shadow_async_fns[64];
static int g_shadow_nasync_fns;

static void shadow_async_fn_reset(void) { g_shadow_nasync_fns = 0; }

static void shadow_async_fn_register(const char* name, const char* ret) {
    int i;
    if (!name || !name[0] || !ret || !ret[0] || g_shadow_nasync_fns >= 64)
        return;
    for (i = 0; i < g_shadow_nasync_fns; i++) {
        if (strcmp(g_shadow_async_fns[i].name, name) == 0) {
            snprintf(g_shadow_async_fns[i].ret, sizeof(g_shadow_async_fns[i].ret),
                     "%s", ret);
            return;
        }
    }
    snprintf(g_shadow_async_fns[g_shadow_nasync_fns].name,
             sizeof(g_shadow_async_fns[0].name), "%s", name);
    snprintf(g_shadow_async_fns[g_shadow_nasync_fns].ret,
             sizeof(g_shadow_async_fns[0].ret), "%s", ret);
    g_shadow_nasync_fns++;
}

static const char* shadow_async_fn_ret(const char* name) {
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_shadow_nasync_fns; i++) {
        if (strcmp(g_shadow_async_fns[i].name, name) == 0)
            return g_shadow_async_fns[i].ret;
    }
    return NULL;
}

/* Matching ')' index for a '(' at open, or -1. */
static int ast_matching_paren(Parser* p, int open, int end) {
    int depth = 0;
    int i;
    if (!p || open < 0 || open >= end || !tok_eq(p->toks[open], TK_PUNCT, "("))
        return -1;
    for (i = open; i < end; i++) {
        if (tok_eq(p->toks[i], TK_PUNCT, "(")) depth++;
        else if (tok_eq(p->toks[i], TK_PUNCT, ")")) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

/* Rebuild token range: keep local source gaps; spell generics/char[:] /
 * macro-expanded tokens from their spells (body offsets are not use-site). */
static int ast_spell_token_range(Parser* p, int start, int end, char* dst,
                                 size_t cap) {
    size_t o = 0;
    int i = start;
    size_t prev_end;
    int have_prev = 0;
    if (!dst || !cap) return 0;
    dst[0] = 0;
    if (!p || start < 0 || end > p->n || start >= end) return 0;
    while (i < end) {
        int gend = -1;
        char piece[160];
        Token t = p->toks[i];
        size_t tok_off = t.offset;
        int spelled = 0;
        piece[0] = 0;
        /* @await / bare await fname(...) → cc_block_on(Ret, fname(...)) when
         * fname is @async; otherwise strip (channel UFCS etc. already block in
         * sync context). @create → __cc_at_create (resolved at emit). */
        {
            int await_at = 0;
            int await_i = -1;
            if (tok_eq(t, TK_PUNCT, "@") && i + 1 < end &&
                shadow_kw(p->toks[i + 1]) == SHADOW_KW_AWAIT) {
                await_at = 1;
                await_i = i + 1;
            } else if (shadow_kw(t) == SHADOW_KW_AWAIT) {
                await_i = i;
            }
            if (await_i >= 0) {
                int j = await_i + 1;
                const char* ret = NULL;
                char fname[64];
                int call_close = -1;
                if (j < end && p->toks[j].kind == TK_IDENT && j + 1 < end &&
                    tok_eq(p->toks[j + 1], TK_PUNCT, "(")) {
                    slice_to(fname, sizeof(fname), p->toks[j].spell);
                    ret = shadow_async_fn_ret(fname);
                    if (ret)
                        call_close = ast_matching_paren(p, j + 1, end);
                }
                if (ret && call_close > j) {
                    char call[512];
                    char wrap[640];
                    if (!ast_spell_token_range(p, j, call_close + 1, call,
                                               sizeof(call)))
                        return 0;
                    /* Result returns: poll packs via malloc; copy out + free. */
                    if (strncmp(ret, "CCResult_", 9) == 0)
                        snprintf(wrap, sizeof(wrap),
                                 "({ %s* __cc_ap = (%s*)(void*)cc_block_on_intptr(%s); "
                                 "%s __cc_av; memset(&__cc_av, 0, sizeof(__cc_av)); "
                                 "if (__cc_ap) { __cc_av = *__cc_ap; free(__cc_ap); } "
                                 "__cc_av; })",
                                 ret, ret, call, ret);
                    else
                        snprintf(wrap, sizeof(wrap), "cc_block_on(%s, %s)", ret,
                                 call);
                    {
                        CCSlice ps = { .ptr = wrap, .len = strlen(wrap) };
                        if (have_prev && tok_off >= prev_end)
                            (void)ast_copy_ws_gap(p, t, prev_end, tok_off, dst,
                                                  &o, cap);
                        if (!ast_append_spell_diag(p, t, dst, &o, cap, ps,
                                                   o && ps.len &&
                                                       ast_need_lex_space(
                                                           dst[o - 1],
                                                           wrap[0])))
                            return 0;
                        if (ast_tok_verbatim(p, p->toks[call_close])) {
                            prev_end = p->toks[call_close].offset +
                                       p->toks[call_close].spell.len;
                            have_prev = 1;
                        }
                    }
                    i = call_close + 1;
                    continue;
                }
                /* Inside @async: wrap the following call/UFCS so emit can
                 * lower channel awaits to the sync Result surface
                 * (`bool !>(CCIoError)` via errno→Result at the await edge).
                 * Raw `*_task` / other calls stay `cc_block_on(intptr_t, …)`.
                 * Sync context: only @await known_async_fn(...) is valid. */
                if (!p->in_async) {
                    parser_fail(p, t,
                                "'await' is only valid inside @async functions");
                    return 0;
                }
                {
                    int call_close2 = -1;
                    char wrap[768];
                    int did = 0;
                    wrap[0] = 0;
                    /* await recv.send/recv(...) → Result-typed channel await. */
                    if (j + 3 < end && p->toks[j].kind == TK_IDENT &&
                        tok_eq(p->toks[j + 1], TK_PUNCT, ".") &&
                        p->toks[j + 2].kind == TK_IDENT &&
                        tok_eq(p->toks[j + 3], TK_PUNCT, "(")) {
                        char recv[64], meth[32], args[256];
                        call_close2 = ast_matching_paren(p, j + 3, end);
                        slice_to(recv, sizeof(recv), p->toks[j].spell);
                        slice_to(meth, sizeof(meth), p->toks[j + 2].spell);
                        if (call_close2 > j + 3)
                            (void)ast_spell_token_range(p, j + 4, call_close2,
                                                        args, sizeof(args));
                        else
                            args[0] = 0;
                        if (strcmp(meth, "send") == 0)
                            snprintf(wrap, sizeof(wrap),
                                     "cc_chan_result_from_errno((int)"
                                     "cc_block_on(intptr_t, "
                                     "cc_channel_send_task(%s, %s)))",
                                     recv, args);
                        else if (strcmp(meth, "recv") == 0)
                            snprintf(wrap, sizeof(wrap),
                                     "cc_chan_result_from_errno((int)"
                                     "cc_block_on(intptr_t, "
                                     "cc_channel_recv_task(%s, %s)))",
                                     recv, args);
                        else if (call_close2 > j) {
                            char inner[384];
                            if (ast_spell_token_range(p, j, call_close2 + 1,
                                                      inner, sizeof(inner)))
                                snprintf(wrap, sizeof(wrap),
                                         "cc_block_on(intptr_t, %s)", inner);
                        }
                        did = wrap[0] != 0;
                    } else if (j + 1 < end && p->toks[j].kind == TK_IDENT &&
                               tok_eq(p->toks[j + 1], TK_PUNCT, "(")) {
                        char inner[384];
                        call_close2 = ast_matching_paren(p, j + 1, end);
                        if (call_close2 > j &&
                            ast_spell_token_range(p, j, call_close2 + 1, inner,
                                                  sizeof(inner))) {
                            snprintf(wrap, sizeof(wrap),
                                     "cc_block_on(intptr_t, %s)", inner);
                            did = 1;
                        }
                    }
                    if (did && call_close2 > j) {
                        CCSlice ps = { .ptr = wrap, .len = strlen(wrap) };
                        if (have_prev && tok_off >= prev_end)
                            (void)ast_copy_ws_gap(p, t, prev_end, tok_off, dst,
                                                  &o, cap);
                        if (!ast_append_spell_diag(
                                p, t, dst, &o, cap, ps,
                                o && ps.len &&
                                    ast_need_lex_space(dst[o - 1], wrap[0])))
                            return 0;
                        if (ast_tok_verbatim(p, p->toks[call_close2])) {
                            prev_end = p->toks[call_close2].offset +
                                       p->toks[call_close2].spell.len;
                            have_prev = 1;
                        }
                        i = call_close2 + 1;
                        continue;
                    }
                }
                /* Strip unresolved await keyword (and optional leading @). */
                i = await_i + 1;
                (void)await_at;
                continue;
            }
            if (tok_eq(t, TK_PUNCT, "@") && i + 1 < end &&
                shadow_kw(p->toks[i + 1]) == SHADOW_KW_CREATE) {
                /* Dest-type resolves at emit (__cc_at_create → nursery/arena/…). */
                snprintf(piece, sizeof(piece), "__cc_at_create");
                gend = i + 2;
                spelled = 1;
            }
        }
        if (!spelled &&
            ast_try_spell_slice_at(p, i, end, piece, sizeof(piece), &gend) &&
            gend > i) {
            spelled = 1;
        } else if (!spelled && (gend = peek_generic_type_end(p, i)) > i &&
                   gend <= end) {
            /* Member `recv.meth::[T](` keeps surface tokens (type formal).
             * Free-name `vec_new::[T](` / `Name::[T]` still mangles. */
            int member_targs = 0;
            if (gend < end && tok_eq(p->toks[gend], TK_PUNCT, "(") &&
                i > start) {
                Token prev = p->toks[i - 1];
                if (tok_eq(prev, TK_PUNCT, ".") || tok_eq(prev, TK_PUNCT, "->"))
                    member_targs = 1;
            }
            if (member_targs) {
                gend = -1;
            } else if (ast_spell_type_tokens(p, i, gend, piece, sizeof(piece))) {
                spelled = 1;
            } else {
                gend = -1;
            }
        }
        if (spelled) {
            CCSlice ps = { .ptr = piece, .len = strlen(piece) };
            if (have_prev && tok_off >= prev_end)
                (void)ast_copy_ws_gap(p, t, prev_end, tok_off, dst, &o, cap);
            if (!ast_append_spell_diag(p, t, dst, &o, cap, ps,
                                       o && ps.len &&
                                           ast_need_lex_space(dst[o - 1],
                                                              piece[0])))
                return 0;
            if (ast_tok_verbatim(p, p->toks[gend - 1])) {
                prev_end = p->toks[gend - 1].offset +
                           p->toks[gend - 1].spell.len;
                have_prev = 1;
            }
            i = gend;
        } else {
            int verbatim = ast_tok_verbatim(p, t);
            size_t tok_end = tok_off + t.spell.len;
            if (have_prev && verbatim && tok_off >= prev_end)
                (void)ast_copy_ws_gap(p, t, prev_end, tok_off, dst, &o, cap);
            if (!ast_append_spell_diag(p, t, dst, &o, cap, t.spell,
                                       o && t.spell.len &&
                                           ast_need_lex_space(
                                               dst[o - 1], t.spell.ptr[0])))
                return 0;
            /* Only advance the use-site cursor for verbatim tokens; macro
             * body tokens keep define-site offsets and must not rewind it. */
            if (verbatim) {
                prev_end = tok_end;
                have_prev = 1;
            }
            i++;
        }
    }
    return 1;
}

#include "pp_ast_autoblock.h"
