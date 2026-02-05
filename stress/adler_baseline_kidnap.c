/*
 * adler_baseline_kidnap.c - Pthread-based baseline for the kidnapping test
 * 
 * This uses standard pthreads to show how a "traditional" Adler-style 
 * implementation handles the same kidnapping scenario.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#define NUM_THREADS 16
#define NUM_KIDNAPPERS 16
#define HEARTBEAT_INTERVAL_MS 100000 // 100ms in microseconds
#define TEST_DURATION_SEC 3

atomic_int g_heartbeats = 0;
atomic_int g_kidnappers_active = 0;
atomic_int g_stop = 0;

void* heartbeat_thread(void* arg) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[Heartbeat] Started (Pthread)\n");
    while (!atomic_load(&g_stop)) {
        usleep(HEARTBEAT_INTERVAL_MS);
        int val = atomic_fetch_add(&g_heartbeats, 1) + 1;
        printf("[Heartbeat] Tick %d\n", val);
    }
    return NULL;
}

void* kidnapper_thread(void* arg) {
    int id = *(int*)arg;
    free(arg);
    atomic_fetch_add(&g_kidnappers_active, 1);
    printf("[Kidnapper %d] Blocking thread for 2 seconds...\n", id);
    sleep(2);
    printf("[Kidnapper %d] Released thread\n", id);
    atomic_fetch_sub(&g_kidnappers_active, 1);
    return NULL;
}

int main(void) {
    printf("=================================================================\n");
    printf("ADLER BASELINE: Pthread robustness against blocking IO\n");
    printf("Threads: %d | Kidnappers: %d\n", NUM_THREADS, NUM_KIDNAPPERS);
    printf("=================================================================\n\n");

    pthread_t heartbeat;
    pthread_create(&heartbeat, NULL, heartbeat_thread, NULL);

    sleep(1); // Wait for healthy start
    printf("Initial heartbeats: %d\n", atomic_load(&g_heartbeats));

    printf("\n!!! Unleashing Kidnappers !!!\n");
    pthread_t kidnappers[NUM_KIDNAPPERS];
    for (int i = 0; i < NUM_KIDNAPPERS; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&kidnappers[i], NULL, kidnapper_thread, id);
    }

    for (int i = 0; i < TEST_DURATION_SEC; i++) {
        sleep(1);
        printf("T+%ds: Heartbeats=%d | Active Kidnappers=%d\n", 
               i+1, atomic_load(&g_heartbeats), atomic_load(&g_kidnappers_active));
    }

    atomic_store(&g_stop, 1);
    pthread_join(heartbeat, NULL);
    for (int i = 0; i < NUM_KIDNAPPERS; i++) {
        pthread_join(kidnappers[i], NULL);
    }

    printf("\n=================================================================\n");
    printf("FINAL RESULTS (Pthread)\n");
    printf("Total Heartbeats: %d\n", atomic_load(&g_heartbeats));
    printf("=================================================================\n");

    return 0;
}
