/*
 * pthread_malloc_baseline.c - Pthread malloc contention baseline
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

#define NUM_THREADS 16
#define ALLOCS_PER_THREAD 62500 // To match 1,000,000 total allocs

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

_Atomic int g_success = 0;

void* malloc_worker(void* arg) {
    void** ptrs = malloc(sizeof(void*) * ALLOCS_PER_THREAD);
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        ptrs[i] = malloc(16);
        if (ptrs[i]) atomic_fetch_add(&g_success, 1);
    }
    // Cleanup to avoid massive leak
    for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
        free(ptrs[i]);
    }
    free(ptrs);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    
    printf("=================================================================\n");
    printf("PTHREAD MALLOC BASELINE: %d threads x %d allocs\n", NUM_THREADS, ALLOCS_PER_THREAD);
    printf("=================================================================\n\n");

    pthread_t threads[NUM_THREADS];
    double start = time_now_ms();
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, malloc_worker, NULL);
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
