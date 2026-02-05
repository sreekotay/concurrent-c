/*
 * pthread_contention_baseline.c - Pthread-based contention test
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#define ITERATIONS 1000000
#define QUEUE_SIZE 1024

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
    for (int i = 0; i < ITERATIONS; i++) queue_push(q, i);
    return NULL;
}

void* consumer_thread(void* arg) {
    SimpleQueue* q = (SimpleQueue*)arg;
    for (int i = 0; i < ITERATIONS; i++) queue_pop(q);
    return NULL;
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

    for (int trial = 1; trial <= 3; trial++) {
        printf("Trial %d:\n", trial);

        double start = time_now_ms();
        pthread_t t1, t2;
        pthread_create(&t1, NULL, producer_thread, &q1);
        pthread_create(&t2, NULL, consumer_thread, &q1);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
        double baseline_ms = time_now_ms() - start;
        printf("  Baseline (Q1 only):   %8.2f ms\n", baseline_ms);

        start = time_now_ms();
        pthread_t t3, t4, t5, t6;
        pthread_create(&t3, NULL, producer_thread, &q1);
        pthread_create(&t4, NULL, consumer_thread, &q1);
        pthread_create(&t5, NULL, producer_thread, &q2);
        pthread_create(&t6, NULL, consumer_thread, &q2);
        pthread_join(t3, NULL);
        pthread_join(t4, NULL);
        pthread_join(t5, NULL);
        pthread_join(t6, NULL);
        double contention_ms = time_now_ms() - start;
        printf("  Contention (Q1+Q2):  %8.2f ms (%8.0f ops/sec total)\n", 
               contention_ms, (double)ITERATIONS * 2.0 * 1000.0 / contention_ms);
        
        double baseline_ops_sec = (double)ITERATIONS * 1000.0 / baseline_ms;
        double contention_ops_sec = (double)ITERATIONS * 2.0 * 1000.0 / contention_ms;
        double throughput_drop = (baseline_ops_sec - contention_ops_sec) / baseline_ops_sec * 100.0;
        printf("  Throughput Drop:      %8.2f%%\n\n", throughput_drop);
    }

    return 0;
}
