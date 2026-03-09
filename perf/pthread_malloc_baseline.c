/*
 * pthread_malloc_baseline.c - Per-thread bump arena allocation baseline
 *
 * Each thread allocates from its own private bump-pointer arena — the pthread
 * equivalent of CC's per-fiber arena strategy. This is a fair comparison:
 * both CC and Pthread use pre-allocated buffers with bump-pointer allocation,
 * no malloc/free per alloc, no shared allocator contention.
 *
 * Go uses make([]byte, 16) which goes through its per-P mcache — also a
 * low-contention per-goroutine allocator, comparable to bump arenas.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdint.h>

#define NUM_THREADS 16
#define ALLOCS_PER_THREAD 62500
#define ARENA_SIZE (1024 * 1024) // 1MB per thread — same as CC per-fiber arena

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

_Atomic int g_success = 0;

typedef struct {
    char* base;
    size_t offset;
    size_t size;
} BumpArena;

static void* bump_alloc(BumpArena* a, size_t sz, size_t align) {
    size_t aligned = (a->offset + align - 1) & ~(align - 1);
    if (aligned + sz > a->size) return NULL;
    void* ptr = a->base + aligned;
    a->offset = aligned + sz;
    return ptr;
}

void* arena_worker(void* arg) {
    char* buf = malloc(ARENA_SIZE);
    BumpArena arena = { .base = buf, .offset = 0, .size = ARENA_SIZE };

    int local_success = 0;
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        void* ptr = bump_alloc(&arena, 16, 8);
        if (ptr) {
            // Touch the memory to prevent dead-code elimination.
            ((volatile char*)ptr)[0] = (char)i;
            local_success++;
        }
    }
    // Single atomic update at the end — no per-alloc shared contention.
    atomic_fetch_add(&g_success, local_success);

    free(buf);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=================================================================\n");
    printf("PTHREAD PER-THREAD ARENA: %d threads x %d allocs\n", NUM_THREADS, ALLOCS_PER_THREAD);
    printf("Total allocations: %d\n", NUM_THREADS * ALLOCS_PER_THREAD);
    printf("=================================================================\n\n");

    pthread_t threads[NUM_THREADS];
    double start = time_now_ms();
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, arena_worker, NULL);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    double duration = time_now_ms() - start;

    int success = atomic_load(&g_success);
    printf("Results:\n");
    printf("  Success: %d\n", success);
    printf("  Time:    %.2f ms\n", duration);
    printf("  Throughput: %.2f M allocs/sec\n", (double)success / duration / 1000.0);
    printf("=================================================================\n");

    return 0;
}
