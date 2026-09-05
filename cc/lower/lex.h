/* Lexer for Concurrent-C: C11 pp-tokens plus the CC tokens, over one file.
 *
 * The token array is the source of truth for text: every byte of the input
 * belongs to exactly one token's leading trivia or to the token itself, so
 * printing `trivia + text` for every token reproduces the file exactly. The
 * parser never copies text; nodes hold token indexes.
 *
 * Positions are physical (byte offset in the file) and logical (the file and
 * line the user sees after `#line` rebasing). Logical positions are computed
 * from the offset on demand through the line table. */
#ifndef CC_LOWER_LEX_H
#define CC_LOWER_LEX_H
#include <stddef.h>
#include <stdint.h>
#include "mem.h"
#include "diag.h"

typedef enum CcTokKind {
    CC_TK_EOF = 0,
    CC_TK_IDENT,      /* identifier or C keyword (the parser decides) */
    CC_TK_AT_WORD,    /* `@` immediately followed by an identifier: text includes the `@` */
    CC_TK_AT,         /* a bare `@` not followed by an identifier (`name@(args)`, `@(`) */
    CC_TK_NUMBER,     /* pp-number: 0x1fULL, 1.5e3, 0b1010, 1'000 is not C11 and not lexed */
    CC_TK_CHAR,       /* 'a', L'a', u8'a' with escapes, text includes quotes and prefix */
    CC_TK_STRING,     /* "..." with prefix (L, u8, u, U); adjacent literals are separate tokens */
    CC_TK_TEMPLATE,   /* `...` backtick template; text includes the backticks; may span lines;
                         `${`, `$~tag{` and `\`` are left for the template parser */
    CC_TK_PUNCT,      /* punctuator; `punct` says which */
    CC_TK_PP,         /* a whole preprocessor line starting at `#` (after leading whitespace),
                         including a trailing backslash-continued run; text excludes the newline */
    CC_TK_ERROR       /* unterminated literal or stray byte; a diagnostic was emitted */
} CcTokKind;

/* Punctuators. C11 set plus the CC set. Multi-character ones win over their
 * prefixes (`!>` over `!`, `?>` over `?`, `=>` over `=`, `..` over `.`, but
 * `...` over `..`; `::` over `:`; `<-`/`->` as in C). */
typedef enum CcPunct {
    CC_P_NONE = 0,
    /* C */
    CC_P_LBRACKET, CC_P_RBRACKET, CC_P_LPAREN, CC_P_RPAREN, CC_P_LBRACE, CC_P_RBRACE,
    CC_P_DOT, CC_P_ARROW, CC_P_INC, CC_P_DEC, CC_P_AMP, CC_P_STAR, CC_P_PLUS, CC_P_MINUS,
    CC_P_TILDE, CC_P_BANG, CC_P_SLASH, CC_P_PERCENT, CC_P_SHL, CC_P_SHR, CC_P_LT, CC_P_GT,
    CC_P_LE, CC_P_GE, CC_P_EQ, CC_P_NE, CC_P_CARET, CC_P_PIPE, CC_P_ANDAND, CC_P_OROR,
    CC_P_QUESTION, CC_P_COLON, CC_P_SEMI, CC_P_ELLIPSIS, CC_P_ASSIGN, CC_P_MUL_ASSIGN,
    CC_P_DIV_ASSIGN, CC_P_MOD_ASSIGN, CC_P_ADD_ASSIGN, CC_P_SUB_ASSIGN, CC_P_SHL_ASSIGN,
    CC_P_SHR_ASSIGN, CC_P_AND_ASSIGN, CC_P_XOR_ASSIGN, CC_P_OR_ASSIGN, CC_P_COMMA,
    CC_P_HASH, CC_P_HASHHASH,
    /* CC */
    CC_P_UNWRAP,      /* !> */
    CC_P_UNWRAP_OR,   /* ?> */
    CC_P_FAT_ARROW,   /* => */
    CC_P_COLONCOLON,  /* :: */
    CC_P_DOTDOT,      /* .. */
    CC_P_BACKTICK,    /* only in error recovery; a well-formed template is one CC_TK_TEMPLATE */
    CC_P_DOLLAR,      /* $ outside a template */
    CC_P_COUNT
} CcPunct;

typedef struct CcToken {
    CcTokKind kind;
    CcPunct punct;         /* for CC_TK_PUNCT */
    uint32_t off;          /* byte offset of the token text in the file */
    uint32_t len;          /* length of the token text */
    uint32_t lead_off;     /* byte offset of the leading trivia (whitespace, comments, newlines) */
    uint32_t lead_len;     /* lead_off + lead_len == off */
    uint32_t line;         /* physical 1-based line of `off` */
    uint32_t col;          /* physical 1-based column of `off` (bytes, tabs count 1) */
    uint8_t at_line_start; /* only trivia between the previous newline and this token */
    uint8_t after_space;   /* lead_len > 0 */
} CcToken;

/* A `#line N "path"` (or `# N "path"`) directive, or a `CC_LN N "path"`
 * block-comment marker, rebases logical positions for everything after it. */
typedef struct CcLineMark {
    uint32_t off;          /* first byte of the line after the directive */
    uint32_t phys_line;    /* physical line number of that byte */
    uint32_t logical_line; /* logical line number of that byte */
    const char *path;      /* logical path (interned), NULL = the file's own path */
} CcLineMark;

typedef struct CcLexFile {
    const char *path;      /* as given */
    const char *src;       /* NUL-terminated; owned by the arena */
    uint32_t len;
    CcToken *toks;         /* toks[n_toks - 1] is CC_TK_EOF */
    uint32_t n_toks;
    uint32_t *line_starts; /* byte offset of each physical line start; line_starts[0] == 0 */
    uint32_t n_lines;
    CcLineMark *marks;     /* in offset order */
    uint32_t n_marks;
} CcLexFile;

/* Lex `src` (NUL-terminated, `len` bytes) as `path`. Never fails: bad input
 * produces CC_TK_ERROR tokens and diagnostics in `d`. */
CcLexFile *cc_lex(CcArena *a, CcDiag *d, const char *path, const char *src, size_t len);

/* Physical position of a byte offset. */
void cc_lex_phys(const CcLexFile *f, uint32_t off, uint32_t *line, uint32_t *col);
/* Logical position of a byte offset (after `#line` rebasing). */
CcLoc cc_lex_loc(const CcLexFile *f, uint32_t off);
/* The physical source line containing `off`, without its newline (arena copy). */
const char *cc_lex_line_text(CcArena *a, const CcLexFile *f, uint32_t off, uint32_t *col_out);

/* Token text helpers. */
static inline const char *cc_tok_text(const CcLexFile *f, const CcToken *t) { return f->src + t->off; }
int  cc_tok_is(const CcLexFile *f, const CcToken *t, const char *text);          /* exact text match */
int  cc_tok_is_punct(const CcToken *t, CcPunct p);
int  cc_tok_is_ident(const CcLexFile *f, const CcToken *t, const char *name);     /* CC_TK_IDENT with that text */
int  cc_tok_is_at(const CcLexFile *f, const CcToken *t, const char *word);        /* CC_TK_AT_WORD "@word" */
const char *cc_punct_text(CcPunct p);

#endif
