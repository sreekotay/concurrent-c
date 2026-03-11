/*
 * pthread_contention_baseline.c - Pthread-based contention test
 *
 * Producer sends i ^ (i >> 16); consumer accumulates a checksum.
 * Results feed g_sink so the optimizer cannot drop the channel work.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define ITERATIONS 1000000
#define QUEUE_SIZE 1024
#define NUM_TRIALS 15

/* Prevent dead-code elimination of consumer work. */
static volatile long long g_sink = 0;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int data[QUEUE_SIZE];
    int head;
    int tail;
    int count;
} SimpleQueue;

void queue_init(SimpleQueue* q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->head = q->tail = q->count = 0;
}

void queue_push(SimpleQueue* q, int v) {
    pthread_mutex_lock(&q->mu);
    while (q->count == QUEUE_SIZE) {
        pthread_cond_wait(&q->not_full, &q->mu);
    }
    q->data[q->tail] = v;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

int queue_pop(SimpleQueue* q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    int v = q->data[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return v;
}

void* producer_thread(void* arg) {
    SimpleQueue* q = (SimpleQueue*)arg;
    for (int i = 0; i < ITERATIONS; i++) queue_push(q, i ^ (i >> 16));
    return NULL;
}

/* Returns heap-allocated checksum so main() can accumulate g_sink. */
void* consumer_thread(void* arg) {
    SimpleQueue* q = (SimpleQueue*)arg;
    long long sum = 0;
    for (int i = 0; i < ITERATIONS; i++) sum += (long long)queue_pop(q);
    long long* result = (long long*)malloc(sizeof(long long));
    *result = sum;
    return result;
}

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    SimpleQueue q1, q2;
    queue_init(&q1);
    queue_init(&q2);

    printf("=================================================================\n");
    printf("PTHREAD CONTENTION BASELINE\n");
    printf("=================================================================\n\n");

    double baseline_times[NUM_TRIALS];
    double contention_times[NUM_TRIALS];

    for (int trial = 1; trial <= NUM_TRIALS; trial++) {
        double start = time_now_ms();
        pthread_t t1, t2;
        pthread_create(&t1, NULL, producer_thread, &q1);
        pthread_create(&t2, NULL, consumer_thread, &q1);
        pthread_join(t1, NULL);
        long long* cs1 = NULL;
        pthread_join(t2, (void**)&cs1);
        g_sink += *cs1; free(cs1);
        baseline_times[trial - 1] = time_now_ms() - start;

        start = time_now_ms();
        pthread_t t3, t4, t5, t6;
        pthread_create(&t3, NULL, producer_thread, &q1);
        pthread_create(&t4, NULL, consumer_thread, &q1);
        pthread_create(&t5, NULL, producer_thread, &q2);
        pthread_create(&t6, NULL, consumer_thread, &q2);
        pthread_join(t3, NULL);
        long long* cs2 = NULL;
        pthread_join(t4, (void**)&cs2);
        pthread_join(t5, NULL);
        long long* cs3 = NULL;
        pthread_join(t6, (void**)&cs3);
        g_sink += *cs2 + *cs3; free(cs2); free(cs3);
        contention_times[trial - 1] = time_now_ms() - start;

        printf("  Trial %d:  baseline=%6.2f ms  contention=%6.2f ms\n",
               trial, baseline_times[trial - 1], contention_times[trial - 1]);
    }

    double best_baseline   = baseline_times[0];
    double best_contention = contention_times[0];
    for (int i = 1; i < NUM_TRIALS; i++) {
        if (baseline_times[i]   < best_baseline)   best_baseline   = baseline_times[i];
        if (contention_times[i] < best_contention) best_contention = contention_times[i];
    }

    double interference = (best_contention - best_baseline) / best_baseline * 100.0;

    printf("\n");
    printf("  Best baseline:    %6.2f ms  (%8.0f ops/sec)\n",
           best_baseline, (double)ITERATIONS * 1000.0 / best_baseline);
    printf("  Best contention:  %6.2f ms  (%8.0f ops/sec per channel)\n",
           best_contention, (double)ITERATIONS * 1000.0 / best_contention);
    printf("\n");
    printf("Interference: %.2f%%  (best-of-%d)\n", interference, NUM_TRIALS);

    return 0;
}
