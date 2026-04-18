/*
 * pthread_herd_baseline.c - Pthread "wake one of many" baseline (condvar)
 *
 * This is the textbook C idiom for "N consumers parked, signal exactly one":
 *
 *     lock(mu);
 *     while (items == 0) cond_wait(&cv, &mu);
 *     items--;
 *     unlock(mu);
 *
 *     lock(mu); items++; cond_signal(&cv); unlock(mu);
 *
 * pthread_cond_signal is spec'd to wake at least one waiter; on glibc/Linux
 * and Darwin it wakes exactly one (FUTEX_WAKE(1) / Mach semaphore). This
 * matches Go's and CC's channel wake-one semantics, and is the closest
 * apples-to-apples pthread baseline for a wake-one comparison.
 *
 * For the pathological pipe-based "thundering herd" worst case, see
 * pthread_herd_pipe_herd.c — it exists as a contrast, not as a baseline.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>

#define NUM_WAITERS 1000
#define NUM_SAMPLES 5

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             items;
    _Atomic int     count;
} Herd;

static void *waiter_thread(void *arg) {
    Herd *h = (Herd *)arg;
    pthread_mutex_lock(&h->mu);
    while (h->items == 0) pthread_cond_wait(&h->cv, &h->mu);
    h->items--;
    pthread_mutex_unlock(&h->mu);
    atomic_fetch_add(&h->count, 1);
    return NULL;
}

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=================================================================\n");
    printf("PTHREAD (CONDVAR) — idiomatic wake-one baseline\n");
    printf("=================================================================\n\n");

    for (int sample = 1; sample <= NUM_SAMPLES; sample++) {
        Herd h;
        pthread_mutex_init(&h.mu, NULL);
        pthread_cond_init(&h.cv, NULL);
        h.items = 0;
        atomic_init(&h.count, 0);

        pthread_t threads[NUM_WAITERS];
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_create(&threads[i], NULL, waiter_thread, &h);
        }

        usleep(100000); /* 100ms - let all threads park in cond_wait */

        double start = time_now_ms();
        pthread_mutex_lock(&h.mu);
        h.items++;
        pthread_cond_signal(&h.cv); /* wakes exactly one waiter */
        pthread_mutex_unlock(&h.mu);

        while (atomic_load(&h.count) < 1) {
            sched_yield();
        }
        double latency_ms = time_now_ms() - start;
        printf("Sample %d: Latency to wake 1st waiter: %8.4f ms\n", sample, latency_ms);

        /* Flush the rest so threads can exit */
        pthread_mutex_lock(&h.mu);
        h.items += NUM_WAITERS - 1;
        pthread_cond_broadcast(&h.cv);
        pthread_mutex_unlock(&h.mu);
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_join(threads[i], NULL);
        }

        pthread_cond_destroy(&h.cv);
        pthread_mutex_destroy(&h.mu);
    }

    return 0;
}
