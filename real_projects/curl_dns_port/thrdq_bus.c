/* Isolated stampede: N workers, optional usleep, then done-push.
 * Repro for SIGBUS in cc__exclusive_lock_entry. */
#include "cc_thrdqueue.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int items[32];
static atomic_int g_done;
static int g_sleep_us;

static void process(void *item) {
    (void)item;
    if (g_sleep_us)
        usleep((useconds_t)g_sleep_us);
    atomic_fetch_add(&g_done, 1);
}

static void freen(void *item) { (void)item; }

int main(int argc, char **argv) {
    struct curl_thrdq *q = NULL;
    int n = argc > 1 ? atoi(argv[1]) : 8;
    int sleep_us = argc > 2 ? atoi(argv[2]) : 20000;
    int i, rc;

    g_sleep_us = sleep_us;
    atomic_store(&g_done, 0);
    rc = Curl_thrdq_create(&q, "bus", 0, 0, (uint32_t)n, 2000, freen, process,
                           NULL, NULL);
    if (rc || !q) {
        fprintf(stderr, "create rc=%d\n", rc);
        return 1;
    }
    for (i = 0; i < n; i++) {
        items[i] = i;
        if (Curl_thrdq_send(q, &items[i], "x", 0))
            return 2;
    }
    for (i = 0; i < 400 && atomic_load(&g_done) < n; i++)
        usleep(5000);
    fprintf(stderr, "done=%d/%d sleep_us=%d\n", atomic_load(&g_done), n,
            sleep_us);
    Curl_thrdq_destroy(q, 1);
    return atomic_load(&g_done) == n ? 0 : 3;
}
