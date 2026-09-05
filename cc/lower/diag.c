/* Diagnostic sink: collect now, print sorted later. See diag.h. */
#include "diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cc_diag_init(CcDiag *d, CcArena *a) {
    memset(d, 0, sizeof *d);
    d->arena = a;
}

static CcDiagMsg *cc__diag_add(CcDiag *d, CcSeverity sev, CcLoc loc,
                               const char *fmt, va_list ap) {
    CcDiagMsg *m = CC_NEW(d->arena, CcDiagMsg);
    va_list ap2;
    int n;
    char *text;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        fprintf(stderr, "cc: cc_diag_emit: bad format \"%s\"\n", fmt);
        abort();
    }
    text = (char *)cc_arena_alloc(d->arena, (size_t)n + 1, 1);
    vsnprintf(text, (size_t)n + 1, fmt, ap);
    m->sev = sev;
    m->loc = loc;
    if (loc.path) m->loc.path = cc_arena_strdup(d->arena, loc.path);
    m->text = text;
    m->source_line = NULL;
    m->caret_col = 0;
    m->caret_len = 0;
    CC_LIST_PUSH(d->arena, &d->msgs, m);
    if (sev == CC_SEV_ERROR) d->n_errors++;
    else if (sev == CC_SEV_WARNING) d->n_warnings++;
    return m;
}

void cc_diag_emit(CcDiag *d, CcSeverity sev, CcLoc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cc__diag_add(d, sev, loc, fmt, ap);
    va_end(ap);
}

void cc_diag_emit_at(CcDiag *d, CcSeverity sev, CcLoc loc, const char *src,
                     size_t src_len, size_t off, size_t len, const char *fmt, ...) {
    va_list ap;
    CcDiagMsg *m;
    size_t line_start, line_end;
    if (off > src_len) {
        fprintf(stderr, "cc: cc_diag_emit_at: offset %zu past end of source (%zu bytes)\n",
                off, src_len);
        abort();
    }
    va_start(ap, fmt);
    m = cc__diag_add(d, sev, loc, fmt, ap);
    va_end(ap);
    line_start = off;
    while (line_start > 0 && src[line_start - 1] != '\n') line_start--;
    line_end = off;
    while (line_end < src_len && src[line_end] != '\n') line_end++;
    if (line_end > line_start && src[line_end - 1] == '\r') line_end--;
    m->source_line = cc_arena_strndup(d->arena, src + line_start, line_end - line_start);
    if (off - line_start >= UINT32_MAX || len > UINT32_MAX) {
        fprintf(stderr, "cc: cc_diag_emit_at: caret position does not fit\n");
        abort();
    }
    m->caret_col = (uint32_t)(off - line_start) + 1;
    m->caret_len = (uint32_t)len;
}

typedef struct CcDiagSortEntry {
    const CcDiagMsg *m;
    size_t index;
} CcDiagSortEntry;

static int cc__diag_cmp(const void *pa, const void *pb) {
    const CcDiagSortEntry *a = (const CcDiagSortEntry *)pa;
    const CcDiagSortEntry *b = (const CcDiagSortEntry *)pb;
    const char *ap = a->m->loc.path, *bp = b->m->loc.path;
    int c;
    if (ap != bp) {
        if (!ap) return -1;
        if (!bp) return 1;
        c = strcmp(ap, bp);
        if (c) return c;
    }
    if (a->m->loc.line != b->m->loc.line) return a->m->loc.line < b->m->loc.line ? -1 : 1;
    if (a->m->loc.col != b->m->loc.col) return a->m->loc.col < b->m->loc.col ? -1 : 1;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    return 0;
}

static const char *cc__sev_name(CcSeverity sev) {
    switch (sev) {
    case CC_SEV_NOTE: return "note";
    case CC_SEV_WARNING: return "warning";
    case CC_SEV_ERROR: return "error";
    }
    return "error";
}

uint32_t cc_diag_print(const CcDiag *d, void *stream) {
    FILE *out = (FILE *)stream;
    size_t n = d->msgs.n, i;
    CcDiagSortEntry *sorted;
    if (n == 0) return d->n_errors;
    if (n > SIZE_MAX / sizeof *sorted) {
        fprintf(stderr, "cc: cc_diag_print: too many messages\n");
        abort();
    }
    sorted = (CcDiagSortEntry *)malloc(n * sizeof *sorted);
    if (!sorted) {
        fprintf(stderr, "cc: out of memory: diagnostic sort (%zu entries)\n", n);
        abort();
    }
    for (i = 0; i < n; i++) {
        sorted[i].m = d->msgs.items[i];
        sorted[i].index = i;
    }
    qsort(sorted, n, sizeof *sorted, cc__diag_cmp);
    for (i = 0; i < n; i++) {
        const CcDiagMsg *m = sorted[i].m;
        fputs(m->loc.path ? m->loc.path : "<unknown>", out);
        if (m->loc.line) {
            fprintf(out, ":%u", (unsigned)m->loc.line);
            if (m->loc.col) fprintf(out, ":%u", (unsigned)m->loc.col);
        }
        fprintf(out, ": %s: %s\n", cc__sev_name(m->sev), m->text);
        if (m->source_line) {
            fputs(m->source_line, out);
            fputc('\n', out);
            if (m->caret_col) {
                uint32_t c, k;
                size_t sl = strlen(m->source_line);
                /* Keep tabs so the caret lines up under the source. */
                for (c = 1; c < m->caret_col; c++)
                    fputc(c - 1 < sl && m->source_line[c - 1] == '\t' ? '\t' : ' ', out);
                fputc('^', out);
                for (k = 1; k < m->caret_len; k++) fputc('~', out);
                fputc('\n', out);
            }
        }
    }
    free(sorted);
    fflush(out);
    return d->n_errors;
}
