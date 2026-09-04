/* Plain C baseline for perf/arena_lifetime_bench.ccs: the same shapes
 * written the way a C programmer writes them with no lifetime model.
 *
 *   alloc          malloc/free, and a hand-rolled pointer-bump arena
 *   checkpoint     bump arena mark + reset (save/restore one offset)
 *   release        malloc + free
 *   regrow         realloc growth
 *   vec            realloc-grown int array (1.6x), interleaved mallocs
 *   string         realloc-grown byte buffer, malloc+copy+free churn,
 *                  snprintf into a stack buffer / into a malloc'd buffer
 *   map            open-addressing int table over malloc, free
 *   owner          malloc header + malloc payload + free both
 *
 * Row names match the CC benchmark where an analog exists so the compare
 * script can line them up. Build: gcc -O2 -std=c11 perf/arena_lifetime_c_baseline.c -o perf/out/arena_lifetime_c_baseline */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#ifndef ROUNDS
#define ROUNDS 5
#endif

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static volatile uint64_t sink;

typedef double (*BenchFn)(size_t iters);

static void run(const char *name, BenchFn fn, size_t iters) {
    double best = 1e30;
    int r;
    for (r = 0; r < ROUNDS; r++) {
        double dt = fn(iters);
        if (dt < best) best = dt;
    }
    printf("%-46s %10.1f ns/op %10.2f Mop/s\n", name, best * 1e9 / (double)iters,
           (double)iters / best / 1e6);
}

/* ---- hand-rolled bump arena ------------------------------------------ */
typedef struct { unsigned char *base; size_t cap; size_t off; } Bump;
static void *bump_alloc(Bump *b, size_t n, size_t align) {
    size_t a = (b->off + (align - 1)) & ~(align - 1);
    if (a + n > b->cap) return NULL;
    b->off = a + n;
    return b->base + a;
}
static Bump g_bump;

/* ---- alloc ---------------------------------------------------------- */
/* ---- contended: 4 threads on glibc malloc (per-thread tcache) ---------- */
enum { CONTEND_THREADS = 4 };
typedef struct { size_t n; int release; void **keep; } ContendArg;
static void *contend_worker(void *p) {
    ContendArg *c = (ContendArg *)p;
    size_t i;
    uint64_t s = 0;
    for (i = 0; i < c->n; i++) {
        void *q = malloc(64);
        s += (uintptr_t)q;
        if (c->release) free(q); else c->keep[i] = q;
    }
    sink += s;
    return NULL;
}
static double b_contend(size_t n, int release) {
    pthread_t th[CONTEND_THREADS];
    ContendArg args[CONTEND_THREADS];
    int t;
    double t0;
    size_t i;
    for (t = 0; t < CONTEND_THREADS; t++) {
        args[t].n = n / CONTEND_THREADS;
        args[t].release = release;
        args[t].keep = release ? NULL : (void **)malloc(sizeof(void *) * args[t].n);
    }
    t0 = now_sec();
    for (t = 0; t < CONTEND_THREADS; t++) pthread_create(&th[t], NULL, contend_worker, &args[t]);
    for (t = 0; t < CONTEND_THREADS; t++) pthread_join(th[t], NULL);
    t0 = now_sec() - t0;
    for (t = 0; t < CONTEND_THREADS; t++) {
        if (args[t].keep) {
            for (i = 0; i < args[t].n; i++) free(args[t].keep[i]);
            free(args[t].keep);
        }
    }
    return t0;
}
/* One hand bump arena per thread: the shape the CC per-thread rows use. */
typedef struct { size_t n; int release; } OwnArg;
static void *own_worker(void *p) {
    OwnArg *c = (OwnArg *)p;
    uint8_t *base = (uint8_t *)malloc(48u << 20);
    size_t off = 0;
    size_t i;
    uint64_t s = 0;
    for (i = 0; i < c->n; i++) {
        void *q = base + off;
        off += 64;
        *(volatile uint64_t *)q = (uint64_t)i; /* touch the block so the bump is not hoisted away */
        s += (uintptr_t)q;
        if (c->release) off -= 64;
        else if ((i & 4095) == 4095) off = 0;
    }
    sink += s;
    free(base);
    return NULL;
}
static double b_own(size_t n, int release) {
    pthread_t th[CONTEND_THREADS];
    OwnArg args[CONTEND_THREADS];
    int t;
    double t0 = now_sec();
    for (t = 0; t < CONTEND_THREADS; t++) {
        args[t].n = n / CONTEND_THREADS;
        args[t].release = release;
        pthread_create(&th[t], NULL, own_worker, &args[t]);
    }
    for (t = 0; t < CONTEND_THREADS; t++) pthread_join(th[t], NULL);
    return now_sec() - t0;
}
static double b_alloc_own(size_t n) { return b_own(n, 0); }
static double b_alloc_release_own(size_t n) { return b_own(n, 1); }
/* Physics floor: four threads CAS-bumping one word (same code as the CC bench). */
static _Alignas(64) uint64_t raw_word;
static void *raw_worker(void *p) {
    size_t n = *(size_t *)p;
    size_t i;
    uint64_t s = 0;
    for (i = 0; i < n; i++) {
        uint64_t e = __atomic_load_n(&raw_word, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&raw_word, &e, e + 64, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {}
        s += e;
    }
    sink += s;
    return NULL;
}
static double b_raw_cas(size_t n) {
    pthread_t th[CONTEND_THREADS];
    size_t per = n / CONTEND_THREADS;
    int t;
    double t0 = now_sec();
    for (t = 0; t < CONTEND_THREADS; t++) pthread_create(&th[t], NULL, raw_worker, &per);
    for (t = 0; t < CONTEND_THREADS; t++) pthread_join(th[t], NULL);
    return now_sec() - t0;
}
static double b_malloc_contended(size_t n) { return b_contend(n, 0); }
static double b_malloc_free_contended(size_t n) { return b_contend(n, 1); }

static double b_malloc_free(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        void *p = malloc(64);
        sink += (uintptr_t)p;
        free(p);
    }
    return now_sec() - t0;
}
static double b_bump_alloc(size_t n) {
    size_t i;
    double t0 = now_sec();
    g_bump.off = 0;
    for (i = 0; i < n; i++) {
        sink += (uintptr_t)bump_alloc(&g_bump, 64, 8);
        if ((i & 4095) == 4095) g_bump.off = 0;
    }
    return now_sec() - t0;
}

/* ---- checkpoint: mark + reset -------------------------------------- */
static double b_mark_reset_empty(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        size_t mark = g_bump.off;
        sink += mark;
        g_bump.off = mark;
    }
    return now_sec() - t0;
}
static double b_mark_3_reset(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        size_t mark = g_bump.off;
        sink += (uintptr_t)bump_alloc(&g_bump, 40, 8);
        sink += (uintptr_t)bump_alloc(&g_bump, 96, 8);
        sink += (uintptr_t)bump_alloc(&g_bump, 24, 8);
        g_bump.off = mark;
    }
    return now_sec() - t0;
}
static double b_mark_nested(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        size_t m1 = g_bump.off, m2, m3;
        sink += (uintptr_t)bump_alloc(&g_bump, 32, 8);
        m2 = g_bump.off;
        sink += (uintptr_t)bump_alloc(&g_bump, 32, 8);
        m3 = g_bump.off;
        sink += (uintptr_t)bump_alloc(&g_bump, 32, 8);
        g_bump.off = m3;
        g_bump.off = m2;
        g_bump.off = m1;
    }
    return now_sec() - t0;
}
static double b_mark_big_reset(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        void *p = malloc(2u << 20); /* scratch past any arena: malloc it */
        sink += (uintptr_t)p;
        free(p);
    }
    return now_sec() - t0;
}

/* ---- regrow --------------------------------------------------------- */
static double b_realloc_tip(size_t n) {
    size_t i;
    void *blk = NULL;
    size_t sz = 0;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        if ((i % 1000) == 0) {
            free(blk);
            blk = malloc(8);
            sz = 8;
        }
        blk = realloc(blk, sz + 64);
        sz += 64;
        sink += (uintptr_t)blk;
    }
    free(blk);
    return now_sec() - t0;
}
static double b_realloc_buried(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        void *blk = malloc(256);
        void *other = malloc(8);
        blk = realloc(blk, 320);
        sink += (uintptr_t)blk;
        free(other);
        free(blk);
    }
    return now_sec() - t0;
}

/* ---- vec: realloc-grown int array ----------------------------------- */
typedef struct { int *data; size_t len, cap; } IntArr;
static int arr_push(IntArr *v, int x) {
    if (v->len == v->cap) {
        size_t nc = v->cap ? (v->cap * 8) / 5 : 8;
        int *nd = (int *)realloc(v->data, nc * sizeof(int));
        if (!nd) return -1;
        v->data = nd;
        v->cap = nc;
    }
    v->data[v->len++] = x;
    return 0;
}
static double b_vec_push(size_t n) {
    size_t i;
    IntArr v = {0, 0, 0};
    double t0 = now_sec();
    for (i = 0; i < n; i++) arr_push(&v, (int)i);
    free(v.data);
    return now_sec() - t0;
}
static double b_vec_push_moves(size_t n) {
    size_t i;
    IntArr v = {0, 0, 0};
    void *junk[1 << 15];
    size_t nj = 0;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        arr_push(&v, (int)i);
        if ((i & 63) == 0 && nj < (1u << 15)) { junk[nj++] = malloc(16); }
    }
    free(v.data);
    for (i = 0; i < nj; i++) free(junk[i]);
    return now_sec() - t0;
}
static double b_vec_get(size_t n) {
    size_t i;
    double t0;
    IntArr v = {0, 0, 0};
    for (i = 0; i < n; i++) arr_push(&v, (int)i);
    t0 = now_sec();
    for (i = 0; i < n; i++) sink += (uintptr_t)v.data[i];
    {
        double dt = now_sec() - t0;
        free(v.data);
        return dt;
    }
}
static double b_vec_churn(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        IntArr v = {0, 0, 0};
        int k;
        for (k = 0; k < 8; k++) arr_push(&v, k);
        sink += (uintptr_t)v.data;
        free(v.data);
    }
    return now_sec() - t0;
}

/* ---- string --------------------------------------------------------- */
typedef struct { char *data; size_t len, cap; } Str;
static void str_push(Str *s, const char *src, size_t n) {
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 8;
        while (nc < s->len + n + 1) nc = (nc * 8) / 5;
        s->data = (char *)realloc(s->data, nc);
        s->cap = nc;
    }
    memcpy(s->data + s->len, src, n);
    s->len += n;
    s->data[s->len] = '\0';
}
static double b_str_append(size_t n) {
    size_t i;
    Str s = {0, 0, 0};
    double t0;
    s.cap = 12 * n + 1;
    s.data = (char *)malloc(s.cap);
    t0 = now_sec();
    for (i = 0; i < n; i++) str_push(&s, "twelve-bytes", 12);
    free(s.data);
    return now_sec() - t0;
}
static double b_str_churn(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        char *p = (char *)malloc(32);
        memcpy(p, "twenty-four-bytes-long!!", 25);
        sink += (uintptr_t)p;
        free(p);
    }
    return now_sec() - t0;
}
static double b_snprintf_stack(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "item %zu of %llu", i, (unsigned long long)sink);
        sink += (uintptr_t)len;
    }
    return now_sec() - t0;
}
static double b_snprintf_malloc(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        char *buf = (char *)malloc(64);
        int len = snprintf(buf, 64, "item %zu of %llu", i, (unsigned long long)sink);
        sink += (uintptr_t)len;
        free(buf);
    }
    return now_sec() - t0;
}

/* ---- map: open addressing over malloc ------------------------------- */
typedef struct { int *keys; int *vals; unsigned char *used; size_t cap, len; } IntMap;
static void map_insert(IntMap *m, int k, int v);
static void map_grow(IntMap *m) {
    IntMap old = *m;
    size_t i;
    m->cap = old.cap ? old.cap * 2 : 16;
    m->keys = (int *)malloc(m->cap * sizeof(int));
    m->vals = (int *)malloc(m->cap * sizeof(int));
    m->used = (unsigned char *)calloc(m->cap, 1);
    m->len = 0;
    for (i = 0; i < old.cap; i++) if (old.used[i]) map_insert(m, old.keys[i], old.vals[i]);
    free(old.keys); free(old.vals); free(old.used);
}
static void map_insert(IntMap *m, int k, int v) {
    size_t h;
    if ((m->len + 1) * 10 > m->cap * 9) map_grow(m);
    h = ((uint32_t)k * 2654435761u) & (m->cap - 1);
    while (m->used[h] && m->keys[h] != k) h = (h + 1) & (m->cap - 1);
    if (!m->used[h]) { m->used[h] = 1; m->keys[h] = k; m->len++; }
    m->vals[h] = v;
}
static int *map_get(IntMap *m, int k) {
    size_t h = ((uint32_t)k * 2654435761u) & (m->cap - 1);
    while (m->used[h]) { if (m->keys[h] == k) return &m->vals[h]; h = (h + 1) & (m->cap - 1); }
    return NULL;
}
static double b_map(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        IntMap m = {0, 0, 0, 0, 0};
        int k;
        for (k = 0; k < 1000; k++) map_insert(&m, k, k);
        sink += (uintptr_t)map_get(&m, 500);
        free(m.keys); free(m.vals); free(m.used);
    }
    return now_sec() - t0;
}

/* ---- owner: header + payload, two mallocs --------------------------- */
typedef struct { void *payload; size_t bytes; uint32_t token; } Hdr;
static double b_owner(size_t n) {
    size_t i;
    double t0 = now_sec();
    for (i = 0; i < n; i++) {
        Hdr *h = (Hdr *)malloc(sizeof(Hdr));
        h->payload = malloc(64);
        h->bytes = 64;
        h->token = (uint32_t)i;
        sink += (uintptr_t)h->payload;
        free(h->payload);
        free(h);
    }
    return now_sec() - t0;
}
static double b_counter(size_t n) {
    size_t i;
    static uint32_t ctr;
    double t0 = now_sec();
    for (i = 0; i < n; i++) { uint32_t t = ++ctr; sink += t; }
    return now_sec() - t0;
}

int main(void) {
    g_bump.cap = 8u << 20;
    g_bump.base = (unsigned char *)malloc(g_bump.cap);
    g_bump.off = 0;
    printf("arena_lifetime_c_baseline (ROUNDS=%d, best of)\n", ROUNDS);
    printf("%-46s %14s %16s\n", "row", "ns/op", "throughput");
    run("alloc 64B shared, reset/4096", b_malloc_free, 4096 * 1000);            /* C: malloc+free */
    run("alloc 64B shared, 4 threads (contended)", b_malloc_contended, 2000000); /* C: malloc x4 threads, freed after */
    run("alloc + release_sized 64B, 4 threads (contended)", b_malloc_free_contended, 2000000); /* C: malloc+free x4 threads */
    run("alloc 64B, 4 threads, one arena each, reset/4096", b_alloc_own, 2000000);               /* C: hand bump per thread */
    run("alloc + release_sized 64B, 4 threads, one arena each", b_alloc_release_own, 2000000);
    run("raw CAS bump on one word, 4 threads (floor)", b_raw_cas, 2000000);
    run("alloc 64B local, reset/4096", b_bump_alloc, 4096 * 1000);              /* C: hand bump */
    run("checkpoint + restore (empty)", b_mark_reset_empty, 1000000);           /* C: mark/reset */
    run("checkpoint + 3 allocs + restore (@scratch shape)", b_mark_3_reset, 1000000);
    run("checkpoint + 3 allocs + restore (1 KiB stack)", b_mark_3_reset, 1000000);
    run("nested checkpoint x3 + restore x3", b_mark_nested, 500000);
    run("checkpoint_local + restore_local (empty, owned)", b_mark_reset_empty, 1000000); /* C: mark/reset */
    run("checkpoint_local + 3 alloc_local + restore_local (owned)", b_mark_3_reset, 1000000);
    run("checkpoint + 2 MiB scratch (child extent) + restore", b_mark_big_reset, 100000);
    run("alloc 64B + release (unsized, last-live)", b_malloc_free, 2000000);
    run("alloc 64B + release_sized (tip pop)", b_malloc_free, 2000000);
    run("alloc 64B x2 + release_sized first (hole)", b_malloc_free, 2000000);
    run("reuse: alloc x2 + release first + alloc", b_malloc_free, 2000000);
    run("realloc tip growth 64B steps (in place)", b_realloc_tip, 1000000);
    run("realloc buried 256B -> 320B (copy + release)", b_realloc_buried, 1000000);
    run("vec push 1M ints (tip fit) + destroy", b_vec_push, 1000000);
    run("vec push 1M ints, alloc every 64 (moves)", b_vec_push_moves, 1000000);
    run("vec get 1M (token-checked handle)", b_vec_get, 1000000);
    run("vec init + 8 push + destroy (owner churn)", b_vec_churn, 500000);
    run("string push_cstr 12B x 1M (one string, reserved)", b_str_append, 1000000);
    run("string promote (24B) + destroy churn", b_str_churn, 1000000);
    run("@string template (2 slots) call-local on @scratch", b_snprintf_stack, 1000000);   /* C: snprintf on stack */
    run("@string template (2 slots) on arena + destroy", b_snprintf_malloc, 1000000);
    run("map insert 1k ints + destroy", b_map, 2000);
    run("owner new 64B + release (header rebirth)", b_owner, 2000000);
    run("token registry birth + kill", b_counter, 4000000);                     /* C: plain counter */
    printf("sink=%llu\n", (unsigned long long)sink);
    free(g_bump.base);
    return 0;
}
