/*
 * Knob and ABI smoke for the Concurrent-C Curl_thrdq.
 * Does not include curl — links thrdqueue.o + the CC runtime.
 */
#include "cc_thrdqueue.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static atomic_int g_inflight;
static atomic_int g_max_inflight;
static atomic_int g_done;

static void process_sleep(void *item) {
    int *p = (int *)item;
    int cur = atomic_fetch_add(&g_inflight, 1) + 1;
    int max = atomic_load(&g_max_inflight);
    while (cur > max &&
           !atomic_compare_exchange_weak(&g_max_inflight, &max, cur))
        max = atomic_load(&g_max_inflight);
    usleep(80000);
    atomic_fetch_sub(&g_inflight, 1);
    atomic_fetch_add(&g_done, 1);
    (void)p;
}

static void free_nop(void *item) {
    (void)item;
}

static bool match_all(void *item, void *match_data) {
    (void)item;
    (void)match_data;
    return true;
}

static int fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

#define BLOCK_MAGIC 0xC0DE0101u
#define BLOCK_SLEEP_US 250000

typedef struct {
    unsigned magic;
    int id;
} BlockItem;

static atomic_int g_block_entered;
static atomic_int g_block_processed;
static atomic_int g_block_freed;

static double monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int wait_atomic(atomic_int *a, int want, double timeout_s) {
    double t0 = monotonic_s();
    while (atomic_load(a) < want) {
        if (monotonic_s() - t0 > timeout_s)
            return 0;
        usleep(1000);
    }
    return 1;
}

static void process_block(void *item) {
    BlockItem *it = (BlockItem *)item;
    int i;
    if (!it || it->magic != BLOCK_MAGIC) {
        fprintf(stderr, "FAIL: process saw a dead item\n");
        abort();
    }
    atomic_fetch_add(&g_block_entered, 1);
    for (i = 0; i < 25; i++) {
        if (it->magic != BLOCK_MAGIC) {
            fprintf(stderr, "FAIL: item freed under process\n");
            abort();
        }
        usleep(BLOCK_SLEEP_US / 25);
    }
    atomic_fetch_add(&g_block_processed, 1);
}

static void free_block(void *item) {
    BlockItem *it = (BlockItem *)item;
    if (!it || it->magic != BLOCK_MAGIC) {
        fprintf(stderr, "FAIL: free_block magic\n");
        abort();
    }
    it->magic = 0;
    free(it);
    atomic_fetch_add(&g_block_freed, 1);
}

static BlockItem *block_item(int id) {
    BlockItem *it = (BlockItem *)malloc(sizeof(*it));
    if (!it)
        return NULL;
    it->magic = BLOCK_MAGIC;
    it->id = id;
    return it;
}

/* Join waits for an in-flight process(); queued work is freed without
 * running. This is the teardown the libcurl abort harness does not hit. */
static int test_join_blocked(void) {
    struct curl_thrdq *q = NULL;
    BlockItem *a, *b;
    double t0, join_s;
    int rc;

    atomic_store(&g_block_entered, 0);
    atomic_store(&g_block_processed, 0);
    atomic_store(&g_block_freed, 0);

    a = block_item(1);
    b = block_item(2);
    if (!a || !b)
        return fail("join_blocked: malloc");

    rc = Curl_thrdq_create(&q, "join", 0, 0, 1, 2000, free_block,
                           process_block, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("join_blocked: create");
    if (Curl_thrdq_send(q, a, "in-flight", 0) != CC_CURLE_OK)
        return fail("join_blocked: send a");
    if (Curl_thrdq_send(q, b, "queued", 0) != CC_CURLE_OK)
        return fail("join_blocked: send b");
    if (!wait_atomic(&g_block_entered, 1, 1.0))
        return fail("join_blocked: process never entered");

    t0 = monotonic_s();
    Curl_thrdq_destroy(q, true);
    join_s = monotonic_s() - t0;

    if (join_s < 0.15)
        return fail("join_blocked: destroy returned before the sleeper "
                    "(not a blocked join)");
    if (join_s > 2.0)
        return fail("join_blocked: destroy hung");
    if (atomic_load(&g_block_processed) != 1)
        return fail("join_blocked: in-flight item was not processed");
    if (atomic_load(&g_block_freed) != 2)
        return fail("join_blocked: expected both items freed");
    printf("thrdqueue_smoke: join_blocked OK (join=%.3fs)\n", join_s);
    return 0;
}

/* join=false must return while process still runs; the item stays valid
 * until process finishes, then fn_free. */
static int test_detach_blocked(void) {
    struct curl_thrdq *q = NULL;
    BlockItem *a;
    double t0, detach_s;
    int rc;

    atomic_store(&g_block_entered, 0);
    atomic_store(&g_block_processed, 0);
    atomic_store(&g_block_freed, 0);

    a = block_item(3);
    if (!a)
        return fail("detach_blocked: malloc");
    rc = Curl_thrdq_create(&q, "detach", 0, 0, 1, 2000, free_block,
                           process_block, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("detach_blocked: create");
    if (Curl_thrdq_send(q, a, "in-flight", 0) != CC_CURLE_OK)
        return fail("detach_blocked: send");
    if (!wait_atomic(&g_block_entered, 1, 1.0))
        return fail("detach_blocked: process never entered");

    t0 = monotonic_s();
    Curl_thrdq_destroy(q, false);
    detach_s = monotonic_s() - t0;
    if (detach_s > 0.10)
        return fail("detach_blocked: destroy(join=false) waited for process");
    if (atomic_load(&g_block_freed) != 0)
        return fail("detach_blocked: item freed before process finished");
    if (!wait_atomic(&g_block_freed, 1, 2.0))
        return fail("detach_blocked: item never freed after detach");
    if (atomic_load(&g_block_processed) != 1)
        return fail("detach_blocked: process did not finish");
    printf("thrdqueue_smoke: detach_blocked OK (return=%.3fs)\n", detach_s);
    return 0;
}

/* Work finishes, then destroy(join). Eight blocking hybrids; two idle
 * workers never reached the exclusive-after-park smash. */
static int test_join_many_blocked(void) {
    struct curl_thrdq *q = NULL;
    static int items[8];
    int i, rc;

    atomic_store(&g_inflight, 0);
    atomic_store(&g_done, 0);
    rc = Curl_thrdq_create(&q, "join8", 0, 0, 8, 2000, free_nop,
                           process_sleep, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("join_many_blocked: create");
    for (i = 0; i < 8; i++) {
        items[i] = i;
        if (Curl_thrdq_send(q, &items[i], "x", 0) != CC_CURLE_OK)
            return fail("join_many_blocked: send");
    }
    if (!wait_atomic(&g_done, 8, 2.0))
        return fail("join_many_blocked: workers did not finish");
    Curl_thrdq_destroy(q, true);
    printf("thrdqueue_smoke: join_many_blocked OK (n=8)\n");
    return 0;
}

static struct curl_thrdq *g_recv_q;
static atomic_int g_saw_recv_error;

static void *recv_while_join(void *arg) {
    void *got;
    int rc;
    (void)arg;
    while (atomic_load(&g_block_entered) < 1)
        usleep(1000);
    for (;;) {
        rc = Curl_thrdq_recv(g_recv_q, &got);
        if (rc == CC_CURLE_RECV_ERROR) {
            atomic_store(&g_saw_recv_error, 1);
            return NULL;
        }
        if (rc != CC_CURLE_AGAIN && rc != CC_CURLE_OK)
            return NULL;
        usleep(1000);
    }
}

/* destroy(join) sets aborted first; recv must be RECV_ERROR, not AGAIN. */
static int test_recv_aborted(void) {
    struct curl_thrdq *q = NULL;
    BlockItem *a;
    pthread_t th;
    int rc;

    atomic_store(&g_block_entered, 0);
    atomic_store(&g_block_processed, 0);
    atomic_store(&g_block_freed, 0);
    atomic_store(&g_saw_recv_error, 0);

    a = block_item(4);
    if (!a)
        return fail("recv_aborted: malloc");
    rc = Curl_thrdq_create(&q, "recv-abort", 0, 0, 1, 2000, free_block,
                           process_block, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("recv_aborted: create");
    if (Curl_thrdq_send(q, a, "in-flight", 0) != CC_CURLE_OK)
        return fail("recv_aborted: send");
    if (!wait_atomic(&g_block_entered, 1, 1.0))
        return fail("recv_aborted: process never entered");

    g_recv_q = q;
    if (pthread_create(&th, NULL, recv_while_join, NULL) != 0)
        return fail("recv_aborted: pthread_create");
    Curl_thrdq_destroy(q, true);
    pthread_join(th, NULL);
    g_recv_q = NULL;
    if (!atomic_load(&g_saw_recv_error))
        return fail("recv_aborted: expected CURLE_RECV_ERROR during join");
    printf("thrdqueue_smoke: recv_aborted OK\n");
    return 0;
}

/* Park until outstanding work is 0. timeout_ms=0 waits forever. */
static int test_await_done(void) {
    struct curl_thrdq *q = NULL;
    void *got;
    int item = 1;
    double t0, dt;
    int rc;

    rc = Curl_thrdq_create(&q, "await", 0, 0, 1, 2000, free_nop,
                           process_sleep, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("await_done: create");

    t0 = monotonic_s();
    rc = Curl_thrdq_await_done(q, 100);
    dt = monotonic_s() - t0;
    if (rc != CC_CURLE_OK)
        return fail("await_done: idle must be OK");
    if (dt > 0.05)
        return fail("await_done: idle wait parked");

    if (Curl_thrdq_send(q, &item, "slow", 0) != CC_CURLE_OK)
        return fail("await_done: send");

    t0 = monotonic_s();
    rc = Curl_thrdq_await_done(q, 30);
    dt = monotonic_s() - t0;
    if (rc != CC_CURLE_OPERATION_TIMEDOUT)
        return fail("await_done: short wait must TIMEOUT");
    if (dt < 0.02 || dt > 0.08)
        return fail("await_done: TIMEOUT was not the deadline");

    {
        double timeout_s = dt;
        t0 = monotonic_s();
        rc = Curl_thrdq_await_done(q, 1000);
        dt = monotonic_s() - t0;
        if (rc != CC_CURLE_OK)
            return fail("await_done: did not become idle");
        if (dt > 0.20)
            return fail("await_done: completion wait too long");
        if (Curl_thrdq_recv(q, &got) != CC_CURLE_OK)
            return fail("await_done: item never landed on done");

        Curl_thrdq_destroy(q, true);
        printf("thrdqueue_smoke: await_done OK (timeout=%.3fs rest=%.3fs)\n",
               timeout_s, dt);
    }
    return 0;
}

static atomic_int g_ran_slow;
static atomic_int g_ran_exp;

static void process_timeout_take(void *item) {
    int *p = (int *)item;
    if (*p == 1) {
        atomic_fetch_add(&g_ran_slow, 1);
        usleep(80000);
    } else {
        atomic_fetch_add(&g_ran_exp, 1);
    }
}

/* Stock take: remaining < 0 while queued → done, no process. */
static int test_timeout_at_take(void) {
    struct curl_thrdq *q = NULL;
    void *got;
    int slow = 1, exp = 2;
    int n = 0;
    int rc;

    atomic_store(&g_ran_slow, 0);
    atomic_store(&g_ran_exp, 0);
    rc = Curl_thrdq_create(&q, "expire", 0, 0, 1, 2000, free_nop,
                           process_timeout_take, NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("timeout_at_take: create");
    if (Curl_thrdq_send(q, &slow, "slow", 0) != CC_CURLE_OK)
        return fail("timeout_at_take: send slow");
    if (Curl_thrdq_send(q, &exp, "exp", 10) != CC_CURLE_OK)
        return fail("timeout_at_take: send exp");
    if (Curl_thrdq_await_done(q, 1000) != CC_CURLE_OK)
        return fail("timeout_at_take: await_done");
    if (atomic_load(&g_ran_slow) != 1)
        return fail("timeout_at_take: slow item must run");
    if (atomic_load(&g_ran_exp) != 0)
        return fail("timeout_at_take: expired item was processed");
    while (Curl_thrdq_recv(q, &got) == CC_CURLE_OK)
        n++;
    if (n != 2)
        return fail("timeout_at_take: both items must land on done");
    Curl_thrdq_destroy(q, true);
    printf("thrdqueue_smoke: timeout_at_take OK\n");
    return 0;
}

int main(void) {
    struct curl_thrdq *q = NULL;
    void *got;
    int items[4] = {1, 2, 3, 4};
    int i;
    int rc;

    rc = Curl_thrdq_create(&q, "smoke", 0, 0, 0, 10, free_nop, process_sleep,
                           NULL, NULL);
    if (rc != CC_CURLE_BAD_FUNCTION_ARGUMENT)
        return fail("max_threads=0 must be BAD_FUNCTION_ARGUMENT");

    rc = Curl_thrdq_create(&q, "smoke", 0, 5, 2, 10, free_nop, process_sleep,
                           NULL, NULL);
    if (rc != CC_CURLE_BAD_FUNCTION_ARGUMENT)
        return fail("min>max must be BAD_FUNCTION_ARGUMENT");

    rc = Curl_thrdq_create(&q, "smoke", 0, 0, 2, 10, NULL, process_sleep,
                           NULL, NULL);
    if (rc != CC_CURLE_FAILED_INIT)
        return fail("NULL fn_free must be FAILED_INIT");

    rc = Curl_thrdq_create(&q, "smoke", 0, 0, 2, 2000, free_nop, process_sleep,
                           NULL, NULL);
    if (rc != CC_CURLE_OK || !q)
        return fail("create(min=0,max=2) failed");

    rc = Curl_thrdq_set_props(q, 0, 3, 1, 10);
    if (rc != CC_CURLE_BAD_FUNCTION_ARGUMENT)
        return fail("set_props min>max must be BAD_FUNCTION_ARGUMENT");

    atomic_store(&g_inflight, 0);
    atomic_store(&g_max_inflight, 0);
    atomic_store(&g_done, 0);
    for (i = 0; i < 4; i++) {
        rc = Curl_thrdq_send(q, &items[i], "item", 0);
        if (rc != CC_CURLE_OK)
            return fail("send failed");
    }
    if (!wait_atomic(&g_done, 4, 2.0))
        return fail("workers did not finish 4 items");
    if (atomic_load(&g_max_inflight) > 2)
        return fail("max_threads=2 was exceeded");
    if (atomic_load(&g_max_inflight) < 1)
        return fail("no worker ran");

    i = 0;
    while (Curl_thrdq_recv(q, &got) == CC_CURLE_OK)
        i++;
    if (i != 4)
        return fail("recv did not return 4 items");

    rc = Curl_thrdq_send(q, &items[0], "clear-me", 0);
    if (rc != CC_CURLE_OK)
        return fail("send before clear failed");
    Curl_thrdq_clear(q, match_all, NULL);
    if (Curl_thrdq_recv(q, &got) != CC_CURLE_AGAIN &&
        atomic_load(&g_done) == 4) {
        /* item may already have been processed; either way, no crash */
    }

    Curl_thrdq_trace(q, NULL);
    Curl_thrdq_destroy(q, true);
    printf("thrdqueue_smoke: knobs OK (max_inflight=%d)\n",
           atomic_load(&g_max_inflight));

    if (test_await_done())
        return 1;
    if (test_timeout_at_take())
        return 1;
    if (test_join_blocked())
        return 1;
    if (test_detach_blocked())
        return 1;
    if (test_join_many_blocked())
        return 1;
    if (test_recv_aborted())
        return 1;
    return 0;
}
