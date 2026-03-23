/*
 * pthread_contention_baseline.c - Shared-channel N x M contention test
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define DEFAULT_MESSAGES   1000000
#define DEFAULT_NUM_TRIALS 15
#define DEFAULT_PRODUCERS  8
#define DEFAULT_CONSUMERS  8
#define QUEUE_SIZE         1024

static volatile long long g_sink = 0;

static int env_int_or_default(const char* name, int fallback, int min_value) {
    const char* v = getenv(name);
    if (!v || !v[0]) return fallback;
    int parsed = atoi(v);
    return parsed < min_value ? min_value : parsed;
}

static int bench_messages(void) {
    static int value = -1;
    if (value < 0) value = env_int_or_default("CC_CONTENTION_ITERATIONS", DEFAULT_MESSAGES, 1);
    return value;
}

static int bench_trials(void) {
    static int value = -1;
    if (value < 0) value = env_int_or_default("CC_CONTENTION_TRIALS", DEFAULT_NUM_TRIALS, 1);
    return value;
}

static int bench_producers(void) {
    static int value = -1;
    if (value < 0) value = env_int_or_default("CC_CONTENTION_PRODUCERS", DEFAULT_PRODUCERS, 1);
    return value;
}

static int bench_consumers(void) {
    static int value = -1;
    if (value < 0) value = env_int_or_default("CC_CONTENTION_CONSUMERS", DEFAULT_CONSUMERS, 1);
    return value;
}

static int work_share(int total, int idx, int workers) {
    int base = total / workers;
    int rem = total % workers;
    return base + (idx < rem ? 1 : 0);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int data[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    int closed;
} SimpleQueue;

typedef struct {
    SimpleQueue* q;
    _Atomic int* producers_left;
    int producer_id;
    int send_count;
} ProducerCtx;

typedef struct {
    SimpleQueue* q;
    long long sum;
} ConsumerCtx;

static void queue_init(SimpleQueue* q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->head = q->tail = q->count = 0;
    q->closed = 0;
}

static void queue_destroy(SimpleQueue* q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static void queue_close(SimpleQueue* q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
}

static void queue_push(SimpleQueue* q, int v) {
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

static int queue_pop(SimpleQueue* q, int* out) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) {
        pthread_cond_wait(&q->not_empty, &q->mu);
    }
    if (q->count == 0 && q->closed) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    *out = q->data[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static void* producer_thread(void* arg) {
    ProducerCtx* ctx = (ProducerCtx*)arg;
    for (int i = 0; i < ctx->send_count; i++) {
        queue_push(ctx->q, (ctx->producer_id + 1) ^ (i << 1) ^ (i >> 16));
    }
    if (atomic_fetch_sub(ctx->producers_left, 1) == 1) {
        queue_close(ctx->q);
    }
    return NULL;
}

static void* consumer_thread(void* arg) {
    ConsumerCtx* ctx = (ConsumerCtx*)arg;
    long long sum = 0;
    int v = 0;
    while (queue_pop(ctx->q, &v)) {
        sum += (long long)v;
    }
    ctx->sum = sum;
    return NULL;
}

static double time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static double run_shared_case(int producers, int consumers) {
    int messages = bench_messages();
    SimpleQueue q;
    queue_init(&q);

    pthread_t* producer_threads = (pthread_t*)calloc((size_t)producers, sizeof(pthread_t));
    pthread_t* consumer_threads = (pthread_t*)calloc((size_t)consumers, sizeof(pthread_t));
    ProducerCtx* producer_ctxs = (ProducerCtx*)calloc((size_t)producers, sizeof(ProducerCtx));
    ConsumerCtx* consumer_ctxs = (ConsumerCtx*)calloc((size_t)consumers, sizeof(ConsumerCtx));
    _Atomic int producers_left = producers;
    if (!producer_threads || !consumer_threads || !producer_ctxs || !consumer_ctxs) abort();

    double start = time_now_ms();
    for (int c = 0; c < consumers; c++) {
        consumer_ctxs[c].q = &q;
        consumer_ctxs[c].sum = 0;
        pthread_create(&consumer_threads[c], NULL, consumer_thread, &consumer_ctxs[c]);
    }
    for (int p = 0; p < producers; p++) {
        producer_ctxs[p].q = &q;
        producer_ctxs[p].producers_left = &producers_left;
        producer_ctxs[p].producer_id = p;
        producer_ctxs[p].send_count = work_share(messages, p, producers);
        pthread_create(&producer_threads[p], NULL, producer_thread, &producer_ctxs[p]);
    }

    for (int p = 0; p < producers; p++) {
        pthread_join(producer_threads[p], NULL);
    }
    for (int c = 0; c < consumers; c++) {
        pthread_join(consumer_threads[c], NULL);
        g_sink += consumer_ctxs[c].sum;
    }
    double elapsed = time_now_ms() - start;

    free(producer_threads);
    free(consumer_threads);
    free(producer_ctxs);
    free(consumer_ctxs);
    queue_destroy(&q);
    return elapsed;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int messages = bench_messages();
    int trials = bench_trials();
    int producers = bench_producers();
    int consumers = bench_consumers();

    printf("=================================================================\n");
    printf("PTHREAD SHARED CHANNEL CONTENTION\n");
    printf("Messages: %d | Trials: %d | Contention: %dx%d\n", messages, trials, producers, consumers);
    printf("=================================================================\n\n");

    double* baseline_times = (double*)calloc((size_t)trials, sizeof(double));
    double* contention_times = (double*)calloc((size_t)trials, sizeof(double));
    if (!baseline_times || !contention_times) abort();

    for (int trial = 1; trial <= trials; trial++) {
        baseline_times[trial - 1] = run_shared_case(1, 1);
        contention_times[trial - 1] = run_shared_case(producers, consumers);
        printf("  Trial %d:  baseline=%6.2f ms  contention=%6.2f ms\n",
               trial, baseline_times[trial - 1], contention_times[trial - 1]);
    }

    double best_baseline = baseline_times[0];
    double best_contention = contention_times[0];
    for (int i = 1; i < trials; i++) {
        if (baseline_times[i] < best_baseline) best_baseline = baseline_times[i];
        if (contention_times[i] < best_contention) best_contention = contention_times[i];
    }

    double interference = (best_contention - best_baseline) / best_baseline * 100.0;

    printf("\n");
    printf("  Best baseline:    %6.2f ms  (%8.0f msgs/sec)\n",
           best_baseline, (double)messages * 1000.0 / best_baseline);
    printf("  Best contention:  %6.2f ms  (%8.0f msgs/sec)\n",
           best_contention, (double)messages * 1000.0 / best_contention);
    printf("\n");
    printf("Interference: %.2f%%  (best-of-%d)\n", interference, trials);

    free(baseline_times);
    free(contention_times);
    return 0;
}
