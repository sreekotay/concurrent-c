/* One-shot DOM memory probe: ours (arena bump) vs yyjson (tracked alc).
 *   gcc -O2 -I ../../../out/include -I ../../../cc/include mem_probe.c \
 *       ../../../out/runtime/arena_state.c yyjson.c -o mem_probe \
 *       && ./mem_probe twitter.json
 */
#include "json.h"
#include "yyjson.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    size_t live;
    size_t peak;
} AlcCtx;

static void *alc_malloc(void *ctx, size_t size) {
    AlcCtx *c = (AlcCtx *)ctx;
    size_t *hdr = (size_t *)malloc(sizeof(size_t) + size);
    if (!hdr) return NULL;
    hdr[0] = size;
    c->live += size;
    if (c->live > c->peak) c->peak = c->live;
    return hdr + 1;
}
static void *alc_realloc(void *ctx, void *ptr, size_t old_size, size_t size) {
    AlcCtx *c = (AlcCtx *)ctx;
    if (!ptr) return alc_malloc(ctx, size);
    size_t *hdr = ((size_t *)ptr) - 1;
    (void)old_size;
    c->live -= hdr[0];
    size_t *nh = (size_t *)realloc(hdr, sizeof(size_t) + size);
    if (!nh) {
        c->live += hdr[0];
        return NULL;
    }
    nh[0] = size;
    c->live += size;
    if (c->live > c->peak) c->peak = c->live;
    return nh + 1;
}
static void alc_free(void *ctx, void *ptr) {
    AlcCtx *c = (AlcCtx *)ctx;
    if (!ptr) return;
    size_t *hdr = ((size_t *)ptr) - 1;
    c->live -= hdr[0];
    free(hdr);
}

static size_t arena_bump_used(const CCArena *a) {
    size_t used = 0;
    for (const CCArena *e = a->prev; e; e = e->prev)
        used += CC_ATOMIC_LOAD(&((CCArena *)(uintptr_t)e)->offset);
    used += CC_ATOMIC_LOAD(&((CCArena *)(uintptr_t)a)->offset);
    return used;
}

static void fmt_kb(char *out, size_t n, size_t bytes) {
    snprintf(out, n, "%.1f KB", bytes / 1024.0);
}

static void out_fd(int fd, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) (void)write(fd, s, n);
}
static void outf(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) (void)write(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

int main(int argc, char **argv) {
    const char *fn = argc > 1 ? argv[1] : "twitter.json";
    FILE *f = fopen(fn, "rb");
    if (!f) { out_fd(STDERR_FILENO, fn); out_fd(STDERR_FILENO, " open failed\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 64);
    size_t rd = fread(b, 1, (size_t)n, f);
    b[rd] = 0;
    fclose(f);

    char kb[32];
    fmt_kb(kb, sizeof(kb), (size_t)n);
    outf(STDOUT_FILENO, "== %s  input=%s (%ld bytes)  ptr=%zuB  JsonNode=%zuB\n",
           fn, kb, n, sizeof(void *), sizeof(JsonNode));

    /* ours: one parse, bump_used + committed slab reservation */
    {
        CCArena ar = cc_arena_create((size_t)n * 24 + (48 << 20));
        JsonParser p = json_parser(b, n, ar);
        JsonNode v;
        if (JsonParser_parse(&p, &v) != JSON_OK) {
            out_fd(STDERR_FILENO, "ours: parse failed\n");
            return 1;
        }
        size_t bump = arena_bump_used(&ar);
        size_t gross = cc_arena_committed_gross_bytes(&ar);
        char bkb[32], gkb[32], tot[32];
        fmt_kb(bkb, sizeof(bkb), bump);
        fmt_kb(gkb, sizeof(gkb), gross);
        fmt_kb(tot, sizeof(tot), bump + (size_t)n); /* keep input for borrows */
        outf(STDOUT_FILENO, "  ours hand     bump_used=%s  committed=%s  +input=%s  "
               "(borrow=%ld mat=%ld)\n",
               bkb, gkb, tot, p.borrowed_strs, p.materialized_strs);
        cc_arena_free(&ar);
    }

    /* yyjson default: copy strings */
    {
        AlcCtx ctx = {0};
        yyjson_alc alc = { alc_malloc, alc_realloc, alc_free, &ctx };
        yyjson_doc *d = yyjson_read_opts(b, rd, 0, &alc, NULL);
        if (!d) { out_fd(STDERR_FILENO, "yyjson default failed\n"); return 1; }
        char pkb[32], tot[32];
        fmt_kb(pkb, sizeof(pkb), ctx.peak);
        fmt_kb(tot, sizeof(tot), ctx.peak); /* strings copied; input not required */
        outf(STDOUT_FILENO, "  yyjson default alc_peak=%s  (input freeable; val_count=%zu)\n",
               pkb, yyjson_doc_get_val_count(d));
        (void)tot;
        yyjson_doc_free(d);
    }

    /* yyjson insitu: mutates a copy of input */
    {
        char *ib = (char *)malloc(rd + 64);
        memcpy(ib, b, rd);
        AlcCtx ctx = {0};
        yyjson_alc alc = { alc_malloc, alc_realloc, alc_free, &ctx };
        yyjson_doc *d = yyjson_read_opts(ib, rd, YYJSON_READ_INSITU, &alc, NULL);
        if (!d) { out_fd(STDERR_FILENO, "yyjson insitu failed\n"); return 1; }
        char pkb[32], tot[32];
        fmt_kb(pkb, sizeof(pkb), ctx.peak);
        fmt_kb(tot, sizeof(tot), ctx.peak + rd); /* must keep mutated input */
        outf(STDOUT_FILENO, "  yyjson insitu  alc_peak=%s  +input=%s  (val_count=%zu)\n",
               pkb, tot, yyjson_doc_get_val_count(d));
        yyjson_doc_free(d);
        free(ib);
    }

    free(b);
    return 0;
}
