/*
 * pthread_herd_pipe_herd.c - Pthread THUNDERING HERD demonstration (pipe-based)
 *
 * This is NOT the idiomatic "wake one of many waiters" pattern in C.
 * It is the classic thundering-herd worst case and exists to demonstrate why:
 *
 *   Blocked read()-ers on a pipe sit on a NON-exclusive wait queue (on both
 *   Linux and Darwin). A single write(1 byte) marks the pipe readable and
 *   wakes EVERY blocked reader; one wins the byte, the rest re-block.
 *   Latency scales with the number of parked readers.
 *
 * For the idiomatic C "wake-one" baseline (condvar + predicate loop), see
 * pthread_herd_baseline.c.
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
    printf("PTHREAD (PIPE / THUNDERING HERD) — worst case\n");
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
