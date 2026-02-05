/*
 * pthread_noisy_baseline.c - Pthread-based baseline for the Noisy Neighbor test
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>

#define NUM_THREADS 16
#define NUM_HOGS 15
#define HEARTBEAT_INTERVAL_MS 100000 // 100ms in microseconds
#define TEST_DURATION_SEC 5

atomic_int g_heartbeats = 0;
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

void* hog_thread(void* arg) {
    int id = *(int*)arg;
    free(arg);
    printf("[Hog %d] Started CPU-intensive loop (Pthread)...\n", id);
    
    volatile double x = 1.1;
    while (!atomic_load(&g_stop)) {
        for (int i = 0; i < 1000000; i++) {
            x = x * x;
            if (x > 1000000.0) x = 1.1;
        }
    }
    printf("[Hog %d] Stopped\n", id);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=================================================================\n");
    printf("PTHREAD BASELINE: OS preemption against CPU hogs\n");
    printf("Threads: %d | CPU Hogs: %d\n", NUM_THREADS, NUM_HOGS);
    printf("=================================================================\n\n");

    pthread_t heartbeat;
    pthread_create(&heartbeat, NULL, heartbeat_thread, NULL);

    sleep(1); 
    printf("Initial heartbeats: %d\n", atomic_load(&g_heartbeats));

    printf("\n!!! Unleashing CPU Hogs !!!\n");
    pthread_t hogs[NUM_HOGS];
    for (int i = 0; i < NUM_HOGS; i++) {
        int* id = malloc(sizeof(int));
        *id = i;
        pthread_create(&hogs[i], NULL, hog_thread, id);
    }

    sleep(TEST_DURATION_SEC);

    printf("\nStopping test...\n");
    atomic_store(&g_stop, 1);
    
    pthread_join(heartbeat, NULL);
    for (int i = 0; i < NUM_HOGS; i++) {
        pthread_join(hogs[i], NULL);
    }

    printf("\n=================================================================\n");
    printf("FINAL RESULTS (Pthread)\n");
    printf("Total Heartbeats: %d\n", atomic_load(&g_heartbeats));
    printf("=================================================================\n");

    return 0;
}
