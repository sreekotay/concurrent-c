/* Bump arena, growable byte buffer, arena pointer lists, whole-file read.
 * See mem.h for the contract. Nothing here is sized by the input in advance:
 * chunks grow to fit a request, buffers double, and every allocation failure
 * aborts with a message. */
#include "mem.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_ARENA_DEFAULT_CHUNK ((size_t)64 * 1024)

struct CcArenaChunk {
    CcArenaChunk *next;   /* older chunk */
    size_t cap;           /* usable bytes after the header */
    size_t used;          /* bytes handed out from `data` */
    /* data follows, aligned to max_align_t */
};

/* Header size rounded so that the data area starts at a max_align_t
 * boundary within the malloc'd block. */
#define CC_CHUNK_HDR \
    ((sizeof(CcArenaChunk) + _Alignof(max_align_t) - 1) & ~(_Alignof(max_align_t) - 1))

static void cc__oom(const char *what, size_t n) {
    fprintf(stderr, "cc: out of memory: %s (%zu bytes)\n", what, n);
    fflush(stderr);
    abort();
}

static void *cc__xmalloc(const char *what, size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) cc__oom(what, n);
    return p;
}

void cc_arena_init(CcArena *a, size_t chunk_bytes) {
    a->cur = NULL;
    a->chunk_bytes = chunk_bytes ? chunk_bytes : CC_ARENA_DEFAULT_CHUNK;
    a->total = 0;
}

static CcArenaChunk *cc__chunk_new(CcArena *a, size_t need) {
    size_t cap = a->chunk_bytes;
    CcArenaChunk *c;
    if (need > cap) cap = need;
    if (cap > SIZE_MAX - CC_CHUNK_HDR) cc__oom("arena chunk", cap);
    c = (CcArenaChunk *)cc__xmalloc("arena chunk", CC_CHUNK_HDR + cap);
    c->next = a->cur;
    c->cap = cap;
    c->used = 0;
    a->cur = c;
    return c;
}

void *cc_arena_alloc(CcArena *a, size_t n, size_t align) {
    CcArenaChunk *c = a->cur;
    uintptr_t base, start;
    size_t pad;
    if (align == 0 || (align & (align - 1)) != 0) {
        fprintf(stderr, "cc: cc_arena_alloc: alignment %zu is not a power of two\n", align);
        abort();
    }
    if (n > SIZE_MAX - align) cc__oom("arena allocation", n);
    for (;;) {
        if (c) {
            base = (uintptr_t)c + CC_CHUNK_HDR;
            start = (base + c->used + align - 1) & ~(uintptr_t)(align - 1);
            pad = (size_t)(start - (base + c->used));
            if (c->used + pad + n <= c->cap) {
                c->used += pad + n;
                a->total += n;
                memset((void *)start, 0, n);
                return (void *)start;
            }
        }
        /* The data area is max_align_t-aligned; a stricter alignment may
         * need up to align - 1 bytes of padding, so reserve it. */
        c = cc__chunk_new(a, n + (align > _Alignof(max_align_t) ? align : 0));
    }
}

void *cc_arena_dup(CcArena *a, const void *p, size_t n) {
    void *q = cc_arena_alloc(a, n, _Alignof(max_align_t));
    if (n) memcpy(q, p, n);
    return q;
}

char *cc_arena_strndup(CcArena *a, const char *s, size_t n) {
    char *q;
    if (n == SIZE_MAX) cc__oom("strndup", n);
    q = (char *)cc_arena_alloc(a, n + 1, 1);
    if (n) memcpy(q, s, n);
    q[n] = 0;
    return q;
}

char *cc_arena_strdup(CcArena *a, const char *s) {
    return cc_arena_strndup(a, s, strlen(s));
}

static char *cc__arena_vprintf(CcArena *a, const char *fmt, va_list ap) {
    va_list ap2;
    int n;
    char *q;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        fprintf(stderr, "cc: cc_arena_printf: bad format \"%s\"\n", fmt);
        abort();
    }
    q = (char *)cc_arena_alloc(a, (size_t)n + 1, 1);
    vsnprintf(q, (size_t)n + 1, fmt, ap);
    return q;
}

char *cc_arena_printf(CcArena *a, const char *fmt, ...) {
    va_list ap;
    char *q;
    va_start(ap, fmt);
    q = cc__arena_vprintf(a, fmt, ap);
    va_end(ap);
    return q;
}

void cc_arena_free(CcArena *a) {
    CcArenaChunk *c = a->cur;
    while (c) {
        CcArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->cur = NULL;
    a->total = 0;
}

/* ---- CcBuf ---------------------------------------------------------- */

#define CC_BUF_MIN_CAP 32

void cc_buf_init(CcBuf *b) {
    b->data = (char *)cc__xmalloc("buffer", CC_BUF_MIN_CAP);
    b->data[0] = 0;
    b->len = 0;
    b->cap = CC_BUF_MIN_CAP;
}

void cc_buf_reserve(CcBuf *b, size_t extra) {
    size_t need, cap;
    char *p;
    if (extra > SIZE_MAX - 1 - b->len) cc__oom("buffer", extra);
    need = b->len + extra + 1;
    if (need <= b->cap && b->data) return;
    cap = b->cap ? b->cap : CC_BUF_MIN_CAP;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p) cc__oom("buffer", cap);
    if (!b->data) p[0] = 0;
    b->data = p;
    b->cap = cap;
}

void cc_buf_push(CcBuf *b, const void *p, size_t n) {
    cc_buf_reserve(b, n);
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void cc_buf_push_str(CcBuf *b, const char *s) {
    cc_buf_push(b, s, strlen(s));
}

void cc_buf_push_char(CcBuf *b, char c) {
    cc_buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

void cc_buf_vprintf(CcBuf *b, const char *fmt, va_list ap) {
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        fprintf(stderr, "cc: cc_buf_printf: bad format \"%s\"\n", fmt);
        abort();
    }
    cc_buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void cc_buf_printf(CcBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cc_buf_vprintf(b, fmt, ap);
    va_end(ap);
}

char *cc_buf_take(CcBuf *b) {
    char *p;
    if (!b->data) cc_buf_init(b);
    p = b->data;
    cc_buf_init(b);
    return p;
}

void cc_buf_free(CcBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ---- CC_LIST -------------------------------------------------------- */

void *cc__list_grow(CcArena *a, void *items, size_t *cap, size_t n, size_t elem) {
    size_t ncap = *cap ? *cap * 2 : 8;
    void *p;
    if (ncap < *cap || (elem && ncap > SIZE_MAX / elem)) cc__oom("list", ncap);
    p = cc_arena_alloc(a, ncap * elem, _Alignof(void *));
    if (n && items) memcpy(p, items, n * elem);
    *cap = ncap;
    return p;
}

/* ---- Files ---------------------------------------------------------- */

char *cc_read_file(CcArena *a, const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    CcBuf b;
    char *out;
    if (!f) return NULL;
    cc_buf_init(&b);
    for (;;) {
        size_t got;
        cc_buf_reserve(&b, 65536);
        got = fread(b.data + b.len, 1, b.cap - b.len - 1, f);
        b.len += got;
        b.data[b.len] = 0;
        if (got == 0) break;
    }
    if (ferror(f)) {
        int e = errno;
        fclose(f);
        cc_buf_free(&b);
        errno = e ? e : EIO;
        return NULL;
    }
    fclose(f);
    out = (char *)cc_arena_alloc(a, b.len + 1, 1);
    memcpy(out, b.data, b.len + 1);
    if (len_out) *len_out = b.len;
    cc_buf_free(&b);
    return out;
}
