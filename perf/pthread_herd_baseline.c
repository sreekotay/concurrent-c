/*
 * pthread_herd_baseline.c - Pthread thundering herd test
 *
 * Uses a pipe as the wake primitive: write(1 byte) wakes exactly one reader,
 * matching Go/CC channel-send semantics for a fair comparison.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sched.h>

#define NUM_WAITERS 1000
#define NUM_SAMPLES 5

typedef struct {
    int pipefd[2];
    _Atomic int count;
} Herd;

void* waiter_thread(void* arg) {
    Herd* h = (Herd*)arg;
    char buf;
    read(h->pipefd[0], &buf, 1);
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
    printf("PTHREAD THUNDERING HERD BASELINE\n");
    printf("=================================================================\n\n");

    for (int sample = 1; sample <= NUM_SAMPLES; sample++) {
        Herd h;
        pipe(h.pipefd);
        atomic_init(&h.count, 0);

        pthread_t threads[NUM_WAITERS];
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_create(&threads[i], NULL, waiter_thread, &h);
        }

        usleep(100000); // 100ms - let all threads block on read()

        double start = time_now_ms();
        char byte = 'x';
        write(h.pipefd[1], &byte, 1); // wake exactly one reader

        while (atomic_load(&h.count) < 1) {
            sched_yield();
        }
        double latency_ms = time_now_ms() - start;
        printf("Sample %d: Latency to wake 1st waiter: %8.4f ms\n", sample, latency_ms);

        // Flush the rest so threads can exit
        for (int i = 1; i < NUM_WAITERS; i++) {
            write(h.pipefd[1], &byte, 1);
        }
        for (int i = 0; i < NUM_WAITERS; i++) {
            pthread_join(threads[i], NULL);
        }

        close(h.pipefd[0]);
        close(h.pipefd[1]);
    }

    return 0;
}
