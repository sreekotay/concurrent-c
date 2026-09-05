/* Memory for the clean lowerer: a bump arena and a growable byte buffer.
 * Everything the lowerer allocates for one unit lives in one CcArena and is
 * freed at once; nothing is sized in advance. */
#ifndef CC_LOWER_MEM_H
#define CC_LOWER_MEM_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef struct CcArenaChunk CcArenaChunk;
typedef struct CcArena {
    CcArenaChunk *cur;      /* chunk list, newest first */
    size_t chunk_bytes;     /* default chunk size (grows for big requests) */
    size_t total;           /* bytes handed out, for diagnostics */
} CcArena;

void  cc_arena_init(CcArena *a, size_t chunk_bytes);
void *cc_arena_alloc(CcArena *a, size_t n, size_t align); /* zeroed; aborts on OOM */
void *cc_arena_dup(CcArena *a, const void *p, size_t n);
char *cc_arena_strdup(CcArena *a, const char *s);
char *cc_arena_strndup(CcArena *a, const char *s, size_t n);
char *cc_arena_printf(CcArena *a, const char *fmt, ...);
void  cc_arena_free(CcArena *a);

#define CC_NEW(a, T) ((T *)cc_arena_alloc((a), sizeof(T), _Alignof(T)))
#define CC_NEW_N(a, T, n) ((T *)cc_arena_alloc((a), sizeof(T) * (size_t)(n), _Alignof(T)))

/* Growable byte buffer (NUL-terminated at all times). Backed by malloc so it
 * can outlive an arena when handed to the caller; cc_buf_take gives the bytes
 * away. */
typedef struct CcBuf {
    char *data;
    size_t len;
    size_t cap;
} CcBuf;

void  cc_buf_init(CcBuf *b);
void  cc_buf_reserve(CcBuf *b, size_t extra);
void  cc_buf_push(CcBuf *b, const void *p, size_t n);
void  cc_buf_push_str(CcBuf *b, const char *s);
void  cc_buf_push_char(CcBuf *b, char c);
void  cc_buf_printf(CcBuf *b, const char *fmt, ...);
void  cc_buf_vprintf(CcBuf *b, const char *fmt, va_list ap);
char *cc_buf_take(CcBuf *b);       /* returns data (caller frees), resets b */
void  cc_buf_free(CcBuf *b);

/* Growable pointer array in an arena: CC_LIST(T) is `T **items; size_t n, cap;`. */
#define CC_LIST(T) struct { T **items; size_t n; size_t cap; }
void *cc__list_grow(CcArena *a, void *items, size_t *cap, size_t n, size_t elem);
#define CC_LIST_PUSH(a, l, x) do { \
    if ((l)->n == (l)->cap) (l)->items = cc__list_grow((a), (l)->items, &(l)->cap, (l)->n, sizeof(*(l)->items)); \
    (l)->items[(l)->n++] = (x); } while (0)

/* Whole-file read: NUL-terminated copy in the arena, or NULL with errno. */
char *cc_read_file(CcArena *a, const char *path, size_t *len_out);

#endif
