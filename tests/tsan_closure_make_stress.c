/*
 * TSan stress: concurrent closure make helpers
 *
 * Validates that cc_closure*_make only touch thread-local stack state.
 * With TSan enabled, this should report NO races.
 *
 * Run with:
 *   clang -fsanitize=thread -g -Icc/include tests/tsan_closure_make_stress.c -lpthread
 */
#include <ccc/cc_closure.cch>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREADS 10
#define ITERS 50

static void* noop0(void* env) { (void)env; return NULL; }
static void* noop1(void* env, intptr_t arg0) { (void)env; (void)arg0; return NULL; }
static void* noop2(void* env, intptr_t arg0, intptr_t arg1) { (void)env; (void)arg0; (void)arg1; return NULL; }

typedef struct {
    int id;
    int sum;
} Worker;

static void* worker_main(void* arg) {
    Worker* w = (Worker*)arg;
    int local = 0;
    for (int i = 0; i < ITERS; i++) {
        int a = w->id + i;
        int b = w->id * 2 + i;
        CCClosure0 c0 = cc_closure0_make(noop0, NULL, NULL);
        CCClosure1 c1 = cc_closure1_make(noop1, (void*)(intptr_t)a, NULL);
        CCClosure2 c2 = cc_closure2_make(noop2, (void*)(intptr_t)b, NULL);
        /* Touch fields to prevent the compiler from optimizing away */
        if (c0.fn) local += a;
        if (c1.fn) local += b;
        if (c2.fn) local += (a + b);
    }
    w->sum = local;
    return NULL;
}

int main(void) {
    pthread_t th[THREADS];
    Worker workers[THREADS];
    for (int i = 0; i < THREADS; i++) {
        workers[i].id = i;
        workers[i].sum = 0;
        if (pthread_create(&th[i], NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    int total = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        total += workers[i].sum;
    }
    if (total == 0) {
        fprintf(stderr, "FAIL: unexpected total=0\n");
        return 1;
    }
    printf("tsan_closure_make_stress: PASS (total=%d)\n", total);
    return 0;
}
