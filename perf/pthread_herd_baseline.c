/*
 * pthread_herd_baseline.c - Pthread thundering herd test
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>

#define NUM_WAITERS 1000

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int ready;
    _Atomic int count;
} Herd;

void* waiter_thread(void* arg) {
    Herd* h = (Herd*)arg;
    pthread_mutex_lock(&h->mu);
    while (!h->ready) {
        pthread_cond_wait(&h->cond, &h->mu);
    }
    // h->ready = 0; // REMOVED: let everyone wake up for cleanup
    atomic_fetch_add(&h->count, 1);
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Herd h;
    pthread_mutex_init(&h.mu, NULL);
    pthread_cond_init(&h.cond, NULL);
    h.ready = 0;
    atomic_init(&h.count, 0);

    printf("=================================================================\n");
    printf("PTHREAD THUNDERING HERD BASELINE\n");
    printf("=================================================================\n\n");

    for (int sample = 1; sample <= 5; sample++) {
        h.ready = 0;
        atomic_store(&h.count, 0);
        
        pthread_t threads[NUM_WAITERS];
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_create(&threads[i], NULL, waiter_thread, &h);
        }

        usleep(100000); // 100ms

        double start = time_now_ms();
        pthread_mutex_lock(&h.mu);
        h.ready = 1;
        pthread_cond_broadcast(&h.cond); // Trigger the herd!
        pthread_mutex_unlock(&h.mu);

        while (atomic_load(&h.count) < 1) {
            usleep(1000);
        }
        double latency_ms = time_now_ms() - start;
        printf("Sample %d: Latency to wake 1st waiter: %8.4f ms\n", sample, latency_ms);

        // Cleanup: let others finish
        pthread_mutex_lock(&h.mu);
        h.ready = 1;
        pthread_cond_broadcast(&h.cond);
        pthread_mutex_unlock(&h.mu);
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    return 0;
}
