/* Host-C parser on a FileTape-shaped token stream. See docs/c-parser.md. */
#ifndef CC_CPARSE_H
#define CC_CPARSE_H

#include <stddef.h>

/* Same numbering as cc/shadow/pp_tape.cch TokKind. */
typedef enum {
    CP_TOK_IDENT = 1,
    CP_TOK_NUM,
    CP_TOK_STR,
    CP_TOK_CHR,
    CP_TOK_PUNCT,
    CP_TOK_EOF
} CpTokKind;

typedef struct {
    CpTokKind kind;
    const char *ptr; /* borrow into the byte buffer (optional; offset is truth) */
    size_t len;
    int file_id;
    size_t offset;
} CpTok;

typedef enum {
    CP_TU = 1,
    CP_DEFINE,
    CP_IF,
    CP_FIELD,
    CP_STRUCT,
    CP_FUNC,
    CP_DIR,     /* #undef / #pragma / #include / #error / #warning / #line */
    CP_TYPEDEF, /* `typedef` that is not `typedef struct {` */
    CP_ENUM,    /* `enum { … };` / `enum Tag { … };` / `enum Tag;` */
    CP_DATA,    /* file-scope object / C++ `template` blob in a #if arm */
    CP_STMT     /* C statement in a function body */
} CpKind;

typedef enum {
    CP_IF_IFDEF = 1,
    CP_IF_IFNDEF,
    CP_IF_CONST,
    CP_IF_EXPR
} CpIfForm;

typedef struct CpNode CpNode;

struct CpNode {
    CpKind kind;
    const char *src;
    int start;
    int end;
    char *name;
    char *attr;
    CpIfForm if_form;
    int if_const;
    int else_start;
    int else_end;
    int endif_start;
    int endif_end;
    CpNode **then_kids;
    int nthen;
    CpNode **else_kids;
    int nelse;
    CpNode **kids;
    int nkid;
    int live;
    int is_elif; /* 1 = this CP_IF is an `#elif` arm (no own `#endif`) */
    int is_func; /* CP_DEFINE: 1 = function-like `NAME(` with no space */
};

typedef struct {
    char *msg;
    int line;
    int col;
} CpError;

typedef struct {
    char **names;
    char **bodies; /* object-like replacement, or `(params) body` if is_func */
    int *is_func;
    int n;
    int cap;
    const char *file_dir; /* quote-include dir for __has_include */
} CpEnv;

typedef struct {
    const char *file_dir;
    const char *const *inc_dirs;
    int ninc;
    int unknown_call; /* 1 = NAME(...) is 0 (parse syntax); 0 = fail */
} CpIfOpts;

typedef struct {
    char *src;
    int len;
    CpNode *root;
    CpError err;
} CpParse;

/* FileTape-shaped lex. `src` must already be line-spliced. */
int cparse_lex(const char *src, int len, CpTok **out, int *n);
/* Splice `\`+newline, then lex. `*bytes` is the owned spliced buffer;
 * toks borrow it. Caller frees *bytes and *toks. */
int cparse_lex_bytes(const char *src, int len, char **bytes, int *blen,
                     CpTok **toks, int *n);
int cparse_dump_tokens(const char *src, const CpTok *toks, int n,
                       char **out, size_t *len);

/* Evaluate directives + expand macros (C11 hide-set, #, ##, __VA_ARGS__).
 * Dead #if arms are omitted. Predefs in `predef` are object-like `1`.
 * `*out` toks borrow `src` or `*arena`. Caller frees *out and *arena. */
int cparse_expand(const char *src, int len, const CpTok *in, int nin,
                  const CpEnv *predef, CpTok **out, int *nout, char **arena,
                  char **err, const char *file_dir);

/* toks[].offset must be into `src`. We copy `src`; we do not keep `toks`. */
int cparse_tokens(const char *path, const char *src, int len,
                  const CpTok *toks, int ntoks, CpParse *out);
int cparse_buffer(const char *path, const char *src, int len, CpParse *out);
int cparse_file(const char *path, CpParse *out);
void cparse_free(CpParse *p);

void cpenv_init(CpEnv *e);
void cpenv_free(CpEnv *e);
int cpenv_define(CpEnv *e, const char *name); /* object-like `1` */
int cpenv_define_body(CpEnv *e, const char *name, const char *body);
int cpenv_define_func(CpEnv *e, const char *name, const char *rest);
int cpenv_undef(CpEnv *e, const char *name);
int cpenv_has(const CpEnv *e, const char *name);

/* Expand macros in a #if operand (same algorithm as --expand), then
 * evaluate. Leftover idents are 0. Empty expansion is 0. */
int cparse_eval_if_expr(const char *expr, const CpEnv *env, long long *out,
                        char *err, int errcap);

void cparse_evaluate(CpNode *n, CpEnv *env, int parent_live);
int cparse_dump_preserve(const CpNode *n, char **out, size_t *len);
int cparse_dump_evaluate(const CpNode *n, char **out, size_t *len);

/* Overlay seam: flatten C field / fn shapes for shadow. */
enum { CP_FLAT_FIELD = 1, CP_FLAT_PPDIR = 2, CP_FLAT_FUNC = 3 };

typedef struct {
    int kind;
    char name[128];
    char text[512];
    int start;
    int end;
} CpFlat;

int cparse_field_group(const char *path, const char *src, int len,
                       const CpTok *toks, int ntoks, CpParse *out);
int cparse_flat_from_parse(const CpParse *p, CpFlat *out, int cap, int *n);
int cparse_flat_fields(const char *src, int len, const CpTok *toks, int ntoks,
                       CpFlat *out, int cap, int *n, char *err, int errcap);
int cparse_match_func(const char *src, int len, const CpTok *toks, int ntoks,
                      char *name, int ncap, int *lbrace_i, char *err,
                      int errcap);
int cparse_func_stmt_spans(const char *src, int len, const CpTok *toks,
                             int ntoks, int *starts, int *ends, int cap, int *n,
                             char *err, int errcap);

/* C11 #if integer expression after macro expansion. Bare leftover
 * idents are 0. `defined` / `__has_*` are not expanded. */
int cparse_eval_if_toks(const char *src, const CpTok *toks, int n,
                        int (*isdef)(void *ctx, const char *name, size_t len),
                        void *ctx, long long *out, char *err, int errcap);
int cparse_eval_if_toks_ex(const char *src, const CpTok *toks, int n,
                           int (*isdef)(void *ctx, const char *name, size_t len),
                           void *ctx, long long *out, char *err, int errcap,
                           const CpIfOpts *opts);

#endif
