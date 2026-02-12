#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERATIONS 2000000
#define SAMPLES 7
#define RING_CAP 1024

typedef struct {
    _Atomic size_t seq;
    void* value;
} ring_cell;

typedef struct {
    ring_cell* cells;
    size_t cap;
    size_t mask;
    _Atomic size_t head;
    _Atomic size_t tail;
    _Atomic int count;
    _Atomic int enq_count;
    _Atomic int deq_count;
} ring_q;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void sort_doubles(double* arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (arr[j] < arr[i]) {
                double t = arr[i];
                arr[i] = arr[j];
                arr[j] = t;
            }
        }
    }
}

static int ring_init(ring_q* q, size_t cap) {
    q->cap = cap;
    q->mask = cap - 1;
    q->cells = (ring_cell*)calloc(cap, sizeof(ring_cell));
    if (!q->cells) return 0;
    for (size_t i = 0; i < cap; i++) {
        atomic_init(&q->cells[i].seq, i);
        q->cells[i].value = NULL;
    }
    atomic_store_explicit(&q->head, 0, memory_order_relaxed);
    atomic_store_explicit(&q->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&q->count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->enq_count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->deq_count, 0, memory_order_relaxed);
    return 1;
}

static void ring_free(ring_q* q) {
    free(q->cells);
    q->cells = NULL;
}

/* Same protocol style as runtime ring queue. */
static inline int ring_enqueue_raw(ring_q* q, void* v) {
    size_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
    for (;;) {
        ring_cell* c = &q->cells[pos & q->mask];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)pos;
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                c->value = v;
                atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
                return 1;
            }
        } else if (dif < 0) {
            return 0;
        } else {
            pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        }
    }
}

static inline int ring_dequeue_raw(ring_q* q, void** out) {
    size_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);
    for (;;) {
        ring_cell* c = &q->cells[pos & q->mask];
        size_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *out = c->value;
                atomic_store_explicit(&c->seq, pos + q->cap, memory_order_release);
                return 1;
            }
        } else if (dif < 0) {
            return 0;
        } else {
            pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        }
    }
}

static inline int ring_enqueue_counted(ring_q* q, void* v) {
    if (!ring_enqueue_raw(q, v)) return 0;
    atomic_fetch_add_explicit(&q->count, 1, memory_order_release);
    return 1;
}

static inline int ring_dequeue_counted(ring_q* q, void** out) {
    if (!ring_dequeue_raw(q, out)) return 0;
    atomic_fetch_sub_explicit(&q->count, 1, memory_order_release);
    return 1;
}

/* Split bookkeeping counters to avoid shared RMW hotspot:
 * producer updates enq_count only, consumer updates deq_count only. */
static inline int ring_enqueue_split_counted(ring_q* q, void* v) {
    if (!ring_enqueue_raw(q, v)) return 0;
    atomic_fetch_add_explicit(&q->enq_count, 1, memory_order_release);
    return 1;
}

static inline int ring_dequeue_split_counted(ring_q* q, void** out) {
    if (!ring_dequeue_raw(q, out)) return 0;
    atomic_fetch_add_explicit(&q->deq_count, 1, memory_order_release);
    return 1;
}

static double bench_local_loop_once(void) {
    volatile int64_t s1 = 0;
    volatile int64_t s2 = 0;
    double t0 = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        s1 += i;
        s2 += i;
    }
    double dt = now_ms() - t0;
    if (s1 < 0 || s2 < 0) fprintf(stderr, "impossible sums\n");
    return (ITERATIONS * 2.0) / (dt / 1000.0);
}

static double bench_ring_single_once(int counted) {
    ring_q q;
    if (!ring_init(&q, RING_CAP)) return 0.0;
    void* out = NULL;
    double t0 = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        void* v = (void*)(uintptr_t)(uint32_t)i;
        if (counted == 1) {
            while (!ring_enqueue_counted(&q, v)) {}
            while (!ring_dequeue_counted(&q, &out)) {}
        } else if (counted == 2) {
            while (!ring_enqueue_split_counted(&q, v)) {}
            while (!ring_dequeue_split_counted(&q, &out)) {}
        } else {
            while (!ring_enqueue_raw(&q, v)) {}
            while (!ring_dequeue_raw(&q, &out)) {}
        }
    }
    double dt = now_ms() - t0;
    ring_free(&q);
    return (ITERATIONS * 2.0) / (dt / 1000.0);
}

typedef struct {
    ring_q* q;
    int counted;
} worker_ctx;

static void* producer_main(void* p) {
    worker_ctx* ctx = (worker_ctx*)p;
    for (int i = 0; i < ITERATIONS; i++) {
        void* v = (void*)(uintptr_t)(uint32_t)i;
        if (ctx->counted == 1) {
            while (!ring_enqueue_counted(ctx->q, v)) {}
        } else if (ctx->counted == 2) {
            while (!ring_enqueue_split_counted(ctx->q, v)) {}
        } else {
            while (!ring_enqueue_raw(ctx->q, v)) {}
        }
    }
    return NULL;
}

static void* consumer_main(void* p) {
    worker_ctx* ctx = (worker_ctx*)p;
    void* out = NULL;
    int64_t checksum = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        if (ctx->counted == 1) {
            while (!ring_dequeue_counted(ctx->q, &out)) {}
        } else if (ctx->counted == 2) {
            while (!ring_dequeue_split_counted(ctx->q, &out)) {}
        } else {
            while (!ring_dequeue_raw(ctx->q, &out)) {}
        }
        checksum += (int64_t)(uint32_t)(uintptr_t)out;
    }
    if (checksum < 0) fprintf(stderr, "impossible checksum\n");
    return NULL;
}

static double bench_ring_two_thread_once(int counted) {
    ring_q q;
    pthread_t prod, cons;
    worker_ctx ctx = { .q = &q, .counted = counted };
    if (!ring_init(&q, RING_CAP)) return 0.0;

    double t0 = now_ms();
    pthread_create(&prod, NULL, producer_main, &ctx);
    pthread_create(&cons, NULL, consumer_main, &ctx);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);
    double dt = now_ms() - t0;

    ring_free(&q);
    return (ITERATIONS * 2.0) / (dt / 1000.0);
}

static double median_ops(double (*fn)(int), int arg) {
    double ops[SAMPLES] = {0};
    for (int i = 0; i < SAMPLES; i++) ops[i] = fn(arg);
    sort_doubles(ops, SAMPLES);
    return ops[SAMPLES / 2];
}

static double median_ops_local(void) {
    double ops[SAMPLES] = {0};
    for (int i = 0; i < SAMPLES; i++) ops[i] = bench_local_loop_once();
    sort_doubles(ops, SAMPLES);
    return ops[SAMPLES / 2];
}

int main(void) {
    double local = median_ops_local();
    double single_raw = median_ops(bench_ring_single_once, 0);
    double single_count = median_ops(bench_ring_single_once, 1);
    double single_split = median_ops(bench_ring_single_once, 2);
    double two_raw = median_ops(bench_ring_two_thread_once, 0);
    double two_count = median_ops(bench_ring_two_thread_once, 1);
    double two_split = median_ops(bench_ring_two_thread_once, 2);

    printf("perf_buffered_ladder_ring_core: iters=%d cap=%d\n", ITERATIONS, RING_CAP);
    printf("  local loop baseline:            %.0f ops/sec\n", local);
    printf("  ring single-thread raw:         %.0f ops/sec\n", single_raw);
    printf("  ring single-thread +count:      %.0f ops/sec\n", single_count);
    printf("  ring single-thread +splitcount: %.0f ops/sec\n", single_split);
    printf("  ring two-thread raw:            %.0f ops/sec\n", two_raw);
    printf("  ring two-thread +count:         %.0f ops/sec\n", two_count);
    printf("  ring two-thread +splitcount:    %.0f ops/sec\n", two_split);

    printf("  counted/raw ratio:\n");
    printf("    single-thread: %.1f%%\n", (single_count / single_raw) * 100.0);
    printf("    two-thread:    %.1f%%\n", (two_count / two_raw) * 100.0);
    printf("  splitcount/raw ratio:\n");
    printf("    single-thread: %.1f%%\n", (single_split / single_raw) * 100.0);
    printf("    two-thread:    %.1f%%\n", (two_split / two_raw) * 100.0);
    printf("  raw ring vs local baseline:\n");
    printf("    single-thread: %.1f%%\n", (single_raw / local) * 100.0);
    printf("    two-thread:    %.1f%%\n", (two_raw / local) * 100.0);
    printf("perf_buffered_ladder_ring_core: DONE\n");
    return 0;
}
