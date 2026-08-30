/* Heap helpers for shadow-emitted closures / autoblocks / spawn envs.
 * Always linked (including host TCC); no variadics. */
#include <stddef.h>
#include <stdlib.h>

void *cc__heap_alloc(size_t n) { return malloc(n); }
void *cc__heap_calloc(size_t n, size_t sz) { return calloc(n, sz); }
void cc__heap_free(void *p) { free(p); }
