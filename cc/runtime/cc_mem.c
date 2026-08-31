/* Libc isolate for default CCS TUs. Linked via concurrent_c.c. */
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *cc_malloc(size_t n) { return malloc(n); }
void cc_free(void *p) { free(p); }
void *cc_realloc(void *p, size_t n) { return realloc(p, n); }
void *cc_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *cc_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *cc_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int cc_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
_Noreturn void cc_abort(void) { abort(); }
_Noreturn void cc_exit(int code) { exit(code); }

int cc_eprintf(const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return n;
}
